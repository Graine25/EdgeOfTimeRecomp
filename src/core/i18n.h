/**
 * @file    core/i18n.h
 * @brief   Localized text for the host's own UI: settings, config menu,
 *          installer wizard.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <rex/types.h>

namespace eot::i18n {

void SetLocale(u32 locale);
u32 CurrentLocale();

void SyncLocale();

void OnLocaleChanged(std::function<void()> cb);

const std::string &Text(std::string_view key);

template <typename... A> std::string Fmt(std::string_view key, A &&...args) {
  const std::string &pattern = Text(key);
  try {
    return fmt::vformat(pattern, fmt::make_format_args(args...));
  } catch (const fmt::format_error &) {
    return pattern;
  }
}

using GameTermResolver = std::function<bool(
    std::string_view file, std::string_view key, u32 locale, std::string &out)>;
void SetGameTermResolver(GameTermResolver resolver);

}
