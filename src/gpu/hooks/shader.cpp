#include <atomic>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/resources.h"
#include "gpu/shaders/guest_shaders.h"

namespace {

using eot::gpu::GuestShader;
using eot::gpu::ResourceType;

const char *StageName(ResourceType type) {
  return type == ResourceType::PixelShader ? "PS" : "VS";
}

bool ContainerType(u32 container_va, ResourceType &out) {
  const auto *c =
      eot::mem::try_at<const eot::gpu::ShaderContainer>(container_va);
  if (!c)
    return false;
  const u32 flags = c->Flags;
  if ((flags & eot::gpu::kShaderContainerMagicMask) !=
      eot::gpu::kShaderContainerMagic)
    return false;
  out = (flags & eot::gpu::kShaderContainerVertexBit)
            ? ResourceType::VertexShader
            : ResourceType::PixelShader;
  return true;
}

void RegisterCreatedShader(u32 function_va, u32 object_va) {
  ResourceType type = ResourceType::VertexShader;
  if (object_va && ContainerType(function_va, type))
    eot::gpu::RegisterShaderObject(object_va, type, function_va,
                                   0);
}

void RegisterStreamShader(u32 node_va, ResourceType type) {
  if (!node_va)
    return;
  const u32 object_field = type == ResourceType::PixelShader
                               ? eot::gpu::kShaderNodePixelObject
                               : eot::gpu::kShaderNodeVertexObject;
  const u32 object_va = eot::mem::try_load<u32>(node_va + object_field);
  const u32 physical_va =
      eot::mem::try_load<u32>(node_va + eot::gpu::kShaderNodePhysicalPayload);

  static std::atomic<u32> traced{0};
  if (traced.fetch_add(1, std::memory_order_relaxed) < 6) {
    EOT_INFO("[shader] {} stream node 0x{:08X}: object=0x{:08X} "
             "physical=0x{:08X}",
             StageName(type), node_va, object_va, physical_va);
  }
  if (!object_va)
    return;
  eot::gpu::RegisterShaderObject(
      object_va, type, object_va + eot::gpu::ShaderContainerOffset(type),
      physical_va);
}

std::atomic<u32> g_unresolved[2];

void BindShader(u32 shader_va, ResourceType type) {
  auto *gs = eot::gpu::ResolveGuestShader(shader_va);
  const bool is_pixel = type == ResourceType::PixelShader;
  if (shader_va && !gs &&
      g_unresolved[is_pixel].fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_WARN("[shader] bound {} 0x{:08X} was never registered - it came from a "
             "creation path with no hook",
             StageName(type), shader_va);
  }
  if (is_pixel)
    eot::gpu::Video::SetPixelShader(gs);
  else
    eot::gpu::Video::SetVertexShader(gs);
}

}

REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const u32 function_va = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  RegisterCreatedShader(function_va, ctx.r3.u32);
}

REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const u32 function_va = ctx.r3.u32;
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  RegisterCreatedShader(function_va, ctx.r3.u32);
}

REX_EXTERN(__imp__eot_ShaderStream_LoadVertexShaderCached);
REX_HOOK_RAW(eot_ShaderStream_LoadVertexShaderCached) {
  __imp__eot_ShaderStream_LoadVertexShaderCached(ctx, base);
  RegisterStreamShader(ctx.r3.u32, ResourceType::VertexShader);
}

REX_EXTERN(__imp__eot_ShaderStream_LoadPixelShaderCached);
REX_HOOK_RAW(eot_ShaderStream_LoadPixelShaderCached) {
  __imp__eot_ShaderStream_LoadPixelShaderCached(ctx, base);
  RegisterStreamShader(ctx.r3.u32, ResourceType::PixelShader);
}

REX_EXTERN(__imp__D3DDevice_SetVertexShader);
REX_HOOK_RAW(D3DDevice_SetVertexShader) {
  const u32 shader_va = ctx.r4.u32;
  __imp__D3DDevice_SetVertexShader(ctx, base);
  BindShader(shader_va, ResourceType::VertexShader);
}

REX_EXTERN(__imp__D3DDevice_SetPixelShader);
REX_HOOK_RAW(D3DDevice_SetPixelShader) {
  const u32 shader_va = ctx.r4.u32;
  __imp__D3DDevice_SetPixelShader(ctx, base);
  BindShader(shader_va, ResourceType::PixelShader);
}
