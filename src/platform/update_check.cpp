#include "platform/update_check.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <rex/cvar.h>
#include <rex/runtime.h>

#include "core/build_info.h"
#include "core/logging.h"
#include "platform/process.h"

REXCVAR_DEFINE_BOOL(eot_update_check, true, "EdgeOfTime/Config",
                    "Notice when a newer reeot build is in the distribution folder (offline).");
REXCVAR_DEFINE_BOOL(eot_update_apply, true, "EdgeOfTime/Config",
                    "Swap in a newer build from the distribution folder at startup and relaunch.");
REXCVAR_DEFINE_STRING(eot_update_dir, "", "EdgeOfTime/Config",
                      "Folder holding the reeot.exe to compare against; empty = Downloads/reeot-dis.");

namespace eot::platform {

namespace {

namespace fs = std::filesystem;

std::once_flag g_once;
std::mutex g_mutex;
std::atomic<bool> g_has_update{false};
AvailableUpdate g_update;

std::wstring Utf8ToWide(const std::string &s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n ? n - 1 : 0, L'\0');
  if (n)
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::string WideToUtf8(const std::wstring &w) {
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n ? n - 1 : 0, '\0');
  if (n)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

fs::path DistFolder() {
  const std::string over = REXCVAR_GET(eot_update_dir);
  if (!over.empty())
    return fs::path(Utf8ToWide(over));
  PWSTR downloads = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &downloads)))
    dir = fs::path(downloads) / L"reeot-dis";
  if (downloads)
    CoTaskMemFree(downloads);
  return dir;
}

std::vector<uint8_t> ReadVersionResource(const fs::path &exe) {
  DWORD unused = 0;
  const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &unused);
  if (!size)
    return {};
  std::vector<uint8_t> data(size);
  if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data()))
    return {};
  return data;
}

std::string ResourceString(const std::vector<uint8_t> &res, const wchar_t *key) {
  if (res.empty())
    return {};
  void *value = nullptr;
  UINT len = 0;
  const std::wstring path = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
  if (!VerQueryValueW(res.data(), path.c_str(), &value, &len) || !value || len == 0)
    return {};
  return WideToUtf8(std::wstring(static_cast<const wchar_t *>(value), len - 1));
}

void Run() {
  if (!REXCVAR_GET(eot_update_check))
    return;
  std::error_code ec;
  const fs::path dir = DistFolder();
  if (dir.empty())
    return;
  const fs::path exe = dir / L"reeot.exe";
  if (!fs::is_regular_file(exe, ec))
    return;
  const fs::path self = [] {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n ? fs::path(std::wstring(buf, n)) : fs::path{};
  }();
  if (!self.empty() && fs::exists(self, ec) && fs::equivalent(self, exe, ec))
    return;
  const std::vector<uint8_t> res = ReadVersionResource(exe);
  const std::string stamp = ResourceString(res, L"BuildStamp");
  const std::string version = ResourceString(res, L"ProductVersion");
  if (stamp.empty()) {
    EOT_DEBUG("[update] {} carries no build stamp; nothing to compare", exe.string());
    return;
  }
  if (stamp <= std::string(REEOT_BUILD_TIMESTAMP)) {
    EOT_INFO("[update] {} holds build {} (this build {}); up to date", dir.string(), stamp,
             REEOT_BUILD_TIMESTAMP);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_update = AvailableUpdate{stamp, version, dir.string()};
  }
  g_has_update.store(true, std::memory_order_release);
  EOT_INFO("[update] a newer reeot build is in {}: v{} build {} (this v{} build {})", dir.string(),
           version.empty() ? "?" : version, stamp, REEOT_VERSION_STRING, REEOT_BUILD_TIMESTAMP);
}

std::vector<std::string> ProgramFiles(const fs::path &dir) {
  std::vector<std::string> files;
  std::ifstream in(dir / "program_files.txt");
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (!line.empty())
      files.push_back(line);
  }
  if (files.empty())
    files = {"reeot.exe",          "rexruntimerd.dll", "reeot_GameLogic.dll",
             "dxcompiler.dll",     "dxil.dll",         "gamecontrollerdb.txt"};
  return files;
}

bool FilesDiffer(const fs::path &have, const fs::path &src) {
  std::error_code ec;
  if (!fs::exists(have, ec))
    return true;
  if (fs::file_size(have, ec) != fs::file_size(src, ec) || ec)
    return true;
  std::ifstream a(have, std::ios::binary), b(src, std::ios::binary);
  if (!a || !b)
    return true;
  char ba[64 * 1024], bb[64 * 1024];
  for (;;) {
    a.read(ba, sizeof(ba));
    b.read(bb, sizeof(bb));
    if (a.gcount() != b.gcount() || std::memcmp(ba, bb, static_cast<size_t>(a.gcount())) != 0)
      return true;
    if (a.eof() && b.eof())
      return false;
    if (a.bad() || b.bad())
      return true;
  }
}

void ClearUpdateLeftovers(const fs::path &dir, const std::vector<std::string> &files) {
  std::error_code ec;
  for (const auto &f : files) {
    fs::remove(dir / (f + ".old"), ec);
    fs::remove(dir / (f + ".incoming"), ec);
  }
}

}

void BeginUpdateCheck() {
  std::call_once(g_once, [] {
    std::thread([] { Run(); }).detach();
  });
}

std::optional<AvailableUpdate> NewerBuildAvailable() {
  if (!g_has_update.load(std::memory_order_acquire))
    return std::nullopt;
  std::lock_guard lock(g_mutex);
  return g_update;
}

bool ApplyOfflineUpdate() {
  const fs::path prog = ProgramDir();
  const std::vector<std::string> files = ProgramFiles(prog);
  ClearUpdateLeftovers(prog, files);
  if (!REXCVAR_GET(eot_update_apply))
    return false;

  std::error_code ec;
  const fs::path dir = DistFolder();
  if (dir.empty())
    return false;
  const fs::path dist_exe = dir / "reeot.exe";
  if (!fs::is_regular_file(dist_exe, ec))
    return false;
  if (fs::equivalent(dir, prog, ec))
    return false;

  const std::string stamp = ResourceString(ReadVersionResource(dist_exe), L"BuildStamp");
  if (stamp.empty() || stamp <= std::string(REEOT_BUILD_TIMESTAMP))
    return false;

  std::vector<std::string> changed;
  for (const auto &f : files) {
    const fs::path src = dir / f;
    if (fs::is_regular_file(src, ec) && FilesDiffer(prog / f, src))
      changed.push_back(f);
  }
  if (changed.empty())
    return false;

  EOT_INFO("[update] build {} in {} is newer than this build {}; swapping {} file(s)", stamp,
           dir.string(), REEOT_BUILD_TIMESTAMP, changed.size());

  for (const auto &f : changed) {
    fs::copy_file(dir / f, prog / (f + ".incoming"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
      EOT_ERROR("[update] could not stage {}: {}", f, ec.message());
      for (const auto &g : changed)
        fs::remove(prog / (g + ".incoming"), ec);
      return false;
    }
  }

  std::vector<std::string> swapped;
  auto rollback = [&] {
    for (auto it = swapped.rbegin(); it != swapped.rend(); ++it) {
      fs::remove(prog / *it, ec);
      fs::rename(prog / (*it + ".old"), prog / *it, ec);
    }
    for (const auto &g : changed)
      fs::remove(prog / (g + ".incoming"), ec);
  };
  for (const auto &f : changed) {
    const fs::path target = prog / f;
    const bool had = fs::exists(target, ec);
    if (had) {
      fs::rename(target, prog / (f + ".old"), ec);
      if (ec) {
        EOT_ERROR("[update] could not move {} aside: {}; leaving the build as it was", f,
                  ec.message());
        rollback();
        return false;
      }
    }
    fs::rename(prog / (f + ".incoming"), target, ec);
    if (ec) {
      EOT_ERROR("[update] could not swap in {}: {}; leaving the build as it was", f, ec.message());
      if (had)
        fs::rename(prog / (f + ".old"), target, ec);
      rollback();
      return false;
    }
    if (had)
      swapped.push_back(f);
  }

  EOT_INFO("[update] swapped to build {}; relaunching", stamp);
  rex::FlushLogging();
  if (RelaunchSelf())
    return true;
  EOT_WARN("[update] swapped the files but could not relaunch; the new build starts next launch");
  return false;
}

}
