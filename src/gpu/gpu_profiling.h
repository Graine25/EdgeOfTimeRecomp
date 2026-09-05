#pragma once

#include <rex/types.h>

#include "core/profiling.h"

#if defined(REXGLUE_ENABLE_PROFILING) && defined(EOT_D3D12)

#include <plume_d3d12.h>
#include <tracy/TracyD3D12.hpp>

namespace eot::gpu {

void InitGPUProfiler(ID3D12Device *device, ID3D12CommandQueue *queue);
TracyD3D12Ctx GpuProfilerCtx();
void SetGPUProfilerCommandList(ID3D12GraphicsCommandList *cmd);
ID3D12GraphicsCommandList *GpuProfilerCommandList();

}

#define EOT_GPU_ZONE(name)                                                                         \
  ZoneNamedN(___tracy_scoped_zone, name, TracyIsStarted);                                          \
  static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_sloc, TracyLine){             \
      name, TracyFunction, TracyFile, (u32)TracyLine, 0};                                          \
  tracy::D3D12ZoneScope TracyConcat(__tracy_gpu_zone, TracyLine) {                                 \
    eot::gpu::GpuProfilerCtx(), eot::gpu::GpuProfilerCommandList(),                                \
        &TracyConcat(__tracy_gpu_sloc, TracyLine), TracyIsStarted                                  \
  }

#elif defined(REXGLUE_ENABLE_PROFILING)

#define EOT_GPU_ZONE(name) ZoneNamedN(___tracy_scoped_zone, name, TracyIsStarted)

#else

#define EOT_GPU_ZONE(name) ((void)0)

#endif
