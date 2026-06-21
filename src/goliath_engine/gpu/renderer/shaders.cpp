#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "generated/shader_cache.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;

namespace {

const ShaderCacheEntry* FindShaderCacheEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry* it = std::lower_bound(
      begin, end, hash,
      [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

std::atomic<uint64_t> g_vs{0}, g_ps{0}, g_hit{0}, g_miss{0}, g_specMask{0};

const ShaderCacheEntry* ResolveShader(uint8_t* base, uint32_t pFunction, bool isVS) {
  if (pFunction < 0x1000) return nullptr;
  const uint32_t virtualSize = gmem::ReadU32(base, pFunction + 4);
  const uint32_t physicalSize = gmem::ReadU32(base, pFunction + 8);
  const uint32_t total = virtualSize + physicalSize;
  if (total == 0 || total > 0x100000) return nullptr;

  const void* hostBytes = gmem::ToHost(base, pFunction);
  const uint64_t hash = XXH3_64bits(hostBytes, total);
  const ShaderCacheEntry* entry = FindShaderCacheEntry(hash);

  (isVS ? g_vs : g_ps).fetch_add(1, std::memory_order_relaxed);
  if (entry) {
    g_hit.fetch_add(1, std::memory_order_relaxed);
    if (entry->spec_constants_mask != 0) g_specMask.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_miss.fetch_add(1, std::memory_order_relaxed);
  }

  const uint64_t done = g_hit.load() + g_miss.load();
  if (done <= 24 || (done % 64) == 0) {
    REXGPU_INFO(
        "[shader] {} hash=0x{:013X} sz={} -> {} (dxil={}B mask=0x{:X}) | totals vs={} ps={} "
        "hit={} miss={} specMask={}",
        isVS ? "VS" : "PS", hash, total, entry ? "HIT" : "MISS",
        entry ? entry->dxil_size : 0u, entry ? entry->spec_constants_mask : 0u,
        g_vs.load(), g_ps.load(), g_hit.load(), g_miss.load(), g_specMask.load());
  }
  return entry;
}

}

REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const uint32_t pFunction = ctx.r3.u32;
  ResolveShader(base, pFunction, true);
  __imp__D3DDevice_CreateVertexShader(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const uint32_t pFunction = ctx.r3.u32;
  ResolveShader(base, pFunction, false);
  __imp__D3DDevice_CreatePixelShader(ctx, base);
}
