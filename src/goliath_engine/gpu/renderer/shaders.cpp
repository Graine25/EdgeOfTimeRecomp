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
#include <d3d12shader.h>
#endif

#include "generated/shader_cache.h"
#include "src/goliath_engine/gpu/renderer/video.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;
using namespace plume;

namespace {

constexpr uint32_t kSpecR11G11B10Normal = 1u << 0;
constexpr uint32_t kSpecAlphaTest = 1u << 1;
constexpr uint32_t kSpecSintTexcoord = 1u << 2;

constexpr uint32_t kColorControlOffset = 0x2934 + 0x8;

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

  bool UsesCbv(const void* dxil, size_t size, uint32_t bindPoint, uint32_t space) {
    if (!utils_) return false;
    DxcBuffer buf{dxil, size, DXC_CP_ACP};
    ID3D12ShaderReflection* refl = nullptr;
    if (FAILED(utils_->CreateReflection(&buf, __uuidof(ID3D12ShaderReflection),
                                        reinterpret_cast<void**>(&refl))) || !refl)
      return false;
    D3D12_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    bool found = false;
    for (uint32_t i = 0; i < sd.BoundResources; ++i) {
      D3D12_SHADER_INPUT_BIND_DESC b{};
      if (FAILED(refl->GetResourceBindingDesc(i, &b))) continue;
      if (b.Type == D3D_SIT_CBUFFER && b.BindPoint == bindPoint && b.Space == space) {
        found = true;
        break;
      }
    }
    refl->Release();
    return found;
  }

  void LogInputSignature(const void* dxil, size_t size, bool isVS) {
    if (!utils_) return;
    DxcBuffer buf{dxil, size, DXC_CP_ACP};
    ID3D12ShaderReflection* refl = nullptr;
    if (FAILED(utils_->CreateReflection(&buf, __uuidof(ID3D12ShaderReflection),
                                        reinterpret_cast<void**>(&refl))) || !refl)
      return;
    D3D12_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    for (uint32_t i = 0; i < sd.InputParameters; ++i) {
      D3D12_SIGNATURE_PARAMETER_DESC p{};
      refl->GetInputParameterDesc(i, &p);
      const char* ct = p.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ? "float"
                       : p.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint"
                       : p.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? "sint"
                                                                          : "?";
      REXGPU_INFO("  {} input[{}] {}{} type={} mask=0x{:X}", isVS ? "VS" : "PS", i, p.SemanticName,
                  p.SemanticIndex, ct, p.Mask);
    }
    refl->Release();
  }

  IDxcBlob* Link(const void* dxil, uint32_t dxilSize, uint32_t dxilOffset, bool isVS,
                 uint32_t specConstants) {
    if (!ready()) return nullptr;
    const wchar_t* entry = entryName_.load(std::memory_order_relaxed);
    if (entry) return LinkWith(dxil, dxilSize, dxilOffset, isVS, specConstants, entry);
    for (const wchar_t* candidate : {L"main", L"shaderMain"}) {
      IDxcBlob* b = LinkWith(dxil, dxilSize, dxilOffset, isVS, specConstants, candidate, true);
      if (b) {
        entryName_.store(candidate, std::memory_order_relaxed);
        REXGPU_INFO("DXC: shader entry name = '{}'", candidate == std::wstring(L"main") ? "main"
                                                                                        : "shaderMain");
        return b;
      }
    }
    REXGPU_ERROR("DXC: shader link failed for both 'main' and 'shaderMain' entry names");
    return nullptr;
  }

 private:
  IDxcBlob* LinkWith(const void* dxil, uint32_t dxilSize, uint32_t dxilOffset, bool isVS,
                     uint32_t specConstants, const wchar_t* entry, bool quiet = false) {
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
    HRESULT hr = linker->Link(entry, isVS ? L"vs_6_0" : L"ps_6_0", libs, std::size(libs),
                              nullptr, 0, &link);
    linker->Release();
    if (FAILED(hr) || !link) return nullptr;
    HRESULT status = E_FAIL;
    link->GetStatus(&status);
    IDxcBlob* out = nullptr;
    if (SUCCEEDED(status)) {
      link->GetResult(&out);
      out = SignDxil(out);
    } else if (!quiet) {
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

  IDxcBlob* SignDxil(IDxcBlob* blob) {
    if (!blob) return nullptr;
    IDxcValidator* validator = nullptr;
    if (FAILED(createInstance_(CLSID_DxcValidator, __uuidof(IDxcValidator),
                               reinterpret_cast<void**>(&validator))) || !validator) {
      if (!warnedNoValidator_.exchange(true))
        REXGPU_ERROR("DXC: no IDxcValidator (dxil.dll missing) - linked shaders stay unsigned");
      return blob;
    }
    IDxcOperationResult* result = nullptr;
    HRESULT hr = validator->Validate(blob, DxcValidatorFlags_InPlaceEdit, &result);
    validator->Release();
    if (FAILED(hr) || !result) return blob;
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status) && !warnedNoValidator_.exchange(true)) {
      IDxcBlobEncoding* err = nullptr;
      if (SUCCEEDED(result->GetErrorBuffer(&err)) && err) {
        std::string msg(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        REXGPU_ERROR("DXC validate failed: {}", msg);
        err->Release();
      }
    }
    result->Release();
    return blob;
  }

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
  std::atomic<const wchar_t*> entryName_{nullptr};
  std::atomic<bool> warnedNoValidator_{false};
};
DxcRuntime& Dxc() { static DxcRuntime r; return r; }
#endif

struct GuestShader {
  const ShaderCacheEntry* entry = nullptr;
  bool isVS = false;
  bool reflected = false;
  bool windowSpace = false;
  std::unordered_map<uint32_t, std::unique_ptr<RenderShader>> variants;
};
std::unordered_map<uint32_t, GuestShader> g_shaders;
std::mutex g_shadersMutex;

std::atomic<uint64_t> g_hit{0}, g_miss{0}, g_created{0}, g_failed{0};
std::atomic<uint32_t> g_vertexFormatSpecBits{0};
uint32_t g_currentVSObj = 0, g_currentPSObj = 0;
RenderShader* g_currentVS = nullptr;
RenderShader* g_currentPS = nullptr;
std::atomic<bool> g_currentVsWindowSpace{false};

std::unique_ptr<RenderShader> CreateVariant(const ShaderCacheEntry* e, bool isVS,
                                            uint32_t maskedSpec) {
  EnsureDxilCache();
  if (!g_dxilCache || !eot::gpu::Device()) return nullptr;
  const uint8_t* dxil = g_dxilCache.get() + e->dxil_offset;
  std::unique_ptr<RenderShader> shader;
  static std::atomic<int> s_reflLogged{0};
  if (e->spec_constants_mask == 0) {
    shader = eot::gpu::Device()->createShader(dxil, e->dxil_size, "main", RenderShaderFormat::DXIL);
#ifdef _WIN32
    if (s_reflLogged.fetch_add(1) < 6) Dxc().LogInputSignature(dxil, e->dxil_size, isVS);
#endif
  } else {
#ifdef _WIN32
    IDxcBlob* linked = Dxc().Link(dxil, e->dxil_size, e->dxil_offset, isVS, maskedSpec);
    if (linked) {
      shader = eot::gpu::Device()->createShader(linked->GetBufferPointer(),
                                                static_cast<uint32_t>(linked->GetBufferSize()),
                                                "main", RenderShaderFormat::DXIL);
      if (s_reflLogged.fetch_add(1) < 6)
        Dxc().LogInputSignature(linked->GetBufferPointer(), linked->GetBufferSize(), isVS);
      linked->Release();
    }
#endif
  }
  (shader ? g_created : g_failed).fetch_add(1, std::memory_order_relaxed);
  return shader;
}

RenderShader* GetVariant(uint32_t objVA, uint32_t specValue) {
  if (!objVA || !eot::gpu::Device()) return nullptr;
  std::lock_guard lock(g_shadersMutex);
  auto it = g_shaders.find(objVA);
  if (it == g_shaders.end() || !it->second.entry) return nullptr;
  GuestShader& gs = it->second;
  if (gs.isVS && !gs.reflected) {
    gs.reflected = true;
#ifdef _WIN32
    EnsureDxilCache();
    if (g_dxilCache)
      gs.windowSpace = !Dxc().UsesCbv(g_dxilCache.get() + gs.entry->dxil_offset,
                                      gs.entry->dxil_size, 0, 4);
#endif
  }
  const uint32_t key = specValue & gs.entry->spec_constants_mask;
  auto vit = gs.variants.find(key);
  if (vit != gs.variants.end()) return vit->second.get();
  std::unique_ptr<RenderShader> sh = CreateVariant(gs.entry, gs.isVS, key);
  RenderShader* p = sh.get();
  gs.variants.emplace(key, std::move(sh));
  uint64_t c = g_created.load();
  if (c <= 8 || (c % 32) == 0)
    REXGPU_INFO("shader: variant {} spec=0x{:X} | created={} failed={}", gs.isVS ? "VS" : "PS", key,
                c, g_failed.load());
  return p;
}

uint32_t ComputeSpecConstants(uint8_t* base, uint32_t deviceVA) {
  uint32_t spec = g_vertexFormatSpecBits.load(std::memory_order_relaxed);
  if (deviceVA >= 0x1000) {
    const uint32_t colorControl = gmem::ReadU32(base, deviceVA + kColorControlOffset);
    if (colorControl & (1u << 3)) spec |= kSpecAlphaTest;
  }
  return spec;
}

void ResolveCurrent(uint8_t* base, uint32_t deviceVA) {
  const uint32_t spec = ComputeSpecConstants(base, deviceVA);
  g_currentVS = GetVariant(g_currentVSObj, spec);
  g_currentPS = GetVariant(g_currentPSObj, spec);
  bool ws = false;
  {
    std::lock_guard lock(g_shadersMutex);
    auto it = g_shaders.find(g_currentVSObj);
    if (it != g_shaders.end()) ws = it->second.windowSpace;
  }
  g_currentVsWindowSpace.store(ws, std::memory_order_relaxed);
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
  g_shaders[objVA].entry = entry;
  g_shaders[objVA].isVS = isVS;
}

}

namespace eot::gpu {
RenderShader* CurrentVertexShader() { return g_currentVS; }
RenderShader* CurrentPixelShader() { return g_currentPS; }
bool CurrentVsIsWindowSpace() { return g_currentVsWindowSpace.load(std::memory_order_relaxed); }
void ResolveShadersForDraw(uint8_t* base, uint32_t deviceVA) { ResolveCurrent(base, deviceVA); }
void SetVertexFormatSpecBits(uint32_t bits) {
  g_vertexFormatSpecBits.store(bits, std::memory_order_relaxed);
}
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
  const uint32_t device = ctx.r3.u32;
  g_currentVSObj = ctx.r4.u32;
  __imp__D3DDevice_SetVertexShader(ctx, base);
  ResolveCurrent(base, device);
}

REX_EXTERN(__imp__D3DDevice_SetPixelShader);
REX_HOOK_RAW(D3DDevice_SetPixelShader) {
  const uint32_t device = ctx.r3.u32;
  g_currentPSObj = ctx.r4.u32;
  __imp__D3DDevice_SetPixelShader(ctx, base);
  ResolveCurrent(base, device);
}
