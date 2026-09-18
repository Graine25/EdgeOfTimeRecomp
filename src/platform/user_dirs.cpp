#include "platform/user_dirs.h"

#if !defined(_WIN32)

#include <cstdlib>
#include <fstream>
#include <string>

namespace eot::platform {
namespace {

namespace fs = std::filesystem;

fs::path EnvOr(const char *name, const fs::path &fallback) {
  const char *value = std::getenv(name);
  return (value && *value) ? fs::path(value) : fallback;
}

fs::path BaseDir(const char *variable, const char *relative) {
  if (const char *value = std::getenv(variable); value && *value)
    return fs::path(value);
  const fs::path home = HomeDir();
  return home.empty() ? fs::path() : home / relative;
}

}

fs::path HomeDir() { return EnvOr("HOME", fs::path()); }
fs::path ConfigHome() { return BaseDir("XDG_CONFIG_HOME", ".config"); }
fs::path DataHome() { return BaseDir("XDG_DATA_HOME", ".local/share"); }
fs::path StateHome() { return BaseDir("XDG_STATE_HOME", ".local/state"); }
fs::path RuntimeDir() { return EnvOr("XDG_RUNTIME_DIR", "/tmp"); }

fs::path UserDir(const char *key, const char *fallback) {
  const fs::path home = HomeDir();
  if (home.empty())
    return {};
  const fs::path config = ConfigHome();
  std::ifstream in(config / "user-dirs.dirs");
  const std::string prefix = std::string(key) + "=";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) != 0)
      continue;
    std::string value = line.substr(prefix.size());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    if (value.rfind("$HOME", 0) == 0)
      value = home.string() + value.substr(5);
    if (!value.empty())
      return fs::path(value);
  }
  return home / fallback;
}

}

#endif
