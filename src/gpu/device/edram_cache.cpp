#include "gpu/device/edram_cache.h"

#include <cstring>
#include <mutex>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/registers.h>
#include <rex/string.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "core/logging.h"

namespace eot::gpu {

namespace rg = rex::graphics;
namespace xe = rex::graphics::xenos;

namespace {

class EotRenderTargetCache final : public rg::RenderTargetCache {
public:
  class Target final : public RenderTarget {
  public:
    explicit Target(RenderTargetKey key) : RenderTarget(key) {}
  };

  EotRenderTargetCache(const rg::RegisterFile &regs,
                       const rex::memory::Memory &mem)
      : RenderTargetCache(regs, mem, 1, 1) {}

  Path GetPath() const override { return Path::kHostRenderTargets; }

  bool IsGammaFormatHostStorageSeparate() const override { return false; }

  uint32_t GetMaxRenderTargetWidth() const override { return 8192; }
  uint32_t GetMaxRenderTargetHeight() const override { return 8192; }

  RenderTarget *CreateRenderTarget(RenderTargetKey key) override {
    owned_.push_back(std::make_unique<Target>(key));
    return owned_.back().get();
  }

  bool IsHostDepthEncodingDifferent(
      xe::DepthRenderTargetFormat format) const override {
    return format == xe::DepthRenderTargetFormat::kD24FS8;
  }

  size_t owned_count() const { return owned_.size(); }

private:
  std::vector<std::unique_ptr<Target>> owned_;
};

std::mutex g_mutex;
std::unique_ptr<rg::RegisterFile> g_regs;

std::unique_ptr<EotRenderTargetCache> g_cache;

}

void FillEdramRegisters(rg::RegisterFile &regs, u32,
                        u32 color_info, u32 depth_info, u32 surface_info) {
  regs[rg::XE_GPU_REG_RB_COLOR_INFO] = color_info;
  regs[rg::XE_GPU_REG_RB_DEPTH_INFO] = depth_info;
  regs[rg::XE_GPU_REG_RB_SURFACE_INFO] = surface_info;
}

bool InitEdramCache(plume::RenderDevice *device) {
  if (!device)
    return false;
  std::lock_guard lock(g_mutex);
  if (g_cache)
    return true;
  auto *rt = rex::Runtime::instance();
  auto *memory = rt ? rt->memory() : nullptr;
  if (!memory) {
    EOT_WARN("[edram] no guest memory yet; cache not built");
    return false;
  }
  g_regs = std::make_unique<rg::RegisterFile>();
  std::memset(g_regs->values, 0, sizeof(g_regs->values));
  g_cache = std::make_unique<EotRenderTargetCache>(*g_regs, *memory);
  EOT_INFO("[edram] SDK render target cache built");
  return true;
}

void ShutdownEdramCache() {
  std::lock_guard lock(g_mutex);
  g_cache.reset();
  g_regs.reset();
}

bool RunEdramUpdate(const u32 *ucode, u32 ucode_dwords,
                    u32 depth_control, u32 color_mask) {
  if (!g_cache || !ucode || !ucode_dwords)
    return false;
  rg::Shader shader(xe::ShaderType::kVertex, 0, ucode, ucode_dwords);
  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm);

  rg::reg::RB_DEPTHCONTROL dc;
  dc.value = depth_control;
  return g_cache->Update(true, dc, color_mask, shader);
}

EdramProbe ProbeEdramCache(u32 device_va, u32 color_info, u32 depth_info,
                           u32 surface_info) {
  EdramProbe out;
  std::lock_guard lock(g_mutex);
  if (!g_cache || !g_regs)
    return out;
  FillEdramRegisters(*g_regs, device_va, color_info, depth_info, surface_info);

  rg::reg::RB_COLOR_INFO ci;
  ci.value = color_info;
  rg::reg::RB_DEPTH_INFO di;
  di.value = depth_info;
  rg::reg::RB_SURFACE_INFO si;
  si.value = surface_info;

  out.valid = true;
  out.colorBase = ci.color_base | (u32(ci.color_base_bit_11) << 11);
  out.pitchTiles = si.surface_pitch;
  out.depthBase = di.depth_base;
  out.ownedRanges = u32(g_cache->owned_count());
  return out;
}

}
