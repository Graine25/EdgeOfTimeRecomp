#define EOT_MODS_HOST
#include "mods/mod_manager.h"

#include <SDL3/SDL.h>

#include <rex/string/utf8.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/logging.h"
#include "installer/disc_install.h"
#include "mods/mods_api.h"
#include "platform/process.h"
#include "platform/update_check.h"

namespace eot::mods {
namespace {

namespace fs = std::filesystem;

constexpr const char *kSlotFolders[EOT_MOD_SLOT_COUNT] = {"pak", "model", "binary"};
constexpr const char *kBackupFolder = "backup";
constexpr const char *kDataFolder = "Data";
constexpr const char *kMarketplaceXuid = "0000000000000000";
constexpr const char *kHeadersDir = "Headers";
constexpr const char *kModelContentName = "Mod_Model";
constexpr const char *kModelDisplayName = "reeot model import";
constexpr uint32_t kFirstDlcPackageId = 0xBB9;
constexpr uint32_t kLastDlcPackageId = 0xBBA;
constexpr uint32_t kDlcPackageIdBase = 0xBB8;

std::mutex g_mutex;
fs::path g_install, g_game, g_profile;
bool g_ready = false;

std::atomic<int32_t> g_import_state{EOT_MODS_IDLE};
int32_t g_import_slot = 0;
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

fs::path ModsDir() { return g_install / kModsFolderName; }
fs::path SlotDir(int32_t slot) { return ModsDir() / kSlotFolders[slot]; }
fs::path BackupDir() { return ModsDir() / kBackupFolder; }
fs::path GameDataDir() { return g_game / kDataFolder; }

fs::path ContentDir() {
  return g_profile / kMarketplaceXuid / std::format("{:08X}", installer::kTitleId) /
         std::format("{:08X}", installer::kContentTypeMarketplace);
}
fs::path HeadersDir() {
  return g_profile / kMarketplaceXuid / std::format("{:08X}", installer::kTitleId) / kHeadersDir /
         std::format("{:08X}", installer::kContentTypeMarketplace);
}

std::vector<fs::path> SlotFiles(int32_t slot) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto &it : fs::directory_iterator(SlotDir(slot), ec))
    if (it.is_regular_file())
      files.push_back(it.path());
  std::sort(files.begin(), files.end());
  return files;
}

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

bool SameSize(const fs::path &a, const fs::path &b) {
  std::error_code ec;
  const uintmax_t sa = fs::file_size(a, ec);
  if (ec)
    return false;
  const uintmax_t sb = fs::file_size(b, ec);
  return !ec && sa == sb;
}

bool ReplaceFile(const fs::path &from, const fs::path &to, const fs::path &aside, std::string &error) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (!ec)
    return true;
  ec.clear();
  fs::create_directories(aside.parent_path(), ec);
  fs::remove(aside, ec);
  ec.clear();
  fs::rename(to, aside, ec);
  if (ec) {
    error = std::format("could not move {} aside: {}", Utf8(to), ec.message());
    return false;
  }
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    error = std::format("could not copy {} to {}: {}", Utf8(from), Utf8(to), ec.message());
    return false;
  }
  return true;
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

fs::path GameDataFile(const std::string &name) {
  std::error_code ec;
  const std::string wanted = Lower(name);
  for (const auto &it : fs::directory_iterator(GameDataDir(), ec))
    if (it.is_regular_file() && Lower(Utf8(it.path().filename())) == wanted)
      return it.path();
  return {};
}

fs::path PakBackup(const fs::path &target) { return BackupDir() / kDataFolder / target.filename(); }

bool ImportPak(const fs::path &path, std::string &message) {
  const std::string name = Utf8(path.filename());
  const fs::path target = GameDataFile(name);
  if (target.empty()) {
    message = std::format("{} is not the name of a file in the game's Data folder; a replacement is named "
                          "after the package it replaces (Common.pkz, 05_SMA_Main.pkz, ...).",
                          name);
    return false;
  }
  unsigned char head[8];
  if (!ReadHead(path, head, sizeof(head))) {
    message = std::format("{} could not be read.", name);
    return false;
  }
  std::error_code ec;
  std::string error;
  const fs::path backup = PakBackup(target);
  if (!fs::exists(backup, ec) && !CopyFile(target, backup, error)) {
    message = "The game's own file could not be kept: " + error;
    return false;
  }
  if (!CopyFile(path, SlotDir(EOT_MOD_SLOT_PAK) / target.filename(), error) || !CopyFile(path, target, error)) {
    message = error;
    return false;
  }
  EOT_INFO("[mods] {} replaced by {} (the game's own kept in {})", Utf8(target), Utf8(path), Utf8(backup));
  message = std::format("{} replaced; the game's own is kept in mods/backup. Takes effect at the next start of the "
                        "game.",
                        Utf8(target.filename()));
  return true;
}

bool RestorePak(std::string &message) {
  std::vector<std::string> put_back, failed;
  std::error_code ec;
  for (const fs::path &kept : SlotFiles(EOT_MOD_SLOT_PAK)) {
    const std::string name = Utf8(kept.filename());
    const fs::path backup = BackupDir() / kDataFolder / kept.filename();
    const fs::path target = GameDataDir() / kept.filename();
    std::string error;
    if (fs::exists(backup, ec)) {
      if (!CopyFile(backup, target, error)) {
        failed.push_back(name);
        EOT_WARN("[mods] {}: {}", name, error);
        continue;
      }
      fs::remove(backup, ec);
    }
    fs::remove(kept, ec);
    put_back.push_back(name);
    EOT_INFO("[mods] {} put back", Utf8(target));
  }
  fs::remove(BackupDir() / kDataFolder, ec);
  if (put_back.empty() && failed.empty()) {
    message = "No package is replaced.";
    return false;
  }
  message.clear();
  for (const std::string &n : put_back)
    message += (message.empty() ? "" : ", ") + n;
  if (!message.empty())
    message += " put back. Takes effect at the next start of the game.";
  for (const std::string &n : failed)
    message += std::format(" {} could not be put back.", n);
  return failed.empty();
}

void SyncPak() {
  std::error_code ec;
  for (const fs::path &kept : SlotFiles(EOT_MOD_SLOT_PAK)) {
    const fs::path target = GameDataDir() / kept.filename();
    if (fs::exists(target, ec) && SameSize(kept, target))
      continue;
    std::string error;
    const fs::path backup = PakBackup(target);
    if (fs::exists(target, ec) && !fs::exists(backup, ec) && !CopyFile(target, backup, error)) {
      EOT_WARN("[mods] {}: {}", Utf8(target), error);
      continue;
    }
    if (CopyFile(kept, target, error))
      EOT_INFO("[mods] {} replaced again by the kept {}", Utf8(target), Utf8(kept.filename()));
    else
      EOT_WARN("[mods] {}", error);
  }
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

struct ContentModel {
  std::string folder;
  std::string file;
};

std::vector<ContentModel> ModelsInPlace() {
  std::vector<ContentModel> models;
  std::error_code ec;
  const fs::path dlc = g_install / "dlc";
  for (const auto &folder : fs::directory_iterator(ContentDir(), ec)) {
    if (!folder.is_directory())
      continue;
    const std::string name = Utf8(folder.path().filename());
    if (fs::is_regular_file(dlc / folder.path().filename(), ec))
      continue;
    for (const auto &file : fs::directory_iterator(folder.path(), ec)) {
      if (!file.is_regular_file())
        continue;
      const std::string stem = Lower(Utf8(file.path().stem()));
      const std::string ext = Lower(Utf8(file.path().extension()));
      if (stem.rfind("_dlc", 0) == 0 && (ext == ".pak" || ext == ".pkz"))
        models.push_back({name, Utf8(file.path().filename())});
    }
  }
  std::sort(models.begin(), models.end(), [](const ContentModel &a, const ContentModel &b) {
    return a.folder < b.folder || (a.folder == b.folder && a.file < b.file);
  });
  return models;
}

void WithdrawModels() {
  std::error_code ec;
  for (const ContentModel &model : ModelsInPlace()) {
    fs::remove_all(ContentDir() / PathFromUtf8(model.folder), ec);
    fs::remove(HeadersDir() / PathFromUtf8(model.folder + ".header"), ec);
    EOT_INFO("[mods] content {} ({}) withdrawn", model.folder, model.file);
  }
}

bool ModelPublished(const fs::path &kept) {
  std::error_code ec;
  return fs::is_regular_file(ContentDir() / kModelContentName / kept.filename(), ec) &&
         fs::is_regular_file(HeadersDir() / (std::string(kModelContentName) + ".header"), ec);
}

bool PublishModel(const fs::path &kept, std::string &error) {
  WithdrawModels();
  if (!CopyFile(kept, ContentDir() / kModelContentName / kept.filename(), error))
    return false;
  if (!WriteContentHeader(HeadersDir() / (std::string(kModelContentName) + ".header"), kModelDisplayName,
                          kModelContentName)) {
    error = "the content header could not be written";
    WithdrawModels();
    return false;
  }
  return true;
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

bool ImportModel(const fs::path &path, std::string &message) {
  std::string why;
  const std::string name = ModelFileName(path, why);
  if (name.empty()) {
    message = std::format("{} is {}.", Utf8(path.filename()), why);
    return false;
  }
  std::error_code ec;
  for (const fs::path &old : SlotFiles(EOT_MOD_SLOT_MODEL))
    fs::remove(old, ec);
  const fs::path kept = SlotDir(EOT_MOD_SLOT_MODEL) / name;
  std::string error;
  if (!CopyFile(path, kept, error) || !PublishModel(kept, error)) {
    message = error;
    return false;
  }
  EOT_INFO("[mods] {} imported as {} and published as content {}", Utf8(path), name, kModelContentName);
  message = std::format("{} imported as {} and published with the game's downloadable content. Takes effect at "
                        "the next start of the game.",
                        Utf8(path.filename()), name);
  return true;
}

bool RestoreModel(std::string &message) {
  std::string names;
  for (const ContentModel &model : ModelsInPlace())
    names += (names.empty() ? "" : ", ") + model.file;
  if (names.empty()) {
    message = "No costume package is in place.";
    return false;
  }
  WithdrawModels();
  std::error_code ec;
  for (const fs::path &file : SlotFiles(EOT_MOD_SLOT_MODEL))
    fs::remove(file, ec);
  message = names + " withdrawn. Takes effect at the next start of the game.";
  return true;
}

std::string ModelsInPlaceText() {
  std::string names;
  for (const ContentModel &model : ModelsInPlace())
    names += (names.empty() ? "" : ", ") + model.folder + "/" + model.file;
  return names;
}

void SyncModel() {
  for (const fs::path &kept : SlotFiles(EOT_MOD_SLOT_MODEL)) {
    if (ModelPublished(kept))
      continue;
    std::string error;
    if (PublishModel(kept, error))
      EOT_INFO("[mods] {} published again as content {}", Utf8(kept.filename()), kModelContentName);
    else
      EOT_WARN("[mods] {}: {}", Utf8(kept.filename()), error);
  }
}

fs::path BinaryTarget(const fs::path &path, std::string &why) {
  unsigned char head[2];
  if (!ReadHead(path, head, sizeof(head)) || head[0] != 'M' || head[1] != 'Z') {
    why = "not a Windows program file";
    return {};
  }
  const std::string ext = Lower(Utf8(path.extension()));
  if (ext == ".exe")
    return g_install / platform::kExecutableFileName;
  if (ext == ".dll") {
    std::error_code ec;
    const fs::path target = g_install / path.filename();
    if (fs::is_regular_file(target, ec))
      return target;
    why = std::format("not the name of a library beside the program ({})", Utf8(g_install));
    return {};
  }
  why = "neither an .exe nor a .dll";
  return {};
}

bool ImportBinary(const fs::path &path, std::string &message) {
  std::string why;
  const fs::path target = BinaryTarget(path, why);
  if (target.empty()) {
    message = std::format("{} is {}.", Utf8(path.filename()), why);
    return false;
  }
  std::error_code ec;
  std::string error;
  const fs::path backup = BackupDir() / target.filename();
  const fs::path kept = SlotDir(EOT_MOD_SLOT_BINARY) / target.filename();
  if (!fs::exists(backup, ec) && fs::exists(target, ec)) {
    fs::create_directories(backup.parent_path(), ec);
    fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      ec.clear();
      fs::rename(target, backup, ec);
    }
    if (ec) {
      message = std::format("The port's own {} could not be kept: {}", Utf8(target.filename()), ec.message());
      return false;
    }
  }
  if (!CopyFile(path, kept, error) || !ReplaceFile(kept, target, BackupDir() / (Utf8(target.filename()) + ".old"), error)) {
    message = error;
    return false;
  }
  EOT_INFO("[mods] {} replaced by {} (the port's own kept in {})", Utf8(target), Utf8(path), Utf8(backup));
  message = std::format("{} replaced; the port's own is kept in mods/backup. Takes effect at the next start of the "
                        "game.",
                        Utf8(target.filename()));
  return true;
}

bool RestoreBinary(std::string &message) {
  std::vector<std::string> put_back, failed;
  std::error_code ec;
  for (const fs::path &kept : SlotFiles(EOT_MOD_SLOT_BINARY)) {
    const std::string name = Utf8(kept.filename());
    const fs::path backup = BackupDir() / kept.filename();
    const fs::path target = g_install / kept.filename();
    std::string error;
    if (fs::exists(backup, ec)) {
      if (!ReplaceFile(backup, target, BackupDir() / (name + ".old"), error)) {
        failed.push_back(name);
        EOT_WARN("[mods] {}: {}", name, error);
        continue;
      }
      fs::remove(backup, ec);
    }
    fs::remove(kept, ec);
    put_back.push_back(name);
    EOT_INFO("[mods] {} put back", Utf8(target));
  }
  if (put_back.empty() && failed.empty()) {
    message = "No program file is replaced.";
    return false;
  }
  message.clear();
  for (const std::string &n : put_back)
    message += (message.empty() ? "" : ", ") + n;
  if (!message.empty())
    message += " put back. Takes effect at the next start of the game.";
  for (const std::string &n : failed)
    message += std::format(" {} could not be put back.", n);
  return failed.empty();
}

std::string BinaryNote() {
  const fs::path exe = g_install / platform::kExecutableFileName;
  std::string note = std::string(platform::kExecutableFileName) + ": ";
  if (const auto id = platform::ReadBuildIdentity(exe)) {
    note += "reeot";
    if (!id->version.empty())
      note += " v" + id->version;
    if (!id->commit.empty())
      note += " (" + id->commit + (id->modified ? "*)" : ")");
    if (!id->stamp.empty())
      note += ", built " + id->stamp;
  } else {
    note += "a program that carries no reeot build information";
  }
  note += SlotFiles(EOT_MOD_SLOT_BINARY).empty() ? " -- the port's own (stock) build"
                                                  : " -- a replacement; the port's own is kept in mods/backup";
  return note;
}

void SyncBinary() {
  std::error_code ec;
  for (const auto &it : fs::directory_iterator(BackupDir(), ec))
    if (it.is_regular_file() && Utf8(it.path().extension()) == ".old")
      fs::remove(it.path(), ec);
  for (const fs::path &kept : SlotFiles(EOT_MOD_SLOT_BINARY))
    EOT_INFO("[mods] {} is in place of the port's own", Utf8(kept.filename()));
}

bool Import(int32_t slot, const fs::path &path, std::string &message) {
  std::lock_guard lock(g_mutex);
  switch (slot) {
  case EOT_MOD_SLOT_PAK:
    return ImportPak(path, message);
  case EOT_MOD_SLOT_MODEL:
    return ImportModel(path, message);
  case EOT_MOD_SLOT_BINARY:
    return ImportBinary(path, message);
  default:
    message = "no such slot";
    return false;
  }
}

bool Restore(int32_t slot, std::string &message) {
  switch (slot) {
  case EOT_MOD_SLOT_PAK:
    return RestorePak(message);
  case EOT_MOD_SLOT_MODEL:
    return RestoreModel(message);
  case EOT_MOD_SLOT_BINARY:
    return RestoreBinary(message);
  default:
    message = "no such slot";
    return false;
  }
}

void FinishImport(int32_t state, std::string message) {
  std::lock_guard lock(g_mutex);
  g_import_message = std::move(message);
  g_import_state.store(state, std::memory_order_release);
}

void ImportInThread(int32_t slot, fs::path path) {
  std::string message;
  const bool ok = Import(slot, path, message);
  EOT_INFO("[mods] import of {} into {}: {}", Utf8(path), kSlotFolders[slot], message);
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
    g_import_thread = std::thread(ImportInThread, g_import_slot, path);
  }
  if (finished.joinable())
    finished.join();
}

void SDLCALL OpenDialog(void *userdata) {
  static const SDL_DialogFileFilter kPakFilters[] = {{"Game packages", "pkz;pak;dat"}, {"All files", "*"}};
  static const SDL_DialogFileFilter kModelFilters[] = {{"Costume packages", "pak;pkz"}, {"All files", "*"}};
  static const SDL_DialogFileFilter kBinaryFilters[] = {{"Program files", "exe;dll"}, {"All files", "*"}};
  const int32_t slot = static_cast<int32_t>(reinterpret_cast<intptr_t>(userdata));
  const SDL_DialogFileFilter *filters = slot == EOT_MOD_SLOT_PAK     ? kPakFilters
                                        : slot == EOT_MOD_SLOT_MODEL ? kModelFilters
                                                                     : kBinaryFilters;
  const char *title = slot == EOT_MOD_SLOT_PAK     ? "Import a package in place of the game's own"
                      : slot == EOT_MOD_SLOT_MODEL ? "Import a costume package"
                                                   : "Import a program file in place of the port's own";
  SDL_PropertiesID props = SDL_CreateProperties();
  if (props == 0) {
    FinishImport(EOT_MODS_FAILED, "The file browser could not open.");
    return;
  }
  SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter *>(filters));
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
  g_install = install_root;
  g_game = game;
  g_profile = profile;
  g_ready = !g_install.empty() && !g_game.empty() && !g_profile.empty();
  if (!g_ready)
    return;
  std::error_code ec;
  if (!fs::is_directory(ModsDir(), ec))
    return;
  SyncPak();
  SyncModel();
  SyncBinary();
}

}

using namespace eot::mods;

extern "C" int32_t eot_mods_slot(int32_t slot, eot_mod_slot_info *out) {
  if (!out || slot < 0 || slot >= EOT_MOD_SLOT_COUNT)
    return 0;
  std::lock_guard lock(g_mutex);
  *out = eot_mod_slot_info{};
  if (!g_ready)
    return 1;
  std::string names;
  if (slot == EOT_MOD_SLOT_MODEL) {
    names = ModelsInPlaceText();
    out->installed = names.empty() ? 0 : 1;
  } else {
    for (const fs::path &file : SlotFiles(slot)) {
      names += (names.empty() ? "" : ", ") + Utf8(file.filename());
      ++out->installed;
    }
  }
  CopyOut(out->files, sizeof(out->files), names);
  if (slot == EOT_MOD_SLOT_BINARY)
    CopyOut(out->note, sizeof(out->note), BinaryNote());
  return 1;
}

extern "C" int32_t eot_mods_import_begin(int32_t slot) {
  if (slot < 0 || slot >= EOT_MOD_SLOT_COUNT)
    return 0;
  {
    std::lock_guard lock(g_mutex);
    if (!g_ready)
      return 0;
    int32_t idle = EOT_MODS_IDLE;
    if (!g_import_state.compare_exchange_strong(idle, EOT_MODS_CHOOSING))
      return 0;
    g_import_slot = slot;
    g_import_message.clear();
  }
  void *userdata = reinterpret_cast<void *>(static_cast<intptr_t>(slot));
  if (!SDL_RunOnMainThread(&OpenDialog, userdata, false)) {
    EOT_WARN("[mods] SDL_RunOnMainThread failed ({}); opening the file browser from here", SDL_GetError());
    OpenDialog(userdata);
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

extern "C" int32_t eot_mods_restore(int32_t slot, char *message, int32_t size) {
  if (slot < 0 || slot >= EOT_MOD_SLOT_COUNT)
    return 0;
  std::lock_guard lock(g_mutex);
  std::string text;
  const bool ok = g_ready && Restore(slot, text);
  EOT_INFO("[mods] restore of {}: {}", kSlotFolders[slot], text);
  CopyOut(message, message && size > 0 ? static_cast<size_t>(size) : 0, text);
  return ok ? 1 : 0;
}

extern "C" int32_t eot_mods_open_folder(void) {
  fs::path dir;
  {
    std::lock_guard lock(g_mutex);
    if (!g_ready)
      return 0;
    dir = ModsDir();
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
