#include "gpu/texture_replace.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/gpu_timing.h"
#include "gpu/resources.h"
#include "gpu/settings.h"

#include "embedded.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
namespace fs = std::filesystem;
using rex::graphics::FormatInfo;
using rex::graphics::TextureInfo;
using F = plume::RenderFormat;

constexpr const char *kDir = "textures";
constexpr const char *kDumpDir = "textures/dump";

struct HostFmt {
  F resource = F::UNKNOWN;
  F view = F::UNKNOWN;
  F viewSrgb = F::UNKNOWN;
  u32 bpb = 0;
  u32 blockDim = 1;
  bool bc = false;
  bool ok = false;
};

HostFmt Bc(F typeless, F unorm, F srgb, u32 bpb) {
  return HostFmt{typeless, unorm, srgb, bpb, 4, true, true};
}

struct Replacement {
  std::string path;
  std::vector<u8> bytes;
  const u8 *blocks = nullptr;
  size_t blocksSize = 0;
  HostFmt fmt;
  u32 width = 0;
  u32 height = 0;
  u32 mips = 1;
};

struct Registry {
  std::once_flag once;
  bool active = false;
  bool dumping = false;
  std::mutex mutex;
  std::unordered_map<u64, Replacement> byHash;
  std::unordered_set<u64> dumped;
};

Registry &registry() {
  static Registry r;
  return r;
}

u32 Rd32(const u8 *p) {
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

HostFmt DxgiToHost(u32 dxgi) {
  switch (dxgi) {
  case 71: case 72: return Bc(F::BC1_TYPELESS, F::BC1_UNORM, F::BC1_UNORM_SRGB, 8);
  case 74: case 75: return Bc(F::BC2_TYPELESS, F::BC2_UNORM, F::BC2_UNORM_SRGB, 16);
  case 77: case 78: return Bc(F::BC3_TYPELESS, F::BC3_UNORM, F::BC3_UNORM_SRGB, 16);
  case 80: case 81: return HostFmt{F::BC4_UNORM, F::BC4_UNORM, F::UNKNOWN, 8, 4, true, true};
  case 83: case 84: return HostFmt{F::BC5_UNORM, F::BC5_UNORM, F::UNKNOWN, 16, 4, true, true};
  case 98: case 99: return Bc(F::BC7_TYPELESS, F::BC7_UNORM, F::BC7_UNORM_SRGB, 16);
  case 28: case 29:
    return HostFmt{F::R8G8B8A8_UNORM, F::R8G8B8A8_UNORM, F::UNKNOWN, 4, 1, false, true};
  default: return HostFmt{};
  }
}

HostFmt FourCcToHost(u32 fourcc) {
  switch (fourcc) {
  case 0x31545844: return Bc(F::BC1_TYPELESS, F::BC1_UNORM, F::BC1_UNORM_SRGB, 8);
  case 0x33545844: return Bc(F::BC2_TYPELESS, F::BC2_UNORM, F::BC2_UNORM_SRGB, 16);
  case 0x35545844: return Bc(F::BC3_TYPELESS, F::BC3_UNORM, F::BC3_UNORM_SRGB, 16);
  case 0x31495441: case 0x55344342:
    return HostFmt{F::BC4_UNORM, F::BC4_UNORM, F::UNKNOWN, 8, 4, true, true};
  case 0x32495441: case 0x55354342:
    return HostFmt{F::BC5_UNORM, F::BC5_UNORM, F::UNKNOWN, 16, 4, true, true};
  default: return HostFmt{};
  }
}

bool ParseDdsBytes(const u8 *d, size_t size, Replacement &out) {
  if (size < 128 || Rd32(d) != 0x20534444)
    return false;
  const u32 height = Rd32(d + 12), width = Rd32(d + 16);
  const u32 flags = Rd32(d + 8);
  u32 mips = (flags & 0x20000) ? Rd32(d + 28) : 1;
  mips = std::max(1u, mips);
  const u32 pf_flags = Rd32(d + 80);
  const u32 fourcc = Rd32(d + 84);
  size_t data_off = 128;
  HostFmt fmt;
  if ((pf_flags & 0x4) && fourcc == 0x30315844) {
    if (size < 148)
      return false;
    fmt = DxgiToHost(Rd32(d + 128));
    data_off = 148;
  } else if (pf_flags & 0x4) {
    fmt = FourCcToHost(fourcc);
  } else if (pf_flags & 0x40) {
    if (Rd32(d + 88) == 32)
      fmt = HostFmt{F::R8G8B8A8_UNORM, F::R8G8B8A8_UNORM, F::UNKNOWN, 4, 1, false, true};
  }
  if (!fmt.ok || data_off > size)
    return false;
  out.fmt = fmt;
  out.width = width;
  out.height = height;
  out.mips = mips;
  out.blocks = d + data_off;
  out.blocksSize = size - data_off;
  return true;
}

bool ParseDdsFile(const fs::path &path, Replacement &out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return false;
  const auto size = in.tellg();
  if (size < 128)
    return false;
  out.bytes.resize(static_cast<size_t>(size));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(out.bytes.data()), size);
  return ParseDdsBytes(out.bytes.data(), out.bytes.size(), out);
}

bool HashFromName(std::string stem, u64 &out) {
  if (stem.rfind("tex_", 0) == 0)
    stem = stem.substr(4);
  if (stem.size() < 16)
    return false;
  u64 hash = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = stem[i];
    const int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (v < 0)
      return false;
    hash = (hash << 4) | static_cast<u64>(v);
  }
  out = hash;
  return true;
}

u64 HashTexture(const GuestTexture &t, const TextureInfo &info) {
  if (!info.memory.base_address)
    return 0;
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, t.fetch, sizeof(fetch));
  const bool is_3d = info.dimension == xe::DataDimension::k3D;
  const u32 w = info.width + 1, h = info.height + 1, d = info.depth + 1;
  const tu::TextureGuestLayout layout =
      tu::GetGuestTextureLayout(info.dimension, fetch.pitch, w, h, is_3d ? d : 1u, info.is_tiled,
                                info.format, info.has_packed_mips, true, info.mip_max_level);
  const u64 span = layout.base.array_slice_stride_bytes;
  const u8 *src = mem::at<u8>(info.memory.base_address);
  if (!src || span == 0)
    return 0;
  struct {
    u32 format, w, h, tiled;
  } hdr{static_cast<u32>(info.format), w, h, info.is_tiled ? 1u : 0u};
  const u64 seed = XXH3_64bits(&hdr, sizeof(hdr));
  return XXH3_64bits_withSeed(src, span, seed);
}

u32 HostToFourcc(F view) {
  switch (view) {
  case F::BC1_UNORM: case F::BC1_UNORM_SRGB: return 0x31545844;
  case F::BC2_UNORM: case F::BC2_UNORM_SRGB: return 0x33545844;
  case F::BC3_UNORM: case F::BC3_UNORM_SRGB: return 0x35545844;
  case F::BC4_UNORM: return 0x55344342;
  case F::BC5_UNORM: return 0x32495441;
  default: return 0;
  }
}

void Wr32(u8 *p, u32 v) {
  p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24);
}

void WriteDds(const fs::path &path, F view, bool bc, u32 bpb, u32 w, u32 h, const u8 *data,
              size_t size) {
  u8 hdr[128] = {};
  Wr32(hdr, 0x20534444);
  Wr32(hdr + 4, 124);
  Wr32(hdr + 8, 0x1 | 0x2 | 0x4 | 0x1000 | (bc ? 0x80000 : 0x8));
  Wr32(hdr + 12, h);
  Wr32(hdr + 16, w);
  Wr32(hdr + 20, static_cast<u32>(size));
  Wr32(hdr + 76, 32);
  const u32 fourcc = bc ? HostToFourcc(view) : 0;
  if (fourcc) {
    Wr32(hdr + 80, 0x4);
    Wr32(hdr + 84, fourcc);
  } else {
    Wr32(hdr + 80, 0x41);
    Wr32(hdr + 88, 32);
    Wr32(hdr + 92, 0x000000ff);
    Wr32(hdr + 96, 0x0000ff00);
    Wr32(hdr + 100, 0x00ff0000);
    Wr32(hdr + 104, 0xff000000);
  }
  Wr32(hdr + 108, 0x1000);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return;
  out.write(reinterpret_cast<const char *>(hdr), sizeof(hdr));
  out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
}

bool UntileLevel0(const GuestTexture &t, const TextureInfo &info, std::vector<u8> &out, u32 &bw_out,
                  u32 &bh_out) {
  const FormatInfo *fi = info.format_info();
  const u32 bpb = fi->bytes_per_block();
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, t.fetch, sizeof(fetch));
  const u32 w = info.width + 1, h = info.height + 1;
  const tu::TextureGuestLayout layout =
      tu::GetGuestTextureLayout(info.dimension, fetch.pitch, w, h, 1u, info.is_tiled, info.format,
                                info.has_packed_mips, true, info.mip_max_level);
  const u8 *src = mem::at<u8>(info.memory.base_address);
  if (!src || !layout.base.row_pitch_bytes)
    return false;
  const u32 bw = (w + fi->block_width - 1) / fi->block_width;
  const u32 bh = (h + fi->block_height - 1) / fi->block_height;
  const u32 guest_pitch_blocks = layout.base.row_pitch_bytes / bpb;
  out.assign(u64(bw) * bh * bpb, 0);
  if (info.is_tiled && info.dimension != xe::DataDimension::k3D) {
    tc::UntileInfo ui{};
    ui.width = bw;
    ui.height = bh;
    ui.input_pitch = guest_pitch_blocks;
    ui.output_pitch = bw;
    ui.input_format_info = fi;
    ui.output_format_info = fi;
    const xe::Endian endian = info.endianness;
    ui.copy_callback = [endian](void *o, const void *i, size_t n) {
      tc::CopySwapBlock(endian, o, i, n);
    };
    tc::Untile(out.data(), src, &ui);
  } else if (!info.is_tiled) {
    for (u32 y = 0; y < bh; ++y)
      tc::CopySwapBlock(info.endianness, out.data() + u64(y) * bw * bpb,
                        src + u64(y) * layout.base.row_pitch_bytes, u64(bw) * bpb);
  } else {
    return false;
  }
  bw_out = bw;
  bh_out = bh;
  return true;
}

void Scan(Registry &r) {
  r.dumping = Settings::DumpTextures();
  if (r.dumping) {
    std::error_code ec;
    fs::create_directories(kDumpDir, ec);
  }
  if (Settings::TextureReplace()) {
    for (const eot::EmbeddedAsset &a : eot::EmbeddedGroup("textures")) {
      const std::string file = fs::path(a.name).filename().string();
      u64 hash = 0;
      if (!HashFromName(fs::path(file).stem().string(), hash))
        continue;
      Replacement rep;
      rep.path = "embedded:" + file;
      if (!ParseDdsBytes(a.data, a.size, rep))
        continue;
      EOT_INFO("[texrep] {:016x} <- embedded '{}' ({}x{}, {} mip(s))", hash, file, rep.width,
               rep.height, rep.mips);
      r.byHash[hash] = std::move(rep);
    }
    std::error_code ec;
    if (fs::is_directory(kDir, ec)) {
      for (const auto &entry : fs::directory_iterator(kDir, ec)) {
        if (!entry.is_regular_file())
          continue;
        const fs::path p = entry.path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".dds")
          continue;
        u64 hash = 0;
        if (!HashFromName(p.stem().string(), hash)) {
          EOT_WARN("[texrep] '{}' is not named by a 16-hex-digit content hash; skipped",
                   p.filename().string());
          continue;
        }
        Replacement rep;
        rep.path = p.string();
        if (!ParseDdsFile(p, rep)) {
          EOT_WARN("[texrep] '{}' did not parse as a supported DDS; skipped",
                   p.filename().string());
          continue;
        }
        EOT_INFO("[texrep] {:016x} <- '{}' ({}x{}, {} mip(s))", hash, p.filename().string(),
                 rep.width, rep.height, rep.mips);
        r.byHash[hash] = std::move(rep);
      }
    }
  }
  r.active = r.dumping || !r.byHash.empty();
  if (r.active)
    EOT_INFO("[texrep] active: {} replacement(s){}", r.byHash.size(),
             r.dumping ? ", dumping on" : "");
}

Registry &ready() {
  Registry &r = registry();
  std::call_once(r.once, [&] { Scan(r); });
  return r;
}

}

namespace texrep {

bool Active() { return ready().active; }

bool TryCreateReplacementImage(VideoState &s, GuestTexture &t, const TextureInfo &info) {
  Registry &r = ready();
  if (r.byHash.empty())
    return false;
  const u64 hash = HashTexture(t, info);
  if (!hash)
    return false;
  std::lock_guard lock(r.mutex);
  const auto it = r.byHash.find(hash);
  if (it == r.byHash.end())
    return false;
  const Replacement &rep = it->second;
  const HostFmt &f = rep.fmt;
  HostTexture &host = t.host;

  plume::RenderTextureDesc desc;
  desc.width = f.bc ? ((rep.width + 3) & ~3u) : rep.width;
  desc.height = f.bc ? ((rep.height + 3) & ~3u) : rep.height;
  desc.depth = 1;
  desc.arraySize = 1;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.mipLevels = std::max(1u, rep.mips);
  desc.committed = Settings::CommittedTextures();
  const bool srgb_capable = f.viewSrgb != F::UNKNOWN;
  desc.format = srgb_capable ? f.resource : f.view;
  host.viewFormat = srgb_capable ? (t.gammaSigned ? f.viewSrgb : f.view) : F::UNKNOWN;
  host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  host.format = desc.format;
  host.width = desc.width;
  host.height = desc.height;
  host.depth = 1;
  host.mipLevels = desc.mipLevels;
  host.arraySize = 1;
  host.isDepth = false;
  host.renderable = false;
  CreateOrRecycleHostTexture(s, host, desc, "texture-replacement");
  if (!host.texture) {
    t.uploadFailed = true;
    return false;
  }
  t.replaced = true;
  t.replacementHash = hash;
  EOT_INFO("[texrep] {:#x}: replaced with '{}' ({}x{} -> {}x{})", t.va,
           fs::path(rep.path).filename().string(), info.width + 1, info.height + 1, rep.width,
           rep.height);
  return true;
}

void UploadReplacement(VideoState &s, GuestTexture &t) {
  Registry &r = ready();
  HostTexture &host = t.host;
  if (!host.texture)
    return;
  std::lock_guard lock(r.mutex);
  const auto it = r.byHash.find(t.replacementHash);
  if (it == r.byHash.end())
    return;
  const Replacement &rep = it->second;
  const HostFmt &f = rep.fmt;

  TransitionLocked(s, host, plume::RenderTextureLayout::COPY_DEST);
  const u8 *src = rep.blocks;
  size_t remaining = rep.blocksSize;
  const u32 levels = std::min(rep.mips, host.mipLevels);
  for (u32 level = 0; level < levels; ++level) {
    const u32 w = std::max(1u, rep.width >> level);
    const u32 h = std::max(1u, rep.height >> level);
    const u32 bw = f.bc ? (w + 3) / 4 : w;
    const u32 bh = f.bc ? (h + 3) / 4 : h;
    const u64 level_bytes = u64(bw) * bh * f.bpb;
    if (level_bytes > remaining)
      break;
    const u64 host_pitch = (u64(bw) * f.bpb + kTextureRowPitchAlignment - 1) /
                           kTextureRowPitchAlignment * kTextureRowPitchAlignment;
    const u64 host_slice = (host_pitch * bh + kTexturePlacementAlignment - 1) /
                           kTexturePlacementAlignment * kTexturePlacementAlignment;
    UploadAlloc staging;
    if (!UploadAllocate(host_slice, kTexturePlacementAlignment, &staging))
      break;
    for (u32 y = 0; y < bh; ++y)
      std::memcpy(staging.cpu + y * host_pitch, src + u64(y) * bw * f.bpb, u64(bw) * f.bpb);
    const u32 row_width_texels = static_cast<u32>(host_pitch / f.bpb) * f.blockDim;
    GpuTimingMark(s, s.command_list, kGpuCatUpload);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(host.texture.get(), level, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer, f.view, w, h, 1,
                                                          row_width_texels, staging.offset),
        0, 0, 0);
    src += level_bytes;
    remaining -= level_bytes;
  }
  t.uploaded = true;
  host.needsClear = false;
}

void MaybeDumpTexture(const GuestTexture &t, const TextureInfo &info) {
  Registry &r = ready();
  if (!r.dumping)
    return;
  const u64 hash = HashTexture(t, info);
  if (!hash)
    return;
  {
    std::lock_guard lock(r.mutex);
    if (!r.dumped.insert(hash).second)
      return;
  }
  const TextureFormatMapping m = MapTextureFormat(info.format);
  if (!m.supported || m.convert)
    return;
  std::vector<u8> tight;
  u32 bw = 0, bh = 0;
  if (!UntileLevel0(t, info, tight, bw, bh))
    return;
  const FormatInfo *fi = info.format_info();
  const u32 bpb = fi->bytes_per_block();
  const std::string name = std::format("{}/tex_{:016x}_{}x{}_fmt{}.dds", kDumpDir, hash,
                                        info.width + 1, info.height + 1,
                                        static_cast<u32>(info.format));
  WriteDds(name, m.format, m.blockCompressed, bpb, info.width + 1, info.height + 1, tight.data(),
           tight.size());
  EOT_INFO("[texrep] dumped {:#x} -> '{}' ({}x{})", t.va, name, info.width + 1, info.height + 1);
}

}

}
