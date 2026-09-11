#include "platform/update_check.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <rex/cvar.h>

#include "core/build_info.h"
#include "core/logging.h"

REXCVAR_DEFINE_BOOL(eot_update_check, true, "EdgeOfTime/Config",
                    "Notice when a newer reeot build is in the distribution folder (offline).");
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

}
