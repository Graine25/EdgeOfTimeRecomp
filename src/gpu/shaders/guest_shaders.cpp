#include "gpu/shaders/guest_shaders.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <rex/hash.h>
#include <zstd.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/guest/d3d.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_linker.h"

namespace eot::gpu {

namespace {

std::mutex g_shader_mutex;
std::unordered_map<u32, GuestShader *> g_shader_objects;

struct Stats {
  std::atomic<u32> registered{0};
  std::atomic<u32> resolved{0};
  std::atomic<u32> missed{0};
  std::atomic<u32> bad_container{0};
  std::atomic<u32> built{0};
  std::atomic<u32> build_failed{0};
};
Stats g_stats;

u32 Bump(std::atomic<u32> &counter) {
  return counter.fetch_add(1, std::memory_order_relaxed);
}

std::once_flag g_dxil_cache_once;
std::unique_ptr<u8[]> g_dxil_cache;

const u8 *DxilCache() {
  std::call_once(g_dxil_cache_once, [] {
    if (g_dxilCacheDecompressedSize == 0) {
      EOT_ERROR("shader cache is empty - the build produced no recompiled "
                "shaders (see cmake/shader_cache.cmake)");
      return;
    }
    auto buf = std::make_unique<u8[]>(g_dxilCacheDecompressedSize);
    const size_t n =
        ZSTD_decompress(buf.get(), g_dxilCacheDecompressedSize,
                        g_compressedDxilCache, g_dxilCacheCompressedSize);
    if (ZSTD_isError(n) || n != g_dxilCacheDecompressedSize) {
      EOT_ERROR("DXIL shader cache decompression failed ({} of {} bytes)", n,
                g_dxilCacheDecompressedSize);
      return;
    }
    g_dxil_cache = std::move(buf);
    EOT_INFO("shader cache: {} entries, DXIL {} -> {} bytes",
             g_shaderCacheEntryCount, g_dxilCacheCompressedSize,
             g_dxilCacheDecompressedSize);
  });
  return g_dxil_cache.get();
}

constexpr u32 kGuestPageSize = 0x1000;

bool ReadableSpan(u32 va, u32 size) {
  if (!va || !size || va + size < va)
    return false;
  for (u32 p = va; p < va + size;
       p = (p & ~(kGuestPageSize - 1)) + kGuestPageSize) {
    if (!mem::try_translate(p, 1))
      return false;
  }
  return true;
}

const ShaderContainer *ContainerAt(u32 container_va) {
  if (!ReadableSpan(container_va, sizeof(ShaderContainer)))
    return nullptr;
  return mem::try_at<const ShaderContainer>(container_va);
}

const char *TypeName(ResourceType type) {
  return type == ResourceType::PixelShader ? "ps" : "vs";
}

}

u32 ShaderContainerOffset(ResourceType type) {
  return type == ResourceType::PixelShader ? kPixelShaderContainerOffset
                                           : kVertexShaderContainerOffset;
}

bool IsValidShaderContainer(u32 container_va, ResourceType type) {
  const ShaderContainer *c = ContainerAt(container_va);
  if (!c)
    return false;
  const u32 flags = c->Flags;
  if ((flags & kShaderContainerMagicMask) != kShaderContainerMagic)
    return false;
  const bool is_vertex = (flags & kShaderContainerVertexBit) != 0;
  if (is_vertex != (type == ResourceType::VertexShader))
    return false;
  return c->Field1C == 0u && c->Field20 == 0u;
}

u64 HashShaderContainer(u32 container_va, u32 physical_va) {
  const ShaderContainer *c = ContainerAt(container_va);
  if (!c)
    return 0;
  const u32 virtual_size = c->VirtualSize;
  const u32 physical_size = c->PhysicalSize;

  if (physical_va == 0 || physical_size == 0) {
    const u32 total = virtual_size + physical_size;
    if (!ReadableSpan(container_va, total))
      return 0;
    return XXH3_64bits(mem::try_translate(container_va, 1), total);
  }

  if (!ReadableSpan(container_va, virtual_size) ||
      !ReadableSpan(physical_va, physical_size))
    return 0;

  std::vector<u8> joined;
  joined.reserve(size_t{virtual_size} + physical_size);
  const auto *bytes =
      static_cast<const u8 *>(mem::try_translate(container_va, 1));
  joined.insert(joined.end(), bytes, bytes + virtual_size);
  bytes = static_cast<const u8 *>(mem::try_translate(physical_va, 1));
  joined.insert(joined.end(), bytes, bytes + physical_size);
  return XXH3_64bits(joined.data(), joined.size());
}

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash) {
  const ShaderCacheEntry *begin = g_shaderCacheEntries;
  const ShaderCacheEntry *end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry *it = std::lower_bound(
      begin, end, hash,
      [](const ShaderCacheEntry &lhs, u64 rhs) { return lhs.hash < rhs; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

GuestShader *ResolveGuestShader(u32 object_va) {
  if (!object_va)
    return nullptr;
  std::lock_guard lock(g_shader_mutex);
  auto it = g_shader_objects.find(object_va);
  return it != g_shader_objects.end() ? it->second : nullptr;
}

GuestShader *RegisterShaderObject(u32 object_va, ResourceType type,
                                  u32 container_va, u32 physical_va) {
  if (!object_va)
    return nullptr;
  {
    std::lock_guard lock(g_shader_mutex);
    auto it = g_shader_objects.find(object_va);
    if (it != g_shader_objects.end())
      return it->second;
  }

  const ShaderContainer *c = ContainerAt(container_va);
  u64 hash = 0;
  if (!IsValidShaderContainer(container_va, type)) {
    if (Bump(g_stats.bad_container) == 0) {
      EOT_WARN("[shader] object 0x{:08X} has no valid {} container at 0x{:08X} "
               "(flags=0x{:08X}) - container offset is wrong",
               object_va, TypeName(type), container_va, c ? u32(c->Flags) : 0u);
    }
  } else {
    hash = HashShaderContainer(container_va, physical_va);
  }

  const ShaderCacheEntry *entry = hash ? FindShaderCacheEntry(hash) : nullptr;
  if (entry) {
    Bump(g_stats.resolved);
  } else if (hash && Bump(g_stats.missed) == 0) {
    EOT_WARN("[shader] cache miss: hash=0x{:016X} {} virtual={} physical={}",
             hash, TypeName(type), u32(c->VirtualSize), u32(c->PhysicalSize));
  }

  auto *shader = HostResourceHeap::Alloc<GuestShader>(type);
  if (!shader)
    return nullptr;
  shader->objectVa = object_va;
  shader->hash = hash;
  shader->shaderCacheEntry = entry;

  std::lock_guard lock(g_shader_mutex);
  auto [it, inserted] = g_shader_objects.try_emplace(object_va, shader);
  if (!inserted) {
    HostResourceHeap::Free(shader);
    return it->second;
  }

  const u32 n = Bump(g_stats.registered) + 1;
  if (n == 1) {
    EOT_INFO("[shader] first guest shader registered: object=0x{:08X} "
             "hash=0x{:016X} {}",
             object_va, hash, entry ? "resolved" : "UNRESOLVED");
  } else if (n == 100 || n % 1000 == 0) {
    LogShaderStats();
  }
  return shader;
}

plume::RenderShader *GetOrLinkShader(GuestShader *gs, u32 specConstants) {
  const ShaderCacheEntry *entry = gs ? gs->shaderCacheEntry : nullptr;
  auto *device = Video::HostDevice();
  if (!entry || !device)
    return nullptr;

  const u32 masked = specConstants & entry->spec_constants_mask;
  {
    std::lock_guard lock(g_shader_mutex);
    auto it = gs->variants.find(masked);
    if (it != gs->variants.end())
      return it->second.get();
  }

  const u8 *cache = DxilCache();
  if (!cache)
    return nullptr;
  const u8 *dxil = cache + entry->dxil_offset;

  std::unique_ptr<plume::RenderShader> built;
  if (entry->spec_constants_mask == 0) {
    built = device->createShader(dxil, entry->dxil_size, LinkedEntryPointName(),
                                 plume::RenderShaderFormat::DXIL);
  } else {
    std::vector<u8> linked =
        LinkSpecConstant(dxil, entry->dxil_size,
                         gs->type == ResourceType::PixelShader, masked);
    if (!linked.empty())
      built = device->createShader(linked.data(), linked.size(),
                                   LinkedEntryPointName(),
                                   plume::RenderShaderFormat::DXIL);
  }
  if (!built) {
    Bump(g_stats.build_failed);
    return nullptr;
  }
  if (Bump(g_stats.built) == 0) {
    EOT_INFO("[shader] first host shader built: hash=0x{:016X} mask=0x{:X} "
             "value=0x{:X}",
             entry->hash, entry->spec_constants_mask, masked);
  }

  std::lock_guard lock(g_shader_mutex);
  auto [it, inserted] = gs->variants.try_emplace(masked, std::move(built));
  return it->second.get();
}

void LogShaderStats() {
  EOT_INFO("[shader] {} registered, {} resolved, {} missed, {} bad; {} built, "
           "{} failed",
           g_stats.registered.load(), g_stats.resolved.load(),
           g_stats.missed.load(), g_stats.bad_container.load(),
           g_stats.built.load(), g_stats.build_failed.load());
}

}
