#pragma once

#include <functional>
#include <string>

#include <rex/types.h>

inline constexpr char kCvarGroup[] = "Edge of Time";

namespace eot {

std::string FormatCvar(i32 v);
std::string FormatCvar(bool v);
std::string FormatCvar(f64 v);

class Settings {
public:
  static Settings &Get();

  void Init();

  void AdoptCvars();

  bool Devmode() const { return devmode_; }
  bool SetDevmode(bool v);

  void SetDevmodeApplier(std::function<void()> applier);

  bool DbgPrint() const { return dbgPrint_; }
  bool SetDbgPrint(bool v);

  const std::string &Language() const { return language_; }
  bool SetLanguage(const std::string &v);

  bool I18nKeys() const { return i18nKeys_; }
  bool SetI18nKeys(bool v);

  const std::string &LanguagePath() const { return languagePath_; }

  i32 PerfHistorySeconds() const { return perfHistorySeconds_; }
  bool SetPerfHistorySeconds(i32 v);

  bool PerfCSV() const { return perfCSV_; }
  bool SetPerfCSV(bool v);

  bool Profiler() const { return profiler_; }

  i32 ShutdownTimeoutMs() const { return shutdownTimeoutMs_; }
  bool SetShutdownTimeoutMs(i32 v);

  const std::string &SavesPath() const { return savesPath_; }
  const std::string &CachePath() const { return cachePath_; }

private:
  Settings() = default;
  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

  void AdoptDevmode();
  void AdoptDbgPrint();
  void AdoptLanguage();
  void AdoptI18nKeys();
  void AdoptLanguagePath();
  void AdoptPerfHistorySeconds();
  void AdoptPerfCSV();
  void AdoptProfiler();
  void AdoptShutdownTimeoutMs();
  void AdoptSavesPath();
  void AdoptCachePath();

  bool devmode_ = false;
  bool dbgPrint_ = false;
  std::string language_ = "auto";
  bool i18nKeys_ = false;
  std::string languagePath_ = "lang";
  i32 perfHistorySeconds_ = 20;
  bool perfCSV_ = false;
  bool profiler_ = false;
  i32 shutdownTimeoutMs_ = 1500;
  std::string savesPath_;
  std::string cachePath_;

  std::function<void()> devmodeApplier_;
};

}
