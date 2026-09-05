#include "gpu/gpu_profiling.h"

#if defined(REXGLUE_ENABLE_PROFILING) && defined(EOT_D3D12)

namespace eot::gpu {

namespace {
TracyD3D12Ctx g_ctx = nullptr;
ID3D12GraphicsCommandList *g_cmd = nullptr;
}

void InitGPUProfiler(ID3D12Device *device, ID3D12CommandQueue *queue) {
  if (!TracyIsStarted || g_ctx)
    return;
  g_ctx = TracyD3D12Context(device, queue);
  TracyD3D12ContextName(g_ctx, "D3D12", 5);
}

TracyD3D12Ctx GpuProfilerCtx() { return g_ctx; }

void SetGPUProfilerCommandList(ID3D12GraphicsCommandList *cmd) { g_cmd = cmd; }

ID3D12GraphicsCommandList *GpuProfilerCommandList() { return g_cmd; }

}

#endif
