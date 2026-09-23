#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace eot::mods {

enum class ModType { kPackage, kReplacement, kModel };

const char *TypeName(ModType type);
bool TypeFromName(std::string_view name, ModType &out);

struct Manifest {
  std::string name;
  std::string creator;
  std::string version;
  std::string description;
  ModType type = ModType::kReplacement;
  std::string file;
  uint32_t package_id = 0;
  std::string language;
  std::string language_name;

  bool IsLanguage() const { return type == ModType::kPackage && !language.empty(); }
};

inline constexpr const char *kManifestFileName = "mod.toml";

bool ParseManifest(std::string_view text, Manifest &out, std::string &error);

std::string WriteManifest(const Manifest &manifest);

}
