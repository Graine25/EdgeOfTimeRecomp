#pragma once

#include <string>

#include <rex/types.h>

namespace plume {
struct RenderCommandList;
}

namespace eot::gpu {

struct VideoState;
struct PerfCounters;

constexpr u32 kGpuCatOther = 0;
constexpr u32 kGpuCatShadow = 1;
constexpr u32 kGpuCatResolve = 2;
constexpr u32 kGpuCatPresent = 3;
constexpr u32 kGpuCatUpload = 4;
constexpr u32 kGpuCatResolveHw = 6;
constexpr u32 kGpuCatResolveDepth = 7;
constexpr u32 kGpuCatBroadcast = 8;
constexpr u32 kGpuCatTaa = 9;

u32 GpuTargetCategory(bool full_frame, bool has_depth, u32 color_count, u32 color0_host_format,
                      u32 samples, bool additive);
std::string GpuCategoryName(u32 cat);

void GpuTimingFrameBegin(VideoState &s, plume::RenderCommandList *cmd, u32 slot);
void GpuTimingMark(VideoState &s, plume::RenderCommandList *cmd, u32 cat);
void GpuTimingCountDraw(VideoState &s);
bool GpuTimingDiagActive(const VideoState &s);
void GpuTimingDiagMark(VideoState &s, plume::RenderCommandList *cmd, std::string tag);
void GpuTimingFrameEnd(plume::RenderCommandList *cmd);
void GpuTimingCollect(VideoState &s, u32 slot);
std::string GpuTimingSummary(const PerfCounters &p);

}
