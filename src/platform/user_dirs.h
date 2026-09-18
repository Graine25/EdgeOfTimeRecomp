#pragma once

#include <filesystem>

#if !defined(_WIN32)

namespace eot::platform {

std::filesystem::path HomeDir();

std::filesystem::path ConfigHome();
std::filesystem::path DataHome();
std::filesystem::path StateHome();

std::filesystem::path RuntimeDir();

std::filesystem::path UserDir(const char *key, const char *fallback);

}

#endif
