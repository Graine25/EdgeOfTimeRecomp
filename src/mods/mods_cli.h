#pragma once

#include <string>
#include <vector>

namespace eot::mods {

inline constexpr const char *kCliCommand = "mods";

int RunCli(const std::vector<std::string> &positional);

}
