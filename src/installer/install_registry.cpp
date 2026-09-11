#include "installer/install_registry.h"

#include <cstdlib>

#include "core/build_info.h"
#include "core/logging.h"

namespace eot::installer {

std::filesystem::path InstallRootFor(const std::filesystem::path &picked) {
  if (picked.filename() == kInstallFolderName)
    return picked;
  return picked / kInstallFolderName;
}

std::filesystem::path DefaultInstallRoot() {
#if defined(_WIN32)
  if (const wchar_t *local = _wgetenv(L"LOCALAPPDATA"); local && *local)
    return std::filesystem::path(local) / L"Programs" / kInstallFolderName;
#endif
  return std::filesystem::path(kInstallFolderName);
}

}

#if defined(_WIN32)
#include <windows.h>

#include "core/encoding.h"

namespace eot::installer {
namespace {

constexpr wchar_t kInstallKey[] = L"Software\\reeot\\Install";

std::optional<std::wstring> ReadString(HKEY key, const wchar_t *name) {
  DWORD type = 0, size = 0;
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS)
    return std::nullopt;
  std::wstring out(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, out.data(), &size) != ERROR_SUCCESS)
    return std::nullopt;
  while (!out.empty() && out.back() == L'\0')
    out.pop_back();
  return out;
}

bool WriteString(HKEY key, const wchar_t *name, const std::wstring &value) {
  const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), bytes) ==
         ERROR_SUCCESS;
}

struct KeyGuard {
  HKEY key;
  ~KeyGuard() {
    if (key)
      RegCloseKey(key);
  }
};

}

std::optional<InstallConfig> ReadInstallRegistry() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kInstallKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    return std::nullopt;
  KeyGuard guard{key};

  const auto root = ReadString(key, L"InstallRoot");
  if (!root || root->empty())
    return std::nullopt;

  InstallConfig cfg;
  cfg.install_root = *root;
  if (auto fp = ReadString(key, L"DiscFingerprint"))
    cfg.disc_fingerprint = WideToUtf8(*fp);
  if (auto sv = ReadString(key, L"SchemaVersion")) {
    try {
      cfg.schema_version = std::stoi(*sv);
    } catch (...) {
      cfg.schema_version = 0;
    }
  }
  if (auto v = ReadString(key, L"AppVersion"))
    cfg.app_version = WideToUtf8(*v);

  return cfg;
}

bool InstallIsPresent(const InstallConfig &config) {
  std::error_code ec;
  return std::filesystem::is_regular_file(config.game_data_path() / "Default.xex", ec);
}

bool WriteInstallRegistry(const InstallConfig &config) {
  HKEY key = nullptr;
  const LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kInstallKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    EOT_ERROR("[install] RegCreateKeyExW failed: {}", status);
    return false;
  }
  {
    KeyGuard guard{key};
    bool ok = true;
    ok &= WriteString(key, L"InstallRoot", config.install_root.wstring());
    ok &= WriteString(key, L"DiscFingerprint", Utf8ToWide(config.disc_fingerprint));
    ok &= WriteString(key, L"SchemaVersion", std::to_wstring(kInstallSchemaVersion));
    ok &= WriteString(key, L"AppVersion", Utf8ToWide(REEOT_VERSION_STRING));
    if (ok)
      return true;
    EOT_ERROR("[install] could not write every value of the install record");
  }
  ClearInstallRegistry();
  return false;
}

bool ClearInstallRegistry() {
  const LONG status = RegDeleteTreeW(HKEY_CURRENT_USER, kInstallKey);
  if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
    return true;
  EOT_ERROR("[install] RegDeleteTreeW failed: {}", status);
  return false;
}

}

#else

namespace eot::installer {

std::optional<InstallConfig> ReadInstallRegistry() { return std::nullopt; }
bool WriteInstallRegistry(const InstallConfig &) { return false; }
bool ClearInstallRegistry() { return true; }
bool InstallIsPresent(const InstallConfig &) { return false; }

}

#endif
