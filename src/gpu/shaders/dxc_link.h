#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eot::gpu {

std::vector<uint8_t> CompileSpecConstantLib(uint32_t value,
                                            std::string *error = nullptr);

std::vector<uint8_t> LinkSpecConstantLib(const uint8_t *libraryDxil,
                                         uint32_t libraryDxilSize,
                                         const uint8_t *specLib,
                                         size_t specLibSize,
                                         const wchar_t *profile,
                                         const wchar_t *entry,
                                         std::string *error = nullptr);

}
