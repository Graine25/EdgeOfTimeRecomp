#define EOT_MODS_HOST
#include "mods/mod_manager.h"

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/string/utf8.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/logging.h"
#include "embedded.h"
#include "embedded_russian_package.h"
#if defined(REEOT_BUNDLED_2099)
#include "embedded_2099_package.h"
#endif
#include "goliath/ui/menu_handles.h"
#include "installer/disc_install.h"
#include "mods/mods_api.h"

namespace eot::mods {
namespace {

namespace fs = std::filesystem;

constexpr const char *kStateFileName = "mods.toml";
constexpr const char *kBackupFolder = "backup";
constexpr const char *kDataFolder = "Data";
constexpr const char *kMarketplaceXuid = "0000000000000000";
constexpr const char *kHeadersDir = "Headers";
constexpr const char *kContentPrefix = "Mod_";
constexpr const char *kLegacyModelContent = "Mod_Model";
constexpr uint32_t kFirstDlcPackageId = 0xBB9;
constexpr uint32_t kLastDlcPackageId = 0xBBA;
constexpr uint32_t kDlcPackageIdBase = 0xBB8;
constexpr uint32_t kPortPackageIds[] = {eot::ui::kReeotPackageId, eot::ui::kReeotMenuPackageId,
                                        eot::ui::kReeotAchievementsPackageId, eot::ui::kReeotIconsPackageId};

struct Bundled {
  const char *folder;
  EmbeddedAsset manifest;
  EmbeddedAsset file;
};
const Bundled kBundled[] = {
    {"russian", Embedded("mods/russian/mod.toml"), EmbeddedRussianPackage()},
#if defined(REEOT_BUNDLED_2099)
    {"spiderverse-2099", Embedded("mods/spiderverse-2099/mod.toml"), Embedded2099Package()},
#endif
};

std::mutex g_mutex;
fs::path g_install, g_game, g_profile;
bool g_ready = false;
std::vector<Mod> g_mods;

struct State {
  bool disabled = false;
  bool removed = false;
  bool asked = false;
};
std::map<std::string, State> g_state;

std::atomic<int32_t> g_import_state{EOT_MODS_IDLE};
std::string g_import_message;
std::thread g_import_thread;

std::string Utf8(const fs::path &path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}
fs::path PathFromUtf8(std::string_view text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

std::string Lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

fs::path ModsDirLocked() { return g_install / kModsFolderName; }
fs::path BackupDir() { return ModsDirLocked() / kBackupFolder; }
fs::path GameDataDir() { return g_game / kDataFolder; }
fs::path CustomDir() { return GameDataDir() / eot::ui::kReeotPackageFolder; }
fs::path ModDir(const std::string &folder) { return ModsDirLocked() / PathFromUtf8(folder); }
fs::path ModFile(const Mod &mod) { return ModDir(mod.folder) / PathFromUtf8(mod.manifest.file); }

fs::path ContentDir() {
  return g_profile / kMarketplaceXuid / std::format("{:08X}", installer::kTitleId) /
         std::format("{:08X}", installer::kContentTypeMarketplace);
}
fs::path HeadersDir() {
  return g_profile / kMarketplaceXuid / std::format("{:08X}", installer::kTitleId) / kHeadersDir /
         std::format("{:08X}", installer::kContentTypeMarketplace);
}
std::string ContentFolderName(const Mod &mod) { return kContentPrefix + mod.folder; }

bool CopyFile(const fs::path &from, const fs::path &to, std::string &error) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    error = std::format("could not copy {} to {}: {}", Utf8(from), Utf8(to), ec.message());
    return false;
  }
  return true;
}

bool WriteFile(const fs::path &path, std::span<const uint8_t> bytes, std::string &error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || !out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    error = std::format("could not write {}", Utf8(path));
    return false;
  }
  return true;
}

bool WriteText(const fs::path &path, const std::string &text, std::string &error) {
  return WriteFile(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()),
                   error);
}

std::string ReadText(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool SameBytes(const fs::path &a, const fs::path &b) {
  std::error_code ec;
  const uintmax_t sa = fs::file_size(a, ec);
  if (ec)
    return false;
  const uintmax_t sb = fs::file_size(b, ec);
  if (ec || sa != sb)
    return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb)
    return false;
  char ba[65536], bb[65536];
  while (fa && fb) {
    fa.read(ba, sizeof(ba));
    fb.read(bb, sizeof(bb));
    if (fa.gcount() != fb.gcount() || std::memcmp(ba, bb, static_cast<size_t>(fa.gcount())) != 0)
      return false;
  }
  return true;
}

bool SameBytes(const fs::path &a, std::span<const uint8_t> bytes) {
  std::error_code ec;
  if (fs::file_size(a, ec) != bytes.size() || ec)
    return false;
  std::ifstream fa(a, std::ios::binary);
  char buffer[65536];
  size_t at = 0;
  while (fa && at < bytes.size()) {
    fa.read(buffer, sizeof(buffer));
    const size_t n = static_cast<size_t>(fa.gcount());
    if (std::memcmp(buffer, bytes.data() + at, n) != 0)
      return false;
    at += n;
  }
  return at == bytes.size();
}

bool ReadHead(const fs::path &path, unsigned char *head, size_t size) {
  std::ifstream in(path, std::ios::binary);
  std::memset(head, 0, size);
  return in && in.read(reinterpret_cast<char *>(head), static_cast<std::streamsize>(size)).gcount() > 0;
}

uint32_t ReadBigEndian32(const unsigned char *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

std::string FolderNameFor(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u))
      out += static_cast<char>(std::tolower(u));
    else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !out.empty() && out.back() != '-')
      out += '-';
    if (out.size() >= 32)
      break;
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out.empty() ? "mod" : out;
}

void LoadState() {
  g_state.clear();
  const fs::path path = ModsDirLocked() / kStateFileName;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec))
    return;
  toml::table table;
  try {
    table = toml::parse(ReadText(path));
  } catch (const toml::parse_error &e) {
    EOT_WARN("[mods] {} line {}: {}; every mod counts as on", Utf8(path), e.source().begin.line, e.description());
    return;
  }
  for (const auto &[key, node] : table) {
    const toml::table *entry = node.as_table();
    if (!entry)
      continue;
    State state;
    if (const toml::node *n = entry->get("enabled"))
      if (const auto *b = n->as_boolean())
        state.disabled = !b->get();
    if (const toml::node *n = entry->get("removed"))
      if (const auto *b = n->as_boolean())
        state.removed = b->get();
    if (const toml::node *n = entry->get("asked"))
      if (const auto *b = n->as_boolean())
        state.asked = b->get();
    g_state[std::string(key.str())] = state;
  }
}

void SaveState() {
  std::string text = "# Written by reeot: the mods switched off, the port's own mods removed, and the\n"
                     "# language mods whose language was offered at boot. A mod folder not listed here is on.\n";
  for (const auto &[folder, state] : g_state) {
    if (!state.disabled && !state.removed && !state.asked)
      continue;
    text += std::format("\n[\"{}\"]\n", folder);
    if (state.disabled)
      text += "enabled = false\n";
    if (state.removed)
      text += "removed = true\n";
    if (state.asked)
      text += "asked = true\n";
  }
  std::error_code ec;
  fs::create_directories(ModsDirLocked(), ec);
  std::ofstream out(ModsDirLocked() / kStateFileName, std::ios::binary | std::ios::trunc);
  out << text;
}

void ExtractBundled() {
  for (const Bundled &bundled : kBundled) {
    const auto state = g_state.find(bundled.folder);
    if (state != g_state.end() && state->second.removed)
      continue;
    Manifest manifest;
    std::string error;
    if (!ParseManifest(bundled.manifest.text(), manifest, error)) {
      EOT_WARN("[mods] the bundled {} manifest: {}", bundled.folder, error);
      continue;
    }
    const fs::path dir = ModDir(bundled.folder);
    const fs::path manifest_path = dir / kManifestFileName;
    const fs::path file_path = dir / PathFromUtf8(manifest.file);
    std::error_code ec;
    bool written = false;
    if (!fs::is_regular_file(manifest_path, ec) || ReadText(manifest_path) != bundled.manifest.text()) {
      if (WriteFile(manifest_path, bundled.manifest.bytes(), error))
        written = true;
      else
        EOT_WARN("[mods] {}", error);
    }
    if (!fs::is_regular_file(file_path, ec) || !SameBytes(file_path, bundled.file.bytes())) {
      if (WriteFile(file_path, bundled.file.bytes(), error))
        written = true;
      else
        EOT_WARN("[mods] {}", error);
    }
    if (written)
      EOT_INFO("[mods] the port's {} written to {}", manifest.name, Utf8(dir));
  }
}

bool IsBundled(std::string_view folder) {
  for (const Bundled &bundled : kBundled)
    if (folder == bundled.folder)
      return true;
  return false;
}

std::string ModelFileName(const fs::path &path, std::string &why) {
  unsigned char head[32];
  if (!ReadHead(path, head, sizeof(head))) {
    why = "could not be read";
    return {};
  }
  const std::string ext = Lower(Utf8(path.extension()));
  if (ReadBigEndian32(head) == 1 && ReadBigEndian32(head + 4) == 0x00010001) {
    const uint32_t id = ReadBigEndian32(head + 0x18);
    if (id < kFirstDlcPackageId || id > kLastDlcPackageId) {
      why = std::format("a package with id {:#x}, which is not a DLC slot (0xBB9 or 0xBBA)", id);
      return {};
    }
    return std::format("_DLC{:03}.pak", id - kDlcPackageIdBase);
  }
  if (std::memcmp(head, "\xBA\xBE\xB1\xB0", 4) == 0 || ext == ".pkz") {
    const std::string stem = Lower(Utf8(path.stem()));
    if (stem == "_dlc001" || stem == "_dlc002")
      return "_DLC" + stem.substr(4) + ".pkz";
    why = "a compressed package not named _DLC001 or _DLC002, so its slot is not known";
    return {};
  }
  why = "not a costume package (a raw DLC package or a _DLC00N.pkz)";
  return {};
}

fs::path GameDataFile(const std::string &name) {
  std::error_code ec;
  const std::string wanted = Lower(name);
  for (const auto &it : fs::directory_iterator(GameDataDir(), ec))
    if (it.is_regular_file() && Lower(Utf8(it.path().filename())) == wanted)
      return it.path();
  return {};
}

bool ManifestForFile(const fs::path &path, Manifest &out, std::string &why) {
  std::string model_why;
  const std::string model = ModelFileName(path, model_why);
  Manifest m;
  m.name = Utf8(path.stem());
  m.creator = "unknown";
  if (!model.empty()) {
    m.type = ModType::kModel;
    m.file = model;
  } else if (!GameDataFile(Utf8(path.filename())).empty()) {
    m.type = ModType::kReplacement;
    m.file = Utf8(path.filename());
  } else {
    why = std::format("{} is neither a costume package ({}) nor named like a file in the game's Data folder; a "
                      "new package needs a folder with a mod.toml that gives its id",
                      Utf8(path.filename()), model_why);
    return false;
  }
  out = std::move(m);
  return true;
}

void MigrateSlotFile(const fs::path &file, const char *slot) {
  std::error_code ec;
  for (const Bundled &bundled : kBundled) {
    if (SameBytes(file, bundled.file.bytes())) {
      EOT_INFO("[mods] the {} slot's {} is the port's own {}: dropped for the bundled copy", slot,
               Utf8(file.filename()), bundled.folder);
      fs::remove(file, ec);
      return;
    }
  }
  Manifest manifest;
  std::string why;
  if (!ManifestForFile(file, manifest, why)) {
    EOT_WARN("[mods] the {} slot's {} was not carried over: {}", slot, Utf8(file.filename()), why);
    return;
  }
  const std::string folder = FolderNameFor(manifest.name);
  const fs::path dir = ModDir(folder);
  std::string error;
  if (!WriteText(dir / kManifestFileName, WriteManifest(manifest), error) ||
      !CopyFile(file, dir / PathFromUtf8(manifest.file), error)) {
    EOT_WARN("[mods] the {} slot's {}: {}", slot, Utf8(file.filename()), error);
    return;
  }
  fs::remove(file, ec);
  EOT_INFO("[mods] the {} slot's {} is now the mod {} ({})", slot, Utf8(file.filename()), folder,
           TypeName(manifest.type));
}

void MigrateSlots() {
  std::error_code ec;
  for (const char *slot : {"pak", "model"}) {
    const fs::path dir = ModsDirLocked() / slot;
    if (!fs::is_directory(dir, ec))
      continue;
    for (const auto &it : fs::directory_iterator(dir, ec))
      if (it.is_regular_file())
        MigrateSlotFile(it.path(), slot);
    fs::remove(dir, ec);
  }
  const fs::path binary = ModsDirLocked() / "binary";
  if (fs::is_directory(binary, ec)) {
    for (const auto &it : fs::directory_iterator(binary, ec)) {
      const fs::path backup = BackupDir() / it.path().filename();
      const fs::path target = g_install / it.path().filename();
      if (fs::is_regular_file(backup, ec)) {
        fs::copy_file(backup, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
          ec.clear();
          fs::rename(target, BackupDir() / (Utf8(it.path().filename()) + ".old"), ec);
          fs::copy_file(backup, target, fs::copy_options::overwrite_existing, ec);
        }
        if (!ec) {
          fs::remove(backup, ec);
          EOT_INFO("[mods] {} put back; the binary slot is gone", Utf8(target));
        } else {
          EOT_WARN("[mods] {} could not be put back: {}", Utf8(target), ec.message());
          continue;
        }
      }
      fs::remove(it.path(), ec);
    }
    fs::remove(binary, ec);
  }
  const fs::path legacy = ContentDir() / kLegacyModelContent;
  if (fs::is_directory(legacy, ec)) {
    fs::remove_all(legacy, ec);
    fs::remove(HeadersDir() / (std::string(kLegacyModelContent) + ".header"), ec);
    EOT_INFO("[mods] content {} withdrawn: model mods publish under their own names", kLegacyModelContent);
  }
}

void Scan() {
  g_mods.clear();
  std::error_code ec;
  for (const auto &it : fs::directory_iterator(ModsDirLocked(), ec)) {
    if (!it.is_directory())
      continue;
    const fs::path manifest_path = it.path() / kManifestFileName;
    if (!fs::is_regular_file(manifest_path, ec))
      continue;
    Mod mod;
    mod.folder = Utf8(it.path().filename());
    std::string error;
    if (!ParseManifest(ReadText(manifest_path), mod.manifest, error)) {
      EOT_WARN("[mods] {}/mod.toml: {}", mod.folder, error);
      continue;
    }
    const auto state = g_state.find(mod.folder);
    mod.enabled = state == g_state.end() || !state->second.disabled;
    mod.bundled = IsBundled(mod.folder);
    if (!fs::is_regular_file(ModFile(mod), ec)) {
      mod.status = std::format("{} is missing from the mod's folder", mod.manifest.file);
      mod.enabled = false;
    }
    g_mods.push_back(std::move(mod));
  }
  std::sort(g_mods.begin(), g_mods.end(),
            [](const Mod &a, const Mod &b) { return Lower(a.manifest.name) < Lower(b.manifest.name); });
}

Mod *FindLocked(std::string_view name) {
  const std::string wanted = Lower(std::string(name));
  for (Mod &mod : g_mods)
    if (Lower(mod.folder) == wanted || Lower(mod.manifest.name) == wanted)
      return &mod;
  return nullptr;
}

void ApplyPackage(Mod &mod) {
  const fs::path target = CustomDir() / PathFromUtf8(mod.manifest.file);
  std::error_code ec;
  std::string error;
  for (const uint32_t taken : kPortPackageIds) {
    if (mod.manifest.package_id == taken) {
      mod.status = std::format("package id {:#x} is one of the port's own; the mod is not loaded", taken);
      fs::remove(target, ec);
      return;
    }
  }
  for (const Mod &other : g_mods) {
    if (&other != &mod && other.enabled && other.manifest.type == ModType::kPackage &&
        other.manifest.package_id == mod.manifest.package_id && Lower(other.manifest.name) < Lower(mod.manifest.name)) {
      mod.status = std::format("package id {:#x} is also {}'s; the mod is not loaded", mod.manifest.package_id,
                               other.manifest.name);
      fs::remove(target, ec);
      return;
    }
  }
  if (!mod.enabled) {
    if (fs::is_regular_file(target, ec) && fs::remove(target, ec))
      EOT_INFO("[mods] {} taken out of Data/custom ({} is off)", mod.manifest.file, mod.manifest.name);
    return;
  }
  if (!fs::is_regular_file(target, ec) || !SameBytes(ModFile(mod), target)) {
    if (!CopyFile(ModFile(mod), target, error)) {
      mod.status = error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, error);
      return;
    }
    EOT_INFO("[mods] {} put in Data/custom for {}", mod.manifest.file, mod.manifest.name);
  }
  mod.active = true;
  mod.status = std::format("loaded at boot as package {:#x}{}", mod.manifest.package_id,
                           mod.manifest.language.empty() ? std::string()
                                                         : std::format(" in {}", mod.manifest.language));
}

void ApplyReplacement(Mod &mod) {
  const fs::path target = GameDataFile(mod.manifest.file);
  std::error_code ec;
  std::string error;
  if (target.empty()) {
    mod.status = std::format("{} is not the name of a file in the game's Data folder", mod.manifest.file);
    return;
  }
  const fs::path backup = BackupDir() / kDataFolder / target.filename();
  const bool in_place = SameBytes(ModFile(mod), target);
  if (!mod.enabled) {
    if (in_place && fs::is_regular_file(backup, ec)) {
      if (CopyFile(backup, target, error)) {
        fs::remove(backup, ec);
        EOT_INFO("[mods] the game's own {} put back ({} is off)", Utf8(target.filename()), mod.manifest.name);
      } else {
        mod.status = error;
        EOT_WARN("[mods] {}: {}", mod.manifest.name, error);
      }
    }
    return;
  }
  if (!in_place) {
    if (!fs::is_regular_file(backup, ec) && !CopyFile(target, backup, error)) {
      mod.status = "the game's own file could not be kept: " + error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, mod.status);
      return;
    }
    if (!CopyFile(ModFile(mod), target, error)) {
      mod.status = error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, error);
      return;
    }
    EOT_INFO("[mods] {} replaced by {}'s (the game's own kept in mods/backup)", Utf8(target.filename()),
             mod.manifest.name);
  }
  mod.active = true;
  mod.status = std::format("in place of the game's {}", Utf8(target.filename()));
}

bool WriteContentHeader(const fs::path &path, const std::string &display_name, const std::string &content_name) {
  using namespace rex::system;
  xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = static_cast<uint32_t>(xam::DummyDeviceId::HDD);
  data.content_type = XContentType::kMarketplaceContent;
  data.title_id = installer::kTitleId;
  data.xuid = 0;
  data.set_display_name(rex::string::to_utf16(display_name));
  data.set_file_name(content_name);
  const uint32_t license_mask = 1;
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(reinterpret_cast<const char *>(&data), sizeof(data));
  out.write(reinterpret_cast<const char *>(&license_mask), sizeof(license_mask));
  return out.good();
}

void WithdrawContent(const std::string &folder) {
  std::error_code ec;
  const fs::path dir = ContentDir() / PathFromUtf8(folder);
  if (!fs::is_directory(dir, ec))
    return;
  fs::remove_all(dir, ec);
  fs::remove(HeadersDir() / PathFromUtf8(folder + ".header"), ec);
  EOT_INFO("[mods] content {} withdrawn", folder);
}

void ApplyModel(Mod &mod, bool &one_published) {
  const std::string folder = ContentFolderName(mod);
  const fs::path published = ContentDir() / PathFromUtf8(folder) / PathFromUtf8(mod.manifest.file);
  const fs::path header = HeadersDir() / PathFromUtf8(folder + ".header");
  std::error_code ec;
  std::string why;
  if (ModelFileName(ModFile(mod), why).empty()) {
    mod.status = std::format("{} is {}", mod.manifest.file, why);
    WithdrawContent(folder);
    return;
  }
  if (!mod.enabled || one_published) {
    if (mod.enabled)
      mod.status = "waiting: another costume package is published (one at a time)";
    WithdrawContent(folder);
    return;
  }
  one_published = true;
  std::string error;
  if (!fs::is_regular_file(published, ec) || !SameBytes(ModFile(mod), published) || !fs::is_regular_file(header, ec)) {
    if (!CopyFile(ModFile(mod), published, error) || !WriteContentHeader(header, mod.manifest.name, folder)) {
      mod.status = error.empty() ? "the content header could not be written" : error;
      EOT_WARN("[mods] {}: {}", mod.manifest.name, mod.status);
      WithdrawContent(folder);
      return;
    }
    EOT_INFO("[mods] {} published as content {} for {}", mod.manifest.file, folder, mod.manifest.name);
  }
  mod.active = true;
  mod.status = std::format("published as downloadable content ({})", mod.manifest.file);
}

void ApplyAll() {
  std::vector<Mod *> order;
  for (Mod &mod : g_mods)
    order.push_back(&mod);
  std::stable_partition(order.begin(), order.end(), [](const Mod *m) { return !m->enabled; });
  bool one_model = false;
  std::error_code ec;
  for (Mod *mod : order) {
    if (!fs::is_regular_file(ModFile(*mod), ec))
      continue;
    switch (mod->manifest.type) {
    case ModType::kPackage:
      ApplyPackage(*mod);
      break;
    case ModType::kReplacement:
      ApplyReplacement(*mod);
      break;
    case ModType::kModel:
      ApplyModel(*mod, one_model);
      break;
    }
  }
  const fs::path dlc = g_install / "dlc";
  for (const auto &folder : fs::directory_iterator(ContentDir(), ec)) {
    if (!folder.is_directory())
      continue;
    const std::string name = Utf8(folder.path().filename());
    if (name.rfind(kContentPrefix, 0) != 0 || fs::is_regular_file(dlc / folder.path().filename(), ec))
      continue;
    bool owned = false;
    for (const Mod &mod : g_mods)
      owned = owned || ContentFolderName(mod) == name;
    if (!owned)
      WithdrawContent(name);
  }
}

void Refresh() {
  LoadState();
  Scan();
  ApplyAll();
}

void TakeBack(Mod &mod) {
  mod.enabled = false;
  mod.status.clear();
  switch (mod.manifest.type) {
  case ModType::kPackage:
    ApplyPackage(mod);
    break;
  case ModType::kReplacement:
    ApplyReplacement(mod);
    break;
  case ModType::kModel:
    WithdrawContent(ContentFolderName(mod));
    break;
  }
}

std::string Describe(const Mod &mod) {
  return std::format("{} ({}, {})", mod.manifest.name, TypeName(mod.manifest.type), mod.manifest.file);
}

std::string LetGoOfLanguage(const Mod &mod) {
  if (!mod.manifest.IsLanguage() || rex::cvar::GetFlagByName("eot_language") != mod.manifest.language)
    return {};
  for (const Mod &other : g_mods)
    if (&other != &mod && other.active && other.manifest.IsLanguage() &&
        other.manifest.language == mod.manifest.language)
      return {};
  rex::cvar::SetFlagByName("eot_language", "auto");
  rex::cvar::InvokeCommand("eot_save_settings", "");
  EOT_INFO("[mods] the language goes back to auto: {} carried {}", mod.manifest.name, mod.manifest.language);
  return std::format(" The language setting was {} and goes back to auto.", mod.manifest.language);
}

bool AddedLocked(const std::string &folder, std::string &message) {
  g_state[folder] = State{};
  SaveState();
  Refresh();
  const Mod *mod = FindLocked(folder);
  if (!mod) {
    message = std::format("{} was copied in but does not read as a mod.", folder);
    return false;
  }
  EOT_INFO("[mods] added {} as {}: {}", Describe(*mod), folder, mod->status);
  message = std::format("{} added as mods/{}: {}. Takes effect at the next start of the game.", Describe(*mod),
                        folder, mod->status);
  return true;
}

bool AddFolderLocked(const fs::path &path, std::string &message) {
  std::error_code ec;
  std::string error;
  const fs::path manifest_path = path / kManifestFileName;
  if (!fs::is_regular_file(manifest_path, ec)) {
    message = std::format("{} holds no {}.", Utf8(path), kManifestFileName);
    return false;
  }
  Manifest manifest;
  if (!ParseManifest(ReadText(manifest_path), manifest, error)) {
    message = std::format("{}: {}.", Utf8(manifest_path), error);
    return false;
  }
  if (!fs::is_regular_file(path / PathFromUtf8(manifest.file), ec)) {
    message = std::format("{} names {}, which is not beside it.", Utf8(manifest_path), manifest.file);
    return false;
  }
  const std::string folder = FolderNameFor(Utf8(path.filename()));
  const fs::path dir = ModDir(folder);
  if (!fs::equivalent(path, dir, ec)) {
    fs::remove_all(dir, ec);
    if (!CopyFile(manifest_path, dir / kManifestFileName, error) ||
        !CopyFile(path / PathFromUtf8(manifest.file), dir / PathFromUtf8(manifest.file), error)) {
      message = error;
      return false;
    }
  }
  return AddedLocked(folder, message);
}

bool AddFileLocked(const fs::path &path, std::string &message) {
  std::error_code ec;
  std::string error;
  Manifest manifest;
  if (!ManifestForFile(path, manifest, error)) {
    message = error + ".";
    return false;
  }
  const std::string folder = FolderNameFor(manifest.name);
  const fs::path dir = ModDir(folder);
  fs::remove_all(dir, ec);
  if (!WriteText(dir / kManifestFileName, WriteManifest(manifest), error) ||
      !CopyFile(path, dir / PathFromUtf8(manifest.file), error)) {
    message = error;
    return false;
  }
  return AddedLocked(folder, message);
}

bool AddLocked(const fs::path &path, std::string &message) {
  std::error_code ec;
  fs::create_directories(ModsDirLocked(), ec);
  const std::string named = Lower(Utf8(path.filename()));
  if (!fs::exists(path, ec) && IsBundled(named)) {
    g_state[named] = State{};
    ExtractBundled();
    return AddedLocked(named, message);
  }
  if (fs::is_directory(path, ec))
    return AddFolderLocked(path, message);
  if (fs::is_regular_file(path, ec)) {
    if (named == kManifestFileName)
      return AddFolderLocked(path.parent_path(), message);
    return AddFileLocked(path, message);
  }
  message = std::format("{} is neither a folder nor a file, nor the name of one of the port's own mods.",
                        Utf8(path));
  return false;
}

void FinishImport(int32_t state, std::string message) {
  std::lock_guard lock(g_mutex);
  g_import_message = std::move(message);
  g_import_state.store(state, std::memory_order_release);
}

void ImportInThread(fs::path path) {
  std::string message;
  bool ok;
  {
    std::lock_guard lock(g_mutex);
    ok = AddLocked(path, message);
  }
  EOT_INFO("[mods] import of {}: {}", Utf8(path), message);
  FinishImport(ok ? EOT_MODS_DONE : EOT_MODS_FAILED, message);
}

void SDLCALL DialogDone(void *, const char *const *files, int) {
  if (!files) {
    EOT_WARN("[mods] the file browser failed: {}", SDL_GetError());
    FinishImport(EOT_MODS_FAILED, "The file browser could not open.");
    return;
  }
  if (!files[0]) {
    FinishImport(EOT_MODS_IDLE, "");
    return;
  }
  const fs::path path = PathFromUtf8(files[0]);
  std::thread finished;
  {
    std::lock_guard lock(g_mutex);
    g_import_state.store(EOT_MODS_IMPORTING, std::memory_order_release);
    finished = std::move(g_import_thread);
    g_import_thread = std::thread(ImportInThread, path);
  }
  if (finished.joinable())
    finished.join();
}

void SDLCALL OpenDialog(void *) {
  static const SDL_DialogFileFilter kFilters[] = {
      {"A mod (mod.toml) or a package (pkz, pak)", "toml;pkz;pak"}, {"All files", "*"}};
  SDL_PropertiesID props = SDL_CreateProperties();
  if (props == 0) {
    FinishImport(EOT_MODS_FAILED, "The file browser could not open.");
    return;
  }
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Add a mod: its mod.toml, or a package file");
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter *>(kFilters));
  SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
  SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, &DialogDone, nullptr, props);
  SDL_DestroyProperties(props);
}

void CopyOut(char *out, size_t size, const std::string &text) {
  if (!out || size == 0)
    return;
  const size_t n = std::min(text.size(), size - 1);
  std::memcpy(out, text.data(), n);
  out[n] = 0;
}

}

void Initialize(const fs::path &install_root, const fs::path &game, const fs::path &profile) {
  std::lock_guard lock(g_mutex);
  if (g_ready && g_install == install_root && g_game == game && g_profile == profile)
    return;
  g_install = install_root;
  g_game = game;
  g_profile = profile;
  g_ready = !g_install.empty() && !g_game.empty() && !g_profile.empty();
  if (!g_ready)
    return;
  std::error_code ec;
  fs::create_directories(ModsDirLocked(), ec);
  LoadState();
  ExtractBundled();
  MigrateSlots();
  Scan();
  ApplyAll();
  for (const Mod &mod : g_mods)
    EOT_INFO("[mods] {} by {} [{}] {}: {}", mod.manifest.name, mod.manifest.creator, TypeName(mod.manifest.type),
             mod.enabled ? "on" : "off", mod.status);
}

bool Ready() {
  std::lock_guard lock(g_mutex);
  return g_ready;
}

std::vector<Mod> List() {
  std::lock_guard lock(g_mutex);
  return g_mods;
}

const Mod *Find(const std::vector<Mod> &mods, std::string_view name) {
  const std::string wanted = Lower(std::string(name));
  for (const Mod &mod : mods)
    if (Lower(mod.folder) == wanted || Lower(mod.manifest.name) == wanted)
      return &mod;
  return nullptr;
}

bool Add(const fs::path &path, std::string &message) {
  std::lock_guard lock(g_mutex);
  if (!g_ready) {
    message = "No install to add a mod to.";
    return false;
  }
  return AddLocked(path, message);
}

bool Remove(std::string_view name, std::string &message) {
  std::lock_guard lock(g_mutex);
  Mod *mod = g_ready ? FindLocked(name) : nullptr;
  if (!mod) {
    message = std::format("No mod is called {}.", name);
    return false;
  }
  const std::string described = Describe(*mod);
  const std::string folder = mod->folder;
  TakeBack(*mod);
  std::error_code ec;
  fs::remove_all(ModDir(folder), ec);
  if (ec) {
    message = std::format("{} could not be deleted: {}", Utf8(ModDir(folder)), ec.message());
    return false;
  }
  const std::string language_note = LetGoOfLanguage(*mod);
  State &state = g_state[folder];
  state = State{};
  state.removed = IsBundled(folder);
  SaveState();
  Refresh();
  EOT_INFO("[mods] removed {} (mods/{})", described, folder);
  message = std::format("{} removed{}.{} Takes effect at the next start of the game.", described,
                        state.removed ? "; it is one of the port's own and stays away until added again" : "",
                        language_note);
  return true;
}

bool SetEnabled(std::string_view name, bool enabled, std::string &message) {
  std::lock_guard lock(g_mutex);
  Mod *mod = g_ready ? FindLocked(name) : nullptr;
  if (!mod) {
    message = std::format("No mod is called {}.", name);
    return false;
  }
  std::error_code ec;
  if (enabled && !fs::is_regular_file(ModFile(*mod), ec)) {
    message = std::format("{} cannot be switched on: {}.", mod->manifest.name, mod->status);
    return false;
  }
  const std::string folder = mod->folder;
  g_state[folder].disabled = !enabled;
  if (enabled && mod->manifest.type == ModType::kModel)
    for (const Mod &other : g_mods)
      if (&other != mod && other.manifest.type == ModType::kModel)
        g_state[other.folder].disabled = true;
  const std::string language_note = enabled ? std::string() : LetGoOfLanguage(*mod);
  SaveState();
  Refresh();
  mod = FindLocked(folder);
  EOT_INFO("[mods] {} switched {}: {}", Describe(*mod), enabled ? "on" : "off", mod->status);
  message = std::format("{} is {}{}{}.{} Takes effect at the next start of the game.", Describe(*mod),
                        enabled ? "on" : "off", mod->status.empty() ? "" : ": ", mod->status, language_note);
  return true;
}

std::vector<LanguageMod> LanguageMods() {
  std::lock_guard lock(g_mutex);
  std::vector<LanguageMod> languages;
  for (const Mod &mod : g_mods) {
    if (!mod.active || !mod.manifest.IsLanguage())
      continue;
    const auto state = g_state.find(mod.folder);
    languages.push_back({mod.folder, mod.manifest.name, mod.manifest.language, mod.manifest.language_name,
                         state != g_state.end() && state->second.asked});
  }
  return languages;
}

void LanguageAsked(std::string_view folder) {
  std::lock_guard lock(g_mutex);
  if (!g_ready)
    return;
  g_state[std::string(folder)].asked = true;
  SaveState();
}

std::vector<PackageToLoad> PackagesToLoad() {
  std::lock_guard lock(g_mutex);
  std::vector<PackageToLoad> packages;
  for (const Mod &mod : g_mods) {
    if (!mod.active || mod.manifest.type != ModType::kPackage)
      continue;
    const std::string stem = Utf8(PathFromUtf8(mod.manifest.file).stem());
    packages.push_back({mod.manifest.package_id, std::format("L:/{}/{}.pak", eot::ui::kReeotPackageFolder, stem),
                        mod.manifest.language, mod.manifest.name});
  }
  return packages;
}

fs::path ModsDir() {
  std::lock_guard lock(g_mutex);
  return ModsDirLocked();
}

}

using namespace eot::mods;

namespace {

void Fill(const Mod &mod, eot_mod_info *out) {
  *out = eot_mod_info{};
  CopyOut(out->folder, sizeof(out->folder), mod.folder);
  CopyOut(out->name, sizeof(out->name), mod.manifest.name);
  CopyOut(out->creator, sizeof(out->creator), mod.manifest.creator);
  out->kind = static_cast<int32_t>(mod.manifest.type);
  out->enabled = mod.enabled ? 1 : 0;
  out->active = mod.active ? 1 : 0;
  out->bundled = mod.bundled ? 1 : 0;
  CopyOut(out->file, sizeof(out->file), mod.manifest.file);
  CopyOut(out->status, sizeof(out->status), mod.status);
}

}

extern "C" int32_t eot_mods_count(void) {
  std::lock_guard lock(g_mutex);
  return g_ready ? static_cast<int32_t>(g_mods.size()) : 0;
}

extern "C" int32_t eot_mods_get(int32_t index, eot_mod_info *out) {
  if (!out)
    return 0;
  std::lock_guard lock(g_mutex);
  if (!g_ready || index < 0 || static_cast<size_t>(index) >= g_mods.size())
    return 0;
  Fill(g_mods[static_cast<size_t>(index)], out);
  return 1;
}

extern "C" int32_t eot_mods_set_enabled(const char *folder, int32_t enabled, char *message, int32_t size) {
  std::string text;
  const bool ok = folder && SetEnabled(folder, enabled != 0, text);
  CopyOut(message, message && size > 0 ? static_cast<size_t>(size) : 0, text);
  return ok ? 1 : 0;
}

extern "C" int32_t eot_mods_remove(const char *folder, char *message, int32_t size) {
  std::string text;
  const bool ok = folder && Remove(folder, text);
  CopyOut(message, message && size > 0 ? static_cast<size_t>(size) : 0, text);
  return ok ? 1 : 0;
}

extern "C" int32_t eot_mods_add_begin(void) {
  {
    std::lock_guard lock(g_mutex);
    if (!g_ready)
      return 0;
    int32_t idle = EOT_MODS_IDLE;
    if (!g_import_state.compare_exchange_strong(idle, EOT_MODS_CHOOSING))
      return 0;
    g_import_message.clear();
  }
  if (!SDL_RunOnMainThread(&OpenDialog, nullptr, false)) {
    EOT_WARN("[mods] SDL_RunOnMainThread failed ({}); opening the file browser from here", SDL_GetError());
    OpenDialog(nullptr);
  }
  return 1;
}

extern "C" int32_t eot_mods_import_state(char *message, int32_t size) {
  std::lock_guard lock(g_mutex);
  const int32_t state = g_import_state.load(std::memory_order_acquire);
  if (message && size > 0)
    CopyOut(message, static_cast<size_t>(size),
            state == EOT_MODS_DONE || state == EOT_MODS_FAILED ? g_import_message : std::string());
  return state;
}

extern "C" void eot_mods_import_acknowledge(void) {
  std::thread finished;
  {
    std::lock_guard lock(g_mutex);
    const int32_t state = g_import_state.load(std::memory_order_acquire);
    if (state != EOT_MODS_DONE && state != EOT_MODS_FAILED)
      return;
    finished = std::move(g_import_thread);
    g_import_message.clear();
    g_import_state.store(EOT_MODS_IDLE, std::memory_order_release);
  }
  if (finished.joinable())
    finished.join();
}

extern "C" int32_t eot_mods_languages(char *out, int32_t size) {
  std::string text;
  int32_t count = 0;
  for (const LanguageMod &language : LanguageMods()) {
    text += language.tag + "\t" + language.language_name + "\n";
    ++count;
  }
  CopyOut(out, out && size > 0 ? static_cast<size_t>(size) : 0, text);
  return count;
}

extern "C" int32_t eot_mods_open_folder(void) {
  fs::path dir;
  {
    std::lock_guard lock(g_mutex);
    if (!g_ready)
      return 0;
    dir = ModsDirLocked();
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::u8string generic = dir.generic_u8string();
  const std::string url = "file:///" + std::string(generic.begin(), generic.end());
  if (!SDL_OpenURL(url.c_str())) {
    EOT_WARN("[mods] could not open {}: {}", Utf8(dir), SDL_GetError());
    return 0;
  }
  return 1;
}
