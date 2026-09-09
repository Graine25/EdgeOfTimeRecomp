#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace eot::installer {

constexpr int kInstallSchemaVersion = 1;

struct InstallConfig {
  std::filesystem::path install_root;
  std::string disc_fingerprint;
  int schema_version = 0;
  std::string app_version;

  std::filesystem::path game_data_path() const { return install_root / "game"; }
};

std::optional<InstallConfig> ReadInstallRegistry();

bool InstallIsPresent(const InstallConfig &config);

bool WriteInstallRegistry(const InstallConfig &config);

bool ClearInstallRegistry();

}
