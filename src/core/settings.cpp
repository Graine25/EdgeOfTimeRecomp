#include "core/settings.h"

#include <charconv>
#include <system_error>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, eot_devmode);
REXCVAR_DECLARE(bool, eot_dbgprint);
REXCVAR_DECLARE(std::string, eot_language);
REXCVAR_DECLARE(bool, eot_i18n_keys);
REXCVAR_DECLARE(std::string, eot_lang_path);
REXCVAR_DECLARE(i32, eot_perf_history_seconds);
REXCVAR_DECLARE(bool, eot_perf_csv);
REXCVAR_DECLARE(bool, eot_profiler);
REXCVAR_DECLARE(i32, eot_shutdown_timeout_ms);
REXCVAR_DECLARE(std::string, eot_saves_path);
REXCVAR_DECLARE(std::string, eot_cache_path);

REXCVAR_DEFINE_BOOL(eot_devmode, false, kCvarGroup,
                    "Developer mode: debug menu boot, Mindows overlay (F11) "
                    "and debug keyboard input.");
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(eot_dbgprint, false, kCvarGroup,
                    "Forward guest DbgPrint output to the host log.");

REXCVAR_DEFINE_STRING(eot_language, "auto", kCvarGroup,
                      "UI text language: auto, us, jp, de, fr, es, it, kr, "
                      "tw, cn, po. Requires restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(eot_i18n_keys, false, kCvarGroup,
                    "Show catalog keys instead of translated text, so a string "
                    "on screen names the entry that produced it.");

REXCVAR_DEFINE_STRING(eot_lang_path, "lang", kCvarGroup,
                      "Folder searched for a localization.toml of UI text "
                      "overrides. Relative to the app data folder.");

REXCVAR_DEFINE_INT32(eot_perf_history_seconds, 20, kCvarGroup,
                     "Seconds of per-frame telemetry retained for the F3 "
                     "overlay and CSV capture.")
    .range(5, 120);

REXCVAR_DEFINE_BOOL(eot_perf_csv, false, kCvarGroup,
                    "Write per-frame telemetry to logs/perf/*.csv. Toggling "
                    "starts a new file.");

REXCVAR_DEFINE_BOOL(eot_profiler, false, kCvarGroup,
                    "Start the Tracy profiler at boot so a viewer can attach. "
                    "Read at startup only, and inert in playtest builds, which "
                    "compile the profiler out.");

REXCVAR_DEFINE_INT32(eot_shutdown_timeout_ms, 1500, kCvarGroup,
                     "Budget for the ordered shutdown (guest quiesce + GPU "
                     "drain) before the process is killed outright.")
    .range(100, 30000);

REXCVAR_DEFINE_STRING(
    eot_saves_path, "", "eot",
    "Save-game directory. Empty = <install_root>/saves (beside the game).");
REXCVAR_DEFINE_STRING(
    eot_cache_path, "", "eot",
    "Transient cache directory (PSO capture). Empty = <exe_dir>/cache.");

namespace eot {

std::string FormatCvar(i32 v) { return std::to_string(v); }
std::string FormatCvar(bool v) { return v ? "true" : "false"; }

std::string FormatCvar(f64 v) {
  char buf[32];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  return ec == std::errc() ? std::string(buf, end) : std::string("0");
}

Settings &Settings::Get() {
  static Settings s;
  return s;
}

void Settings::AdoptDevmode() {
  devmode_ = REXCVAR_GET(eot_devmode);
  if (devmodeApplier_)
    devmodeApplier_();
}
void Settings::AdoptDbgPrint() { dbgPrint_ = REXCVAR_GET(eot_dbgprint); }
void Settings::AdoptLanguage() { language_ = REXCVAR_GET(eot_language); }
void Settings::AdoptI18nKeys() { i18nKeys_ = REXCVAR_GET(eot_i18n_keys); }
void Settings::AdoptLanguagePath() {
  languagePath_ = REXCVAR_GET(eot_lang_path);
}
void Settings::AdoptPerfHistorySeconds() {
  perfHistorySeconds_ = REXCVAR_GET(eot_perf_history_seconds);
}
void Settings::AdoptPerfCSV() { perfCSV_ = REXCVAR_GET(eot_perf_csv); }
void Settings::AdoptProfiler() { profiler_ = REXCVAR_GET(eot_profiler); }
void Settings::AdoptShutdownTimeoutMs() {
  shutdownTimeoutMs_ = REXCVAR_GET(eot_shutdown_timeout_ms);
}
void Settings::AdoptSavesPath() { savesPath_ = REXCVAR_GET(eot_saves_path); }
void Settings::AdoptCachePath() { cachePath_ = REXCVAR_GET(eot_cache_path); }

bool Settings::SetDevmode(bool v) {
  return rex::cvar::SetFlagByName("eot_devmode", FormatCvar(v));
}

void Settings::SetDevmodeApplier(std::function<void()> applier) {
  devmodeApplier_ = std::move(applier);
  if (devmodeApplier_)
    devmodeApplier_();
}

bool Settings::SetDbgPrint(bool v) {
  return rex::cvar::SetFlagByName("eot_dbgprint", FormatCvar(v));
}

bool Settings::SetLanguage(const std::string &v) {
  return rex::cvar::SetFlagByName("eot_language", v);
}

bool Settings::SetI18nKeys(bool v) {
  return rex::cvar::SetFlagByName("eot_i18n_keys", FormatCvar(v));
}

bool Settings::SetPerfHistorySeconds(i32 v) {
  return rex::cvar::SetFlagByName("eot_perf_history_seconds", FormatCvar(v));
}

bool Settings::SetPerfCSV(bool v) {
  return rex::cvar::SetFlagByName("eot_perf_csv", FormatCvar(v));
}

bool Settings::SetShutdownTimeoutMs(i32 v) {
  return rex::cvar::SetFlagByName("eot_shutdown_timeout_ms", FormatCvar(v));
}

void Settings::AdoptCvars() {
  AdoptDevmode();
  AdoptDbgPrint();
  AdoptLanguage();
  AdoptI18nKeys();
  AdoptLanguagePath();
  AdoptPerfHistorySeconds();
  AdoptPerfCSV();
  AdoptProfiler();
  AdoptShutdownTimeoutMs();
  AdoptSavesPath();
  AdoptCachePath();
}

void Settings::Init() {
  AdoptCvars();

  auto reg = [](const char *name, void (Settings::*adopt)()) {
    rex::cvar::RegisterChangeCallback(
        name, [adopt](std::string_view, std::string_view) {
          (Settings::Get().*adopt)();
        });
  };
  reg("eot_devmode", &Settings::AdoptDevmode);
  reg("eot_dbgprint", &Settings::AdoptDbgPrint);
  reg("eot_language", &Settings::AdoptLanguage);
  reg("eot_i18n_keys", &Settings::AdoptI18nKeys);
  reg("eot_lang_path", &Settings::AdoptLanguagePath);
  reg("eot_perf_history_seconds", &Settings::AdoptPerfHistorySeconds);
  reg("eot_perf_csv", &Settings::AdoptPerfCSV);
  reg("eot_profiler", &Settings::AdoptProfiler);
  reg("eot_shutdown_timeout_ms", &Settings::AdoptShutdownTimeoutMs);
  reg("eot_saves_path", &Settings::AdoptSavesPath);
  reg("eot_cache_path", &Settings::AdoptCachePath);
}

}
