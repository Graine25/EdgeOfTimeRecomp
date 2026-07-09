#include <rex/hook.h>
#include <rex/types.h>

#include "gpu/device/device.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/d3d.h"
#include "gpu/pipeline/vertex_layout.h"

REX_EXTERN(__imp__XGSetVertexBufferHeader);
REX_HOOK_RAW(XGSetVertexBufferHeader) {
  const u32 header_va = ctx.r7.u32;
  __imp__XGSetVertexBufferHeader(ctx, base);
  eot::gpu::RegisterBufferHeader(header_va, eot::gpu::ResourceType::VertexBuffer);
}

REX_EXTERN(__imp__XGSetIndexBufferHeader);
REX_HOOK_RAW(XGSetIndexBufferHeader) {
  const u32 header_va = ctx.r8.u32;
  __imp__XGSetIndexBufferHeader(ctx, base);
  eot::gpu::RegisterBufferHeader(header_va, eot::gpu::ResourceType::IndexBuffer);
}

REX_EXTERN(__imp__XGOffsetResourceAddress);
REX_HOOK_RAW(XGOffsetResourceAddress) {
  const u32 resource_va = ctx.r3.u32;
  const u32 base_va = ctx.r4.u32;
  __imp__XGOffsetResourceAddress(ctx, base);
  eot::gpu::NotifyBufferAddressFixup(resource_va, base_va);
}

REX_EXTERN(__imp__XGSetVertexDeclaration);
REX_HOOK_RAW(XGSetVertexDeclaration) {
  const u32 decl_va = ctx.r4.u32;
  __imp__XGSetVertexDeclaration(ctx, base);
  eot::gpu::RegisterVertexDeclaration(decl_va);
}

REX_EXTERN(__imp__D3DDevice_SetStreamSource);
REX_HOOK_RAW(D3DDevice_SetStreamSource) {
  const u32 stream = ctx.r4.u32;
  const u32 header_va = ctx.r5.u32;
  const u32 offset = ctx.r6.u32;
  const u32 stride = ctx.r7.u32;
  __imp__D3DDevice_SetStreamSource(ctx, base);
  eot::gpu::Video::SetStreamSource(
      stream,
      eot::gpu::ResolveGuestBuffer(header_va,
                                   eot::gpu::ResourceType::VertexBuffer),
      offset, stride);
}

REX_EXTERN(__imp__D3DDevice_SetIndices);
REX_HOOK_RAW(D3DDevice_SetIndices) {
  const u32 header_va = ctx.r4.u32;
  __imp__D3DDevice_SetIndices(ctx, base);
  eot::gpu::Video::SetIndices(eot::gpu::ResolveGuestBuffer(
      header_va, eot::gpu::ResourceType::IndexBuffer));
}
