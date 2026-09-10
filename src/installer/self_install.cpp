#include "installer/self_install.h"

#include <iterator>

#include <rex/filesystem.h>

#include "core/logging.h"
#include "installer/program_files.h"

namespace eot::installer {

namespace fs = std::filesystem;

std::vector<std::string> MissingProgramFiles() {
  const fs::path here = rex::filesystem::GetExecutableFolder();
  std::vector<std::string> missing;
  std::error_code ec;
  for (const char *rel : kProgramFiles)
    if (!fs::exists(here / rel, ec))
      missing.push_back(rel);
  return missing;
}

bool CopyProgramTo(const fs::path &install, std::string &error) {
  const fs::path here = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  if (!here.empty() && fs::equivalent(here, install, ec))
    return true;

  size_t copied = 0;
  for (const char *rel : kProgramFiles) {
    const fs::path src = here / rel;
    const fs::path dst = install / rel;
    if (!fs::exists(src, ec)) {
      error = std::string(rel) + " is not beside " + here.string();
      return false;
    }
    fs::create_directories(dst.parent_path(), ec);
    if (fs::is_directory(src, ec)) {
      fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    } else {
      fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
      error = src.string() + " -> " + dst.string() + " (" + ec.message() + ")";
      return false;
    }
    ++copied;
  }
  EOT_INFO("[install] copied {} program files into {}", copied, install.string());
  return true;
}

void SyncPortPackages(const fs::path &game) {
  const fs::path here = rex::filesystem::GetExecutableFolder() / "pkz";
  std::error_code ec;
  if (!fs::is_directory(here, ec))
    return;
  for (const auto &it : fs::directory_iterator(here, ec)) {
    if (!it.is_regular_file() || it.path().extension() != ".pkz")
      continue;
    const fs::path dst = game / "Data" / it.path().filename();
    if (fs::exists(dst, ec) && fs::file_size(dst, ec) == fs::file_size(it.path(), ec))
      continue;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(it.path(), dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
      EOT_WARN("[install] could not refresh {} in {}: {}", it.path().filename().string(), dst.parent_path().string(),
               ec.message());
    else
      EOT_INFO("[install] {} refreshed in {}", it.path().filename().string(), dst.parent_path().string());
  }
}

void AdoptLegacyUserData(const fs::path &profile) {
  const fs::path legacy = rex::filesystem::GetUserFolder() / "reeot";
  std::error_code ec;
  if (!fs::is_directory(legacy, ec) || fs::equivalent(legacy, profile, ec))
    return;
  size_t adopted = 0;
  for (const auto &it : fs::directory_iterator(legacy, ec)) {
    const std::string name = it.path().filename().string();
    if (!it.is_directory() || name == "cache" || name.find('.') != std::string::npos)
      continue;
    const fs::path dst = profile / name;
    if (fs::exists(dst, ec))
      continue;
    fs::copy(it.path(), dst, fs::copy_options::recursive, ec);
    if (ec) {
      EOT_WARN("[install] could not adopt {} from {}: {}", name, legacy.string(), ec.message());
      ec.clear();
      continue;
    }
    ++adopted;
  }
  if (adopted)
    EOT_INFO("[install] adopted {} folder(s) of saves from {} into {}", adopted, legacy.string(), profile.string());
}

}
