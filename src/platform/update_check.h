#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace eot::platform {

struct InstallUpdate {
  std::string stamp;
  std::string version;
  std::string location;
};

void UpdateInstalledCopy(const std::filesystem::path &install_root);

std::optional<InstallUpdate> LastInstallUpdate();

}
