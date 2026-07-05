/**
 * @file    core/dbg_print.cpp
 * @brief   Guest DbgPrint relay into the host log.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/encoding.h"
#include "core/logging.h"
#include "core/settings.h"

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/format.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/types.h>

#include <cctype>
#include <string>

u32 eotDebugPrintHook(mapped_string fmt) {
  if (!eot::Settings::Get().DbgPrint())
    return 1;

  u32 fmt_addr = fmt.guest_address();
  if (fmt_addr < 0x82000000u || fmt_addr >= 0xC0000000u)
    return 1;

  auto &ctx = *rex::runtime::current_ppc_context();
  auto *base = REX_KERNEL_MEMORY()->virtual_membase();

  rex::system::format::StackArgList args(ctx, base, 1);
  rex::system::format::StringFormatData data(
      reinterpret_cast<const u8 *>(fmt.host_address()));

  i32 count = rex::system::format::format_core(base, data, args,
                                               false);
  if (count <= 0)
    return 1;

  auto str = data.str();
  while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back())))
    str.pop_back();
  if (str.empty())
    return 1;

  str = eot::SjisToUtf8(str);

  EOT_INFO("[dbg] {}", str);
  return 1;
}
REX_HOOK(eot_DebugPrint, eotDebugPrintHook);
