#pragma once

#include <cstdint>

#include "core/export.h"
#include "mods/mods_api.h"

namespace eot::ui::mods {

struct Host {
  int32_t (*count)() = nullptr;
  int32_t (*get)(int32_t, eot_mod_info *) = nullptr;
  int32_t (*set_enabled)(const char *, int32_t, char *, int32_t) = nullptr;
  int32_t (*remove)(const char *, char *, int32_t) = nullptr;
  int32_t (*add_begin)() = nullptr;
  int32_t (*import_state)(char *, int32_t) = nullptr;
  void (*import_acknowledge)() = nullptr;
  int32_t (*open_folder)() = nullptr;
  int32_t (*languages)(char *, int32_t) = nullptr;

  bool Bound() const {
    return count && get && set_enabled && remove && add_begin && import_state && import_acknowledge;
  }
};

inline const Host &Api() {
  static const Host host = [] {
    Host h;
    h.count = reinterpret_cast<decltype(h.count)>(HostEntryPoint("eot_mods_count"));
    h.get = reinterpret_cast<decltype(h.get)>(HostEntryPoint("eot_mods_get"));
    h.set_enabled = reinterpret_cast<decltype(h.set_enabled)>(HostEntryPoint("eot_mods_set_enabled"));
    h.remove = reinterpret_cast<decltype(h.remove)>(HostEntryPoint("eot_mods_remove"));
    h.add_begin = reinterpret_cast<decltype(h.add_begin)>(HostEntryPoint("eot_mods_add_begin"));
    h.import_state = reinterpret_cast<decltype(h.import_state)>(HostEntryPoint("eot_mods_import_state"));
    h.import_acknowledge =
        reinterpret_cast<decltype(h.import_acknowledge)>(HostEntryPoint("eot_mods_import_acknowledge"));
    h.open_folder = reinterpret_cast<decltype(h.open_folder)>(HostEntryPoint("eot_mods_open_folder"));
    h.languages = reinterpret_cast<decltype(h.languages)>(HostEntryPoint("eot_mods_languages"));
    return h;
  }();
  return host;
}

}
