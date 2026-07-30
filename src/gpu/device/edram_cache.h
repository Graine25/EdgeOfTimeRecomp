#pragma once

#include <memory>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

struct EdramProbe {
  bool valid = false;
  u32 colorBase = 0;
  u32 pitchTiles = 0;
  u32 depthBase = 0;
  u32 ownedRanges = 0;
};

void FillEdramRegisters(rex::graphics::RegisterFile &regs, u32 device_va,
                        u32 color_info, u32 depth_info, u32 surface_info);

bool InitEdramCache(plume::RenderDevice *device);
void ShutdownEdramCache();

EdramProbe ProbeEdramCache(u32 device_va, u32 color_info, u32 depth_info,
                           u32 surface_info);

bool RunEdramUpdate(const u32 *ucode, u32 ucode_dwords, u32 depth_control,
                    u32 color_mask);

}
