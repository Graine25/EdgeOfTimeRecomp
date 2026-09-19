#pragma once

#include <cstdint>

#include "core/export.h"
#include "mods/mods_api.h"

namespace eot::ui::mods {

struct Host {
  int32_t (*slot)(int32_t, eot_mod_slot_info *) = nullptr;
  int32_t (*import_begin)(int32_t) = nullptr;
  int32_t (*import_state)(char *, int32_t) = nullptr;
  void (*import_acknowledge)() = nullptr;
  int32_t (*restore)(int32_t, char *, int32_t) = nullptr;
  int32_t (*open_folder)() = nullptr;

  bool Bound() const { return slot && import_begin && import_state && import_acknowledge && restore; }
};

inline const Host &Api() {
  static const Host host = [] {
    Host h;
    h.slot = reinterpret_cast<decltype(h.slot)>(HostEntryPoint("eot_mods_slot"));
    h.import_begin = reinterpret_cast<decltype(h.import_begin)>(HostEntryPoint("eot_mods_import_begin"));
    h.import_state = reinterpret_cast<decltype(h.import_state)>(HostEntryPoint("eot_mods_import_state"));
    h.import_acknowledge =
        reinterpret_cast<decltype(h.import_acknowledge)>(HostEntryPoint("eot_mods_import_acknowledge"));
    h.restore = reinterpret_cast<decltype(h.restore)>(HostEntryPoint("eot_mods_restore"));
    h.open_folder = reinterpret_cast<decltype(h.open_folder)>(HostEntryPoint("eot_mods_open_folder"));
    return h;
  }();
  return host;
}

}
