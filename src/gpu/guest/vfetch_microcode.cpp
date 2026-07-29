#include "gpu/guest/vfetch_microcode.h"

#include <rex/graphics/format/ucode.h>

#include <rex/graphics/xenos.h>

#include "core/memory_helpers.h"
#include "gpu/guest/d3d.h"

namespace eot::gpu {

namespace {

namespace xenos = rex::graphics::xenos;

constexpr u32 kInstructionSize = 12;

constexpr u32 kObjectMicrocodePointer = 0x20;

i32 SignExtend(u32 value, u32 bits) {
  const u32 shift = 32u - bits;
  return static_cast<i32>(value << shift) >> shift;
}

}

u32 MicrocodeAddress(const GuestShader &shader) {
  if (shader.physicalVa)
    return shader.physicalVa;

  if (!shader.objectVa)
    return 0;
  const u32 pointer =
      mem::try_load<u32>(shader.objectVa + kObjectMicrocodePointer);
  if (pointer < 0x1000 || !mem::try_at<const u8>(pointer))
    return 0;
  return pointer;
}

FetchMicrocode DecodeFetch(u32 microcode_va, const VertexFetch &fetch) {
  FetchMicrocode out;
  if (!microcode_va)
    return out;

  const u32 shape_addr =
      microcode_va +
      (fetch.miniFetch ? fetch.parentAddress : fetch.instructionAddress) *
          kInstructionSize;
  const u32 offset_addr =
      microcode_va + fetch.instructionAddress * kInstructionSize;

  if (!mem::try_at<const u8>(shape_addr) || !mem::try_at<const u8>(offset_addr))
    return out;

  const auto read = [](u32 at) {
    rex::graphics::ucode::VertexFetchInstruction insn{};
    u32 words[3] = {mem::try_load<u32>(at), mem::try_load<u32>(at + 4),
                    mem::try_load<u32>(at + 8)};
    std::memcpy(&insn, words, sizeof(insn));
    return insn;
  };

  const auto shape = read(shape_addr);
  const auto own = read(offset_addr);

  out.format = static_cast<u32>(shape.data_format());
  out.expAdjust = shape.exp_adjust();
  out.normalized = shape.is_normalized();
  out.signedValue = shape.is_signed();
  out.stride = shape.stride() * 4u;
  out.offset = static_cast<u32>(own.offset() * 4);
  out.valid = out.stride != 0 && out.format != 0;
  return out;
}

bool DecodeLayoutFromMicrocode(const GuestShader &shader,
                               const VertexLayout &layout, FetchMicrocode *out,
                               u32 &stride_out) {
  const u32 microcode = MicrocodeAddress(shader);
  if (!microcode || layout.count == 0)
    return false;

  u32 stride = 0;
  for (u32 i = 0; i < layout.count; ++i) {
    out[i] = DecodeFetch(microcode, layout.fetches[i]);
    if (!out[i].valid)
      return false;
    if (stride == 0)
      stride = out[i].stride;
    else if (stride != out[i].stride)
      return false;
  }

  stride_out = stride;
  return true;
}

}
