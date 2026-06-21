#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <plume_render_interface.h>
#include <zstd.h>

#ifdef _WIN32
#include <windows.h>
#include <objidl.h>
#include <unknwn.h>
#include <dxcapi.h>
#endif

#include "generated/shader_cache.h"
#include "src/goliath_engine/gpu/renderer/video.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;
using namespace plume;

namespace {

const ShaderCacheEntry* FindShaderCacheEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry* it = std::lower_bound(
      begin, end, hash,
      [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

std::unique_ptr<uint8_t[]> g_dxilCache;
std::once_flag g_dxilCacheOnce;
void EnsureDxilCache() {
  std::call_once(g_dxilCacheOnce, [] {
    g_dxilCache = std::make_unique<uint8_t[]>(g_dxilCacheDecompressedSize);
    size_t r = ZSTD_decompress(g_dxilCache.get(), g_dxilCacheDecompressedSize,
                               g_compressedDxilCache, g_dxilCacheCompressedSize);
    if (ZSTD_isError(r)) {
      REXGPU_ERROR("shader cache: ZSTD_decompress failed: {}", ZSTD_getErrorName(r));
      g_dxilCache.reset();
    } else {
      REXGPU_INFO("shader cache: DXIL decompressed {} -> {} bytes", g_dxilCacheCompressedSize,
                  g_dxilCacheDecompressedSize);
    }
  });
}

#ifdef _WIN32
class DxcRuntime {
 public:
  DxcRuntime() {
    library_ = LoadLibraryW(L"dxcompiler.dll");
    if (!library_) { REXGPU_ERROR("DXC: failed to load dxcompiler.dll"); return; }
    createInstance_ =
        reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(library_, "DxcCreateInstance"));
    if (!createInstance_) { REXGPU_ERROR("DXC: no DxcCreateInstance"); return; }
    if (FAILED(createInstance_(CLSID_DxcCompiler, __uuidof(IDxcCompiler3),
                               reinterpret_cast<void**>(&compiler_))) || !compiler_) {
      REXGPU_ERROR("DXC: no IDxcCompiler3"); return;
    }
    if (FAILED(createInstance_(CLSID_DxcUtils, __uuidof(IDxcUtils),
                               reinterpret_cast<void**>(&utils_))) || !utils_) {
      REXGPU_ERROR("DXC: no IDxcUtils"); return;
    }
  }
  bool ready() const { return compiler_ && utils_; }

  IDxcBlob* Link(const void* dxil, uint32_t dxilSize, uint32_t dxilOffset, bool isVS,
                 uint32_t specConstants) {
    if (!ready()) return nullptr;
    IDxcBlob* specBlob = GetSpecLib(specConstants);
    if (!specBlob) return nullptr;

    IDxcBlobEncoding* shaderBlob = nullptr;
    if (FAILED(utils_->CreateBlobFromPinned(dxil, dxilSize, DXC_CP_ACP, &shaderBlob)) ||
        !shaderBlob) {
      specBlob->Release();
      return nullptr;
    }

    IDxcLinker* linker = nullptr;
    if (FAILED(createInstance_(CLSID_DxcLinker, __uuidof(IDxcLinker),
                               reinterpret_cast<void**>(&linker))) || !linker) {
      specBlob->Release(); shaderBlob->Release(); return nullptr;
    }
    wchar_t specName[64], shaderName[64];
    swprintf_s(specName, L"SpecConstants_%u", specConstants);
    swprintf_s(shaderName, L"Shader_%u", dxilOffset);
    bool reg = SUCCEEDED(linker->RegisterLibrary(specName, specBlob)) &&
               SUCCEEDED(linker->RegisterLibrary(shaderName, shaderBlob));
    specBlob->Release();
    shaderBlob->Release();
    if (!reg) { linker->Release(); return nullptr; }

    const wchar_t* libs[] = {specName, shaderName};
    IDxcOperationResult* link = nullptr;
    HRESULT hr = linker->Link(L"main", isVS ? L"vs_6_0" : L"ps_6_0", libs,
                              std::size(libs), nullptr, 0, &link);
    linker->Release();
    if (FAILED(hr) || !link) return nullptr;
    HRESULT status = E_FAIL;
    link->GetStatus(&status);
    IDxcBlob* out = nullptr;
    if (SUCCEEDED(status)) link->GetResult(&out);
    else {
      IDxcBlobEncoding* err = nullptr;
      if (SUCCEEDED(link->GetErrorBuffer(&err)) && err) {
        std::string msg(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        REXGPU_ERROR("DXC link failed: {}", msg);
        err->Release();
      }
    }
    link->Release();
    return out;
  }

 private:
  IDxcBlob* GetSpecLib(uint32_t specConstants) {
    std::lock_guard lock(mutex_);
    auto it = specLibs_.find(specConstants);
    if (it != specLibs_.end()) { it->second->AddRef(); return it->second; }
    char src[128];
    int n = std::snprintf(src, sizeof(src),
                          "export uint g_SpecConstants() { return %u; }", specConstants);
    DxcBuffer buf{src, static_cast<size_t>(n), DXC_CP_ACP};
    LPCWSTR args[] = {L"-T lib_6_3"};
    IDxcResult* result = nullptr;
    if (FAILED(compiler_->Compile(&buf, args, std::size(args), nullptr, __uuidof(IDxcResult),
                                  reinterpret_cast<void**>(&result))) || !result)
      return nullptr;
    IDxcBlob* obj = nullptr;
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (SUCCEEDED(status))
      result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&obj), nullptr);
    result->Release();
    if (obj) { obj->AddRef(); specLibs_.emplace(specConstants, obj); }
    return obj;
  }
  HMODULE library_ = nullptr;
  DxcCreateInstanceProc createInstance_ = nullptr;
  IDxcCompiler3* compiler_ = nullptr;
  IDxcUtils* utils_ = nullptr;
  std::mutex mutex_;
  std::unordered_map<uint32_t, IDxcBlob*> specLibs_;
};
DxcRuntime& Dxc() { static DxcRuntime r; return r; }
#endif

struct GuestShader {
  const ShaderCacheEntry* entry = nullptr;
  bool isVS = false;
  std::unique_ptr<RenderShader> shader;
  bool tried = false;
};
std::unordered_map<uint32_t, GuestShader> g_shaders;
std::mutex g_shadersMutex;

std::atomic<uint64_t> g_hit{0}, g_miss{0}, g_created{0}, g_failed{0};
RenderShader* g_currentVS = nullptr;
RenderShader* g_currentPS = nullptr;

RenderShader* GetOrCreateShader(uint32_t objVA) {
  if (!objVA || !eot::gpu::Device()) return nullptr;
  std::lock_guard lock(g_shadersMutex);
  auto it = g_shaders.find(objVA);
  if (it == g_shaders.end() || !it->second.entry) return nullptr;
  GuestShader& gs = it->second;
  if (gs.shader) return gs.shader.get();
  if (gs.tried) return nullptr;
  gs.tried = true;

  EnsureDxilCache();
  if (!g_dxilCache) return nullptr;
  const ShaderCacheEntry* e = gs.entry;
  const uint8_t* dxil = g_dxilCache.get() + e->dxil_offset;

  if (e->spec_constants_mask == 0) {
    gs.shader = eot::gpu::Device()->createShader(dxil, e->dxil_size, "main",
                                                 RenderShaderFormat::DXIL);
  } else {
#ifdef _WIN32
    IDxcBlob* linked = Dxc().Link(dxil, e->dxil_size, e->dxil_offset, gs.isVS, 0);
    if (linked) {
      gs.shader = eot::gpu::Device()->createShader(linked->GetBufferPointer(),
                                                   static_cast<uint32_t>(linked->GetBufferSize()),
                                                   "main", RenderShaderFormat::DXIL);
      linked->Release();
    }
#endif
  }

  if (gs.shader) {
    g_created.fetch_add(1, std::memory_order_relaxed);
    uint64_t c = g_created.load();
    if (c <= 8 || (c % 32) == 0)
      REXGPU_INFO("shader: created Plume {} #{} (mask=0x{:X}) | created={} failed={}",
                  gs.isVS ? "VS" : "PS", c, e->spec_constants_mask, c, g_failed.load());
  } else {
    g_failed.fetch_add(1, std::memory_order_relaxed);
  }
  return gs.shader.get();
}

void RegisterShader(uint8_t* base, uint32_t pFunction, uint32_t objVA, bool isVS) {
  if (pFunction < 0x1000 || !objVA) return;
  const uint32_t total =
      gmem::ReadU32(base, pFunction + 4) + gmem::ReadU32(base, pFunction + 8);
  if (total == 0 || total > 0x100000) return;
  const uint64_t hash = XXH3_64bits(gmem::ToHost(base, pFunction), total);
  const ShaderCacheEntry* entry = FindShaderCacheEntry(hash);
  (entry ? g_hit : g_miss).fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(g_shadersMutex);
  g_shaders[objVA] = GuestShader{entry, isVS, nullptr, false};
}

}

namespace eot::gpu {
RenderShader* CurrentVertexShader() { return g_currentVS; }
RenderShader* CurrentPixelShader() { return g_currentPS; }
}

REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const uint32_t pFunction = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  RegisterShader(base, pFunction, ctx.r3.u32, true);
}

REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const uint32_t pFunction = ctx.r3.u32;
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  RegisterShader(base, pFunction, ctx.r3.u32, false);
}

REX_EXTERN(__imp__D3DDevice_SetVertexShader);
REX_HOOK_RAW(D3DDevice_SetVertexShader) {
  const uint32_t obj = ctx.r4.u32;
  __imp__D3DDevice_SetVertexShader(ctx, base);
  g_currentVS = GetOrCreateShader(obj);
}

REX_EXTERN(__imp__D3DDevice_SetPixelShader);
REX_HOOK_RAW(D3DDevice_SetPixelShader) {
  const uint32_t obj = ctx.r4.u32;
  __imp__D3DDevice_SetPixelShader(ctx, base);
  g_currentPS = GetOrCreateShader(obj);
}
