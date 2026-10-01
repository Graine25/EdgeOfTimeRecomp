#pragma once

#include <cstdint>

#include "core/export.h"
#include "goliath/controller/binds_api.h"

namespace eot::ui::binds {

struct Host {
  int32_t (*capture_begin)(int32_t) = nullptr;
  int32_t (*capture_poll)(char *, int32_t) = nullptr;
  void (*capture_end)() = nullptr;
  int32_t (*ctrl_pressed)() = nullptr;
  void (*changed)() = nullptr;
  int32_t (*key_glyph)(int32_t) = nullptr;
  int32_t (*pad_glyph)(int32_t) = nullptr;
  void (*bar_object)(int32_t) = nullptr;
  void (*bar_zone)(int32_t) = nullptr;
  void (*mash_prompt)(int32_t) = nullptr;

  bool Bound() const { return capture_begin && capture_poll && capture_end && changed; }
};

inline const Host &Api() {
  static const Host host = [] {
    Host h;
    h.capture_begin = reinterpret_cast<decltype(h.capture_begin)>(HostEntryPoint("eot_binds_capture_begin"));
    h.capture_poll = reinterpret_cast<decltype(h.capture_poll)>(HostEntryPoint("eot_binds_capture_poll"));
    h.capture_end = reinterpret_cast<decltype(h.capture_end)>(HostEntryPoint("eot_binds_capture_end"));
    h.ctrl_pressed = reinterpret_cast<decltype(h.ctrl_pressed)>(HostEntryPoint("eot_binds_ctrl_pressed"));
    h.changed = reinterpret_cast<decltype(h.changed)>(HostEntryPoint("eot_binds_changed"));
    h.key_glyph = reinterpret_cast<decltype(h.key_glyph)>(HostEntryPoint("eot_binds_key_glyph"));
    h.pad_glyph = reinterpret_cast<decltype(h.pad_glyph)>(HostEntryPoint("eot_binds_pad_glyph"));
    h.bar_object = reinterpret_cast<decltype(h.bar_object)>(HostEntryPoint("eot_prompts_bar_object"));
    h.bar_zone = reinterpret_cast<decltype(h.bar_zone)>(HostEntryPoint("eot_prompts_bar_zone"));
    h.mash_prompt = reinterpret_cast<decltype(h.mash_prompt)>(HostEntryPoint("eot_kbm_mash_prompt"));
    return h;
  }();
  return host;
}

}
