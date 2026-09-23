#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mods/mod_manifest.h"

namespace eot::mods {

inline constexpr const char *kModsFolderName = "mods";

struct Mod {
  std::string folder;
  Manifest manifest;
  bool enabled = true;
  bool active = false;
  bool bundled = false;
  std::string status;
};

void Initialize(const std::filesystem::path &install_root, const std::filesystem::path &game,
                const std::filesystem::path &profile);
bool Ready();

std::vector<Mod> List();

const Mod *Find(const std::vector<Mod> &mods, std::string_view name);

bool Add(const std::filesystem::path &path, std::string &message);

bool Remove(std::string_view name, std::string &message);

bool SetEnabled(std::string_view name, bool enabled, std::string &message);

struct PackageToLoad {
  uint32_t id;
  std::string name;
  std::string language;
  std::string mod;
};
std::vector<PackageToLoad> PackagesToLoad();

struct LanguageMod {
  std::string folder;
  std::string name;
  std::string tag;
  std::string language_name;
  bool asked;
};
std::vector<LanguageMod> LanguageMods();
void LanguageAsked(std::string_view folder);

std::filesystem::path ModsDir();

}
