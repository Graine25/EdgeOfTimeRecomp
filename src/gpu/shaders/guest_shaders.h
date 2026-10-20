// gpu/shaders/guest_shaders.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

struct ShaderCacheEntry;

namespace eot::gpu {

struct VideoState;

constexpr u32 kSpecR11G11B10Normal = 1u << 0;
constexpr u32 kSpecAlphaTest = 1u << 1;
constexpr u32 kSpecSintTexcoord = 1u << 2;
constexpr u32 kSpecSpecularAA = 1u << 5;

bool GuestShadersInit();
u32 GuestShaderCacheCount();

GuestShader *RegisterGuestShader(VideoState &s, u32 object_va, bool is_pixel);
GuestShader *FindGuestShader(VideoState &s, u32 object_va);
void DrainShaderGraveyardLocked(VideoState &s);

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash);
u64 CanonicalShaderHash(u64 hash);
void VertexInputsFromEntry(const ShaderCacheEntry &e, std::vector<VertexInput> &out);
// Fast and FastVelocity are a pixel shader's twins without the gamma decode and the alpha test.
enum class VsVariant : u8 { Trimmed = 0, Full = 1, PositionOnly = 2, Velocity = 3, Fast = 4, FastVelocity = 5 };
VsVariant VsVariantFor(const ShaderCacheEntry *vs, const ShaderCacheEntry *ps, bool null_ps,
                       bool velocity = false);
bool EntryHasVelocity(const ShaderCacheEntry *e);

// A host-only spec bit: the pipeline is built with the pixel shader's fast twin.
constexpr u32 kSpecFastPs = 1u << 31;
enum class FastPs : u8 { Off, Lights, All };
FastPs FastPsFromSetting();
bool FastPsWanted(FastPs mode, u64 ps_hash);
bool EntryHasFast(const ShaderCacheEntry *e, bool velocity);

plume::RenderShader *GetHostShaderByHash(VideoState &s, u64 hash, u32 spec_mask, bool is_pixel,
                                         bool worker = false,
                                         VsVariant variant = VsVariant::Trimmed);

plume::RenderShader *ResolveHostShader(VideoState &s, GuestShader &shader, u32 spec_mask,
                                       VsVariant variant = VsVariant::Trimmed);

}
