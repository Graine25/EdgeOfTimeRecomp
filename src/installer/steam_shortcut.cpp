#include "installer/steam_shortcut.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <vdflib.h>

#include "core/logging.h"
#include "embedded.h"
#include "platform/process.h"

namespace eot::installer {
namespace {

namespace fs = std::filesystem;

constexpr const char *kSteamName = "Spider-Man: Edge of Time Recompiled";

fs::path LaunchTarget(const fs::path &install) {
#if defined(_WIN32)
  return fs::absolute(install) / platform::kExecutableFileName;
#else
  (void)install;
  fs::path program = platform::LaunchPath();
#if defined(__APPLE__)
  const fs::path contents = program.parent_path().parent_path();
  if (contents.filename() == "Contents" && contents.parent_path().extension() == ".app")
    program = contents.parent_path();
#endif
  return program;
#endif
}

void WriteAsset(const fs::path &path, const EmbeddedAsset &asset) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(asset.data), static_cast<std::streamsize>(asset.size));
  out.close();
  if (!out)
    throw std::runtime_error("could not write " + path.string());
}

fs::path InstallArtwork(const fs::path &grid, uint32_t app_id) {
  struct Art {
    EmbeddedAsset asset;
    vdflib::ArtworkSlot slot;
  };
  constexpr Art kArt[] = {
      {Embedded("installer/steamgrid/cover.jpg"), vdflib::ArtworkSlot::Portrait},
      {Embedded("installer/steamgrid/capsule.jpg"), vdflib::ArtworkSlot::Capsule},
      {Embedded("installer/steamgrid/hero.jpg"), vdflib::ArtworkSlot::Hero},
      {Embedded("installer/steamgrid/logo.png"), vdflib::ArtworkSlot::Logo},
  };
  fs::create_directories(grid);
  for (const Art &art : kArt) {
    const std::string extension = fs::path(art.asset.name).extension().string();
    WriteAsset(grid / vdflib::artworkFileName(app_id, art.slot, extension), art.asset);
  }
  const fs::path icon = grid / (std::to_string(app_id) + "_icon.png");
  WriteAsset(icon, Embedded("installer/steamgrid/icon.png"));
  return icon;
}

}

std::string AddToSteam(const fs::path &install) {
  try {
    const auto steam = vdflib::findSteamInstallPath();
    if (!steam)
      return "Steam was not found, so the game was not added to it.";
    const auto users = vdflib::listLocalSteamUserIds(*steam);
    if (users.empty())
      return "No one has signed in to Steam on this computer, so the game was not added to it.";
    const std::string &user = users.front();

    const fs::path target = LaunchTarget(install);
    vdflib::Shortcut shortcut =
        vdflib::Shortcut::create(kSteamName, target.string(), target.parent_path().string());
    const uint32_t app_id = shortcut.appid;
    const std::string icon = InstallArtwork(vdflib::getGridDirectory(*steam, user), app_id).string();

    vdflib::ShortcutRepository shortcuts(vdflib::getShortcutsVdfPath(*steam, user));
    shortcuts.load();
    if (vdflib::Shortcut *existing = shortcuts.findByAppId(app_id)) {
      if (existing->icon != icon) {
        existing->icon = icon;
        shortcuts.save();
      }
    } else {
      shortcut.icon = icon;
      shortcuts.addShortcut(std::move(shortcut));
      shortcuts.save();
    }
    EOT_INFO("[install] added to Steam for user {} (app {:#x}): {}", user, app_id, target.string());
    return "Added to Steam. Restart Steam to see it in the library.";
  } catch (const std::exception &e) {
    EOT_WARN("[install] could not add the game to Steam: {}", e.what());
    return std::string("Could not add the game to Steam: ") + e.what();
  }
}

}
