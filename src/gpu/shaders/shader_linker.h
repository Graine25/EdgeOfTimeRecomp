#pragma once

#include <rex/types.h>
#include <vector>

namespace eot::gpu {

std::vector<u8> LinkSpecConstant(const u8 *libraryDxil, u32 libraryDxilSize,
                                 bool isPixelShader, u32 specConstants);

const char *LinkedEntryPointName();

}
