#pragma once

#include <rex/types.h>

#include "gpu/guest/resources.h"
#include "gpu/pipeline/vertex_layout.h"

namespace eot::gpu {

struct FetchMicrocode {
  u32 offset = 0;
  u32 stride = 0;
  u32 format = 0;
  i32 expAdjust = 0;
  bool signedValue = false;
  bool normalized = false;
  bool valid = false;
};

u32 MicrocodeAddress(const GuestShader &shader);

FetchMicrocode DecodeFetch(u32 microcode_va, const VertexFetch &fetch);

bool DecodeLayoutFromMicrocode(const GuestShader &shader,
                               const VertexLayout &layout,
                               FetchMicrocode *out, u32 &stride_out);

}
