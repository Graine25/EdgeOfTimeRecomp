#pragma once

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

struct GuestTextureFetch {
  i32 expAdjust = 0;
  u32 baseAddress = 0;
  bool physicalAddress = false;
  u32 mipAddress = 0;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 mipLevels = 1;
  u32 pitch = 0;
  u32 pitchTiles = 0;
  bool tiled = false;
  bool packedMips = false;
  rex::graphics::xenos::TextureFormat format{};

  rex::graphics::xenos::ClampMode clampX{};
  rex::graphics::xenos::ClampMode clampY{};
  rex::graphics::xenos::ClampMode clampZ{};
  rex::graphics::xenos::TextureFilter magFilter{};
  rex::graphics::xenos::TextureFilter minFilter{};
  rex::graphics::xenos::TextureFilter mipFilter{};
  bool borderWhite = false;
  rex::graphics::xenos::Endian endianness{};
  rex::graphics::xenos::DataDimension dimension{};
};

bool DecodeTextureFetch(u32 device_va, u32 sampler, GuestTextureFetch &out);

bool DecodeTextureFetchAt(u32 fetch_va, GuestTextureFetch &out);

bool DecodeTextureFetchWords(const u32 *words, GuestTextureFetch &out);

void RegisterCanonicalTexture(u32 texture_va, bool replace);

void RegisterCanonicalTexturePool(u32 record_va);

void RetireCanonicalTexture(u32 texture_va);

bool DecodeTextureObjectFetch(u32 texture_va, GuestTextureFetch &out);

void LogCanonicalTextureStats();

plume::RenderFormat HostFormatForTextureFormat(
    rex::graphics::xenos::TextureFormat format);

u32 PhysicalTextureKey(u32 address, bool already_physical);

void NoteTextureFetch(const GuestTextureFetch &fetch);
void LogTextureFetchCensus();

}
