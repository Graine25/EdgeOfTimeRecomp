#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eot::gpu {

std::vector<uint8_t> CompileSpecConstantLib(uint32_t value);

std::vector<uint8_t> LinkSpecConstantLib(const uint8_t *library_dxil, uint32_t library_dxil_size,
                                         const uint8_t *spec_lib, size_t spec_lib_size,
                                         const wchar_t *profile,
                                         std::string *error_out = nullptr);

}
