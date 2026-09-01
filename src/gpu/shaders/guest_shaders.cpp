#include "gpu/shaders/guest_shaders.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>
#include <zstd.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/shaders/dxc_link.h"
#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

namespace {

struct CacheState {
  std::vector<u8> blob;
  std::unordered_map<u64, const ShaderCacheEntry *> by_hash;
  std::mutex spec_mutex;
  std::unordered_map<u32, std::vector<u8>> spec_libs;
  std::mutex host_mutex;
  std::unordered_map<u64, std::unique_ptr<plume::RenderShader>> host;
  bool ready = false;
};

CacheState &cache() {
  static CacheState c;
  return c;
}

const u8 *EntryBytes(const ShaderCacheEntry &e, u32 *size) {
  auto &c = cache();
  if (c.blob.empty()) {
    *size = 0;
    return nullptr;
  }
#if defined(EOT_D3D12)
  *size = e.dxilSize;
  return c.blob.data() + e.dxilOffset;
#else
  *size = e.spirvSize;
  return c.blob.data() + e.spirvOffset;
#endif
}

std::vector<u8> SpecLib(u32 value) {
  auto &c = cache();
  {
    std::lock_guard lock(c.spec_mutex);
    auto it = c.spec_libs.find(value);
    if (it != c.spec_libs.end())
      return it->second;
  }
  auto lib = CompileSpecConstantLib(value);
  if (lib.empty()) {
    EOT_ERROR("[shaders] CompileSpecConstantLib({:#x}) failed: are dxcompiler.dll and "
              "dxil.dll next to the executable?",
              value);
  }
  std::lock_guard lock(c.spec_mutex);
  return c.spec_libs.emplace(value, std::move(lib)).first->second;
}

}

bool GuestShadersInit() {
  auto &c = cache();
  if (c.ready)
    return !c.blob.empty();
  c.ready = true;
#if defined(EOT_D3D12)
  const u8 *compressed = g_compressedDxilCache;
  const size_t compressed_size = g_dxilCacheCompressedSize;
  const size_t decompressed_size = g_dxilCacheDecompressedSize;
#else
  const u8 *compressed = g_compressedSpirvCache;
  const size_t compressed_size = g_spirvCacheCompressedSize;
  const size_t decompressed_size = g_spirvCacheDecompressedSize;
#endif
  if (g_shaderCacheEntryCount == 0 || decompressed_size == 0) {
    EOT_ERROR("[shaders] the embedded shader cache is empty: build "
              "reeot_shader_cache_regen and rebuild; no guest shader will run");
    return false;
  }
  c.blob.resize(decompressed_size);
  const size_t got =
      ZSTD_decompress(c.blob.data(), decompressed_size, compressed, compressed_size);
  if (ZSTD_isError(got) || got != decompressed_size) {
    EOT_ERROR("[shaders] shader cache decompression failed: {}",
              ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short read");
    c.blob.clear();
    return false;
  }
  c.by_hash.reserve(g_shaderCacheEntryCount);
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i)
    c.by_hash.emplace(g_shaderCacheEntries[i].hash, &g_shaderCacheEntries[i]);
  EOT_INFO("[shaders] cache ready: {} shaders, {} MB decompressed", g_shaderCacheEntryCount,
           decompressed_size / (1024 * 1024));
  return true;
}

u32 GuestShaderCacheCount() { return static_cast<u32>(g_shaderCacheEntryCount); }

GuestShader *FindGuestShader(VideoState &s, u32 object_va) {
  auto it = s.shaders.find(object_va);
  return it == s.shaders.end() ? nullptr : it->second.get();
}

u32 FloatConstantRegisters(const u8 *bytes, u32 size, u32 table_off, bool is_pixel) {
  constexpr u32 kFull = 256;
  constexpr u32 kTableBytes = 4 + 28;
  constexpr u32 kInfoBytes = 20;      // D3DXSHADER_CONSTANTINFO
  if (!table_off || table_off + kTableBytes > size)
    return kFull;
  const u8 *table = bytes + table_off + 4;
  const u32 constants = rex::memory::load_and_swap<u32>(table + 12);
  const u32 info_off = rex::memory::load_and_swap<u32>(table + 16);
  if (!constants || constants > 1024)
    return kFull;
  const u64 info_end = u64(table_off) + 4 + info_off + u64(constants) * kInfoBytes;
  if (info_end > size)
    return kFull;
  u32 end = 0;
  for (u32 i = 0; i < constants; ++i) {
    const u8 *ci = table + info_off + i * kInfoBytes;
    const u32 set = rex::memory::load_and_swap<u16>(ci + 4);
    const u32 index = rex::memory::load_and_swap<u16>(ci + 6);
    const u32 count = rex::memory::load_and_swap<u16>(ci + 8);
    if (set != 2) // D3DXRS_FLOAT4
      continue;
    if (count > 1 || index >= kFull)
      return kFull;
    end = std::max(end, index + 1);
  }
  (void)is_pixel;
  return std::max(end, 16u);
}

GuestShader *RegisterGuestShader(VideoState &s, u32 object_va, bool is_pixel) {
  if (!object_va)
    return nullptr;
  const u32 container_va =
      object_va + (is_pixel ? obj::kPixelShaderContainer : obj::kVertexShaderContainer);
  const u32 physical_va = mem::load<u32>(
      object_va + (is_pixel ? obj::kPixelShaderPhysical : obj::kVertexShaderPhysical));
  auto *header = mem::at<ShaderContainerHeader>(container_va);
  if (!header) {
    EOT_WARN("[shaders] register {:#x}: container unreadable", object_va);
    return nullptr;
  }
  const u32 flags = header->flags;
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
    u32 n;
    if (DiagShouldLog(0x5B00, &n))
      EOT_WARN("[shaders] register {:#x}: bad container magic {:#x}", object_va, flags);
    return nullptr;
  }
  const u32 virtual_size = header->virtualSize;
  const u32 physical_size = header->physicalSize;
  const u8 *virtual_bytes = mem::at<u8>(container_va);
  const u8 *physical_bytes = physical_va ? mem::at<u8>(physical_va) : nullptr;
  if (!virtual_bytes || (physical_size && !physical_bytes) ||
      virtual_size < sizeof(*header)) {
    EOT_WARN("[shaders] register {:#x}: parts unreadable (virt {} phys {} @ {:#x})",
             object_va, virtual_size, physical_size, physical_va);
    return nullptr;
  }

  XXH3_state_t *xs = XXH3_createState();
  XXH3_64bits_reset(xs);
  XXH3_64bits_update(xs, virtual_bytes, virtual_size);
  if (physical_size)
    XXH3_64bits_update(xs, physical_bytes, physical_size);
  const u64 hash = XXH3_64bits_digest(xs);
  XXH3_freeState(xs);

  auto &slot = s.shaders[object_va];
  if (slot && slot->hash == hash)
    return slot.get();
  auto sh = std::make_unique<GuestShader>();
  sh->va = object_va;
  sh->hash = hash;
  sh->isPixel = is_pixel;
  auto &c = cache();
  auto it = c.by_hash.find(hash);
  sh->entry = it == c.by_hash.end() ? nullptr : it->second;
  if (sh->entry)
    sh->usesFloatConstants = sh->entry->usesFloatConstants != 0;
  sh->floatConstantRegs =
      FloatConstantRegisters(virtual_bytes, virtual_size, header->constantTableOffset, is_pixel);

  if (!is_pixel) {
    const u32 shader_off = header->shaderOffset;
    auto *rec = mem::at<VertexShaderRecord>(container_va + shader_off);
    if (rec && shader_off + sizeof(ShaderRecord) < virtual_size) {
      const u32 first = rec->field18;
      const u32 count = rec->vertexElementCount;
      if (count <= 32) {
        for (u32 i = 0; i < count; ++i) {
          const u32 v = rec->vertexElementsAndInterpolators[first + i];
          VertexInput in;
          in.usage = static_cast<u8>((v >> 12) & 0xF);
          in.usageIndex = static_cast<u8>((v >> 16) & 0xF);
          sh->inputs.push_back(in);
        }
      }
    }
    if (sh->inputs.empty() && sh->entry) {
      for (u32 i = 0; i < sh->entry->vertexLayoutCount; ++i) {
        const u32 w0 = g_shaderVertexLayouts[sh->entry->vertexLayoutOffset + i * 2];
        VertexInput in;
        in.usage = static_cast<u8>(w0 & 0xF);
        in.usageIndex = static_cast<u8>((w0 >> 4) & 0xF);
        sh->inputs.push_back(in);
      }
    }
  }

  EOT_INFO("[shaders] {} {:#x} hash {:016x} {} inputs={} spec={:#x} regs={}",
            is_pixel ? "ps" : "vs", object_va, hash, sh->entry ? "hit" : "MISS",
            sh->inputs.size(), sh->entry ? sh->entry->specConstantsMask : 0,
            sh->floatConstantRegs);
  slot = std::move(sh);
  return slot.get();
}

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash) {
  auto &c = cache();
  auto it = c.by_hash.find(hash);
  return it == c.by_hash.end() ? nullptr : it->second;
}

void VertexInputsFromEntry(const ShaderCacheEntry &e, std::vector<VertexInput> &out) {
  out.clear();
  for (u32 i = 0; i < e.vertexLayoutCount; ++i) {
    const u32 w0 = g_shaderVertexLayouts[e.vertexLayoutOffset + i * 2];
    VertexInput in;
    in.usage = static_cast<u8>(w0 & 0xF);
    in.usageIndex = static_cast<u8>((w0 >> 4) & 0xF);
    out.push_back(in);
  }
}

plume::RenderShader *GetHostShaderByHash(VideoState &s, u64 hash, u32 spec_mask, bool is_pixel,
                                         bool worker) {
  auto &c = cache();
  const ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  if (!entry || !s.device)
    return nullptr;
  const u32 effective = spec_mask & entry->specConstantsMask;
  const u64 key = hash ^ ((u64(effective) + 1) * 0x9E3779B97F4A7C15ull) ^ (is_pixel ? 1u : 0u);
  {
    std::lock_guard lock(c.host_mutex);
    auto it = c.host.find(key);
    if (it != c.host.end())
      return it->second.get();
  }
  u32 size = 0;
  const u8 *bytes = EntryBytes(*entry, &size);
  std::unique_ptr<plume::RenderShader> host;
  if (bytes && size) {
    if (entry->specConstantsMask == 0) {
      host = s.device->createShader(bytes, size, "main", kHostShaderFormat);
      if (!host)
        EOT_ERROR("[shaders] createShader failed for {:016x}", hash);
    } else {
      std::unique_ptr<PerfScope> perf_scope;
      if (!worker) {
        perf_scope = std::make_unique<PerfScope>(s.perf.link_ms);
        s.perf.links++;
      }
#if defined(EOT_D3D12)
      const std::vector<u8> spec_lib = SpecLib(effective);
      if (!spec_lib.empty()) {
        std::string error;
        auto linked = LinkSpecConstantLib(bytes, size, spec_lib.data(), spec_lib.size(),
                                          is_pixel ? L"ps_6_0" : L"vs_6_0", &error);
        if (linked.empty()) {
          EOT_ERROR("[shaders] DXC link failed for {:016x} mask {:#x} ({} bytes): {}", hash,
                    effective, size, error);
        } else {
          host = s.device->createShader(linked.data(), linked.size(), "main", kHostShaderFormat);
        }
      }
#else
      host = s.device->createShader(bytes, size, "main", kHostShaderFormat);
#endif
    }
  }
  std::lock_guard lock(c.host_mutex);
  auto [it, inserted] = c.host.emplace(key, std::move(host));
  return it->second.get();
}

plume::RenderShader *ResolveHostShader(VideoState &s, GuestShader &shader, u32 spec_mask) {
  if (!shader.entry) {
    if (!shader.cacheMissLogged) {
      shader.cacheMissLogged = true;
      EOT_WARN("[shaders] {} {:#x} hash {:016x} is not in the shader cache",
               shader.isPixel ? "ps" : "vs", shader.va, shader.hash);
    }
    return nullptr;
  }
  return GetHostShaderByHash(s, shader.hash, spec_mask, shader.isPixel);
}

}
