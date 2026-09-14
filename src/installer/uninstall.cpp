#include "installer/uninstall.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

#include "core/logging.h"
#include "installer/install_registry.h"
#include "platform/desktop_shortcut.h"
#include "platform/fatal_dialog.h"
#include "platform/process.h"

namespace eot::installer {

namespace {

namespace fs = std::filesystem;

fs::path DownloadsFolder() {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)))
    out = p;
  if (p)
    CoTaskMemFree(p);
  if (out.empty())
    if (const wchar_t *home = _wgetenv(L"USERPROFILE"); home && *home)
      out = fs::path(home) / L"Downloads";
  return out;
}

fs::path UniqueDir(const fs::path &parent, const std::string &base) {
  std::error_code ec;
  fs::path p = parent / base;
  for (int n = 2; fs::exists(p, ec); ++n)
    p = parent / (base + " (" + std::to_string(n) + ")");
  return p;
}

size_t CopySaves(const fs::path &profiles, const fs::path &dest) {
  std::error_code ec;
  size_t files = 0;
  for (fs::recursive_directory_iterator it(profiles, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec)
      break;
    if (it->is_directory(ec)) {
      if (it->path().filename() == "cache")
        it.disable_recursion_pending();
      continue;
    }
    const fs::path rel = fs::relative(it->path(), profiles, ec);
    if (ec)
      continue;
    const fs::path out = dest / rel;
    fs::create_directories(out.parent_path(), ec);
    if (fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec))
      ++files;
  }
  return files;
}

void SpawnDeferredDelete(const fs::path &root) {
  const std::wstring q = L"\"" + root.wstring() + L"\"";
  std::wstring cmd = L"cmd.exe /c \"for /l %i in (1,1,30) do (rmdir /s /q " + q + L" 2>nul & if not exist " +
                     q + L" exit & ping 127.0.0.1 -n 2 >nul)\"";
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                     &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

}

void RunUninstall() {
  std::error_code ec;
  fs::path root;
  if (auto cfg = ReadInstallRegistry())
    root = cfg->install_root;
  if (root.empty()) {
    const fs::path prog = platform::ProgramDir();
    if (prog.filename() == kInstallFolderName)
      root = prog;
  }
  if (root.empty() || !fs::exists(root, ec)) {
    ClearInstallRegistry();
    platform::ShowInfo("reeot", "No reeot installation was found to remove.");
    return;
  }

  if (!platform::ShowConfirm(
          "Uninstall reeot",
          "This removes the installation at:\n\n" + root.string() +
              "\n\nYour saves are copied to your Downloads folder first. Continue?"))
    return;

  std::string saved_to;
  const fs::path profiles = root / "profiles";
  if (fs::is_directory(profiles, ec)) {
    if (const fs::path downloads = DownloadsFolder(); !downloads.empty()) {
      const fs::path dest = UniqueDir(downloads, "reeot saves");
      if (CopySaves(profiles, dest) > 0)
        saved_to = dest.string();
      else
        fs::remove_all(dest, ec);
    }
  }

  ClearInstallRegistry();
  platform::RemoveDesktopShortcut("reeot");

  fs::remove_all(root, ec);
  const bool deferred = fs::exists(root, ec);
  if (deferred)
    SpawnDeferredDelete(root);

  std::string msg = "reeot has been uninstalled.";
  msg += saved_to.empty() ? "\n\nNo saves were found to keep."
                          : "\n\nYour saves were copied to:\n" + saved_to;
  if (deferred)
    msg += "\n\nThe last program files are removed a moment after this window closes.";
  EOT_INFO("[uninstall] removed {} (saves: {}{})", root.string(),
           saved_to.empty() ? "none" : saved_to, deferred ? "; deferred" : "");
  platform::ShowInfo("reeot - uninstalled", msg);
}

}
