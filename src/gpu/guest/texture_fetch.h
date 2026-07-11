#pragma once

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

struct GuestTextureFetch {
  u32 baseAddress = 0;
  u32 mipAddress = 0;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 mipLevels = 1;
  u32 pitch = 0;
  u32 pitchTiles = 0;
  bool tiled = false;
  rex::graphics::xenos::TextureFormat format{};
  rex::graphics::xenos::Endian endianness{};
  rex::graphics::xenos::DataDimension dimension{};
};

bool DecodeTextureFetch(u32 device_va, u32 sampler, GuestTextureFetch &out);

bool DecodeTextureFetchAt(u32 fetch_va, GuestTextureFetch &out);

plume::RenderFormat HostFormatForTextureFormat(
    rex::graphics::xenos::TextureFormat format);

void NoteTextureFetch(const GuestTextureFetch &fetch);
void LogTextureFetchCensus();

}
