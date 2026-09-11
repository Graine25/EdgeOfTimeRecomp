#include "platform/update_check.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>

#include "core/build_info.h"
#include "core/logging.h"
#include "platform/process.h"

REXCVAR_DEFINE_BOOL(eot_update_apply, true, "EdgeOfTime/Config",
                    "When a newer build is run, copy it over an older install in the "
                    "EdgeOfTimeRecompiled folder.");

namespace eot::platform {

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::optional<InstallUpdate> g_updated;

std::string WideToUtf8(const std::wstring &w) {
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n ? n - 1 : 0, '\0');
  if (n)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
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
    files = {"reeot.exe",      "rexruntimerd.dll", "reeot_GameLogic.dll",
             "dxcompiler.dll", "dxil.dll",         "gamecontrollerdb.txt"};
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
  for (const auto &f : files)
    fs::remove(dir / (f + ".old"), ec);
}

bool ReplaceFile(const fs::path &src, const fs::path &dst, const fs::path &aside) {
  std::error_code ec;
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (!ec)
    return true;
  ec.clear();
  fs::remove(aside, ec);
  fs::rename(dst, aside, ec);
  if (ec)
    return false;
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

}

void UpdateInstalledCopy(const fs::path &install_root) {
  if (!REXCVAR_GET(eot_update_apply) || install_root.empty())
    return;
  std::error_code ec;
  const fs::path prog = ProgramDir();
  const std::vector<std::string> files = ProgramFiles(prog);

  ClearUpdateLeftovers(install_root, files);

  if (fs::equivalent(prog, install_root, ec))
    return;
  const fs::path installed_exe = install_root / "reeot.exe";
  if (!fs::is_regular_file(installed_exe, ec))
    return;

  const std::string installed = ResourceString(ReadVersionResource(installed_exe), L"BuildStamp");
  if (!installed.empty() && installed >= std::string(REEOT_BUILD_TIMESTAMP))
    return;

  size_t pushed = 0;
  for (const auto &f : files) {
    const fs::path src = prog / f;
    if (!fs::is_regular_file(src, ec) || !FilesDiffer(install_root / f, src))
      continue;
    if (ReplaceFile(src, install_root / f, install_root / (f + ".old")))
      ++pushed;
    else
      EOT_ERROR("[update] could not replace {} in {}", f, install_root.string());
  }
  if (pushed == 0)
    return;

  {
    std::lock_guard lock(g_mutex);
    g_updated = InstallUpdate{REEOT_BUILD_TIMESTAMP, REEOT_VERSION_STRING, install_root.string()};
  }
  EOT_INFO("[update] updated the install at {} to v{} build {} ({} file(s))", install_root.string(),
           REEOT_VERSION_STRING, REEOT_BUILD_TIMESTAMP, pushed);
}

std::optional<InstallUpdate> LastInstallUpdate() {
  std::lock_guard lock(g_mutex);
  return g_updated;
}

}
