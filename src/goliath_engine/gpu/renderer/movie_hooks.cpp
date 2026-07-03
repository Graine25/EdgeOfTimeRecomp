#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "src/goliath_engine/kernel/guest_memory.h"
#include "src/goliath_engine/gpu/renderer/render_state.h"

namespace gmem = eot::kernel::memory;

REXCVAR_DEFINE_BOOL(eot_skip_startup_movies, true, "EdgeOfTime/Movie",
                    "Skip the Activision/Beenox/Marvel boot logo movies and go "
                    "straight to the title screen.");

namespace {

uint32_t g_startupLogoSkips = 0;
std::mutex g_skipMutex;
std::unordered_set<uint32_t> g_skippedPlayers;

bool ContainsAsciiInsensitive(const char* text, const char* needle) {
  if (!text || !needle || !*needle) return false;
  const size_t needleLen = std::strlen(needle);
  for (const char* p = text; *p; ++p) {
    size_t i = 0;
    for (; i < needleLen; ++i) {
      char a = p[i], b = needle[i];
      if (!a) return false;
      if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
      if (a != b) break;
    }
    if (i == needleLen) return true;
  }
  return false;
}

bool IsBootLogoMovieDescriptor(uint8_t* base, uint32_t descVA) {
  if (descVA < 0x1000) return false;

  const uint32_t size = gmem::ReadU32(base, descVA);
  if (size != 168) return false;

  std::array<char, 256> path{};
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    const uint8_t c = gmem::ReadU8(base, descVA + 4 + static_cast<uint32_t>(i));
    path[i] = static_cast<char>(c);
    if (!c) break;
  }

  return ContainsAsciiInsensitive(path.data(), "LogoIntros") ||
         ContainsAsciiInsensitive(path.data(), "ActivisionIntro") ||
         ContainsAsciiInsensitive(path.data(), "BeenoxIntro") ||
         ContainsAsciiInsensitive(path.data(), "MarvelIntro");
}

bool ShouldSkipStartupMovie(uint8_t* base, uint32_t descVA, bool resourceBacked) {
  if (!REXCVAR_GET(eot_skip_startup_movies)) return false;
  if (g_startupLogoSkips >= 3) return false;

  if (IsBootLogoMovieDescriptor(base, descVA)) {
    ++g_startupLogoSkips;
    return true;
  }

  if (resourceBacked && descVA >= 0x1000 && gmem::ReadU32(base, descVA) == 168) {
    ++g_startupLogoSkips;
    return true;
  }

  return false;
}

bool IsSkippedPlayer(uint32_t playerVA) {
  std::lock_guard<std::mutex> lock(g_skipMutex);
  return g_skippedPlayers.contains(playerVA);
}

void TrackSkippedPlayer(uint32_t playerVA) {
  if (playerVA < 0x1000) return;
  std::lock_guard<std::mutex> lock(g_skipMutex);
  g_skippedPlayers.insert(playerVA);
}

}

REX_EXTERN(__imp__sub_82319638);
REX_HOOK_RAW(sub_82319638) {
  const uint32_t player = ctx.r3.u32;
  const uint32_t yuvBuffers = ctx.r4.u32;
  __imp__sub_82319638(ctx, base);
  if (IsSkippedPlayer(player)) return;
  if (ctx.r3.u32 == 1) eot::render::SubmitMovieFrame(base, yuvBuffers);
}

REX_EXTERN(__imp__sub_820851F8);
REX_HOOK_RAW(sub_820851F8) {
  const uint32_t movieObject = ctx.r3.u32;
  __imp__sub_820851F8(ctx, base);
  if (REXCVAR_GET(eot_skip_startup_movies) && ctx.r3.u32 &&
      g_startupLogoSkips < 3 && movieObject >= 0x1000) {
    const uint32_t player = gmem::ReadU32(base, movieObject + 0);
    TrackSkippedPlayer(player);
    ++g_startupLogoSkips;
    REXGPU_INFO("[movie] forcing startup movie {} to PLAYEND", g_startupLogoSkips);
  }
}

REX_EXTERN(__imp__sub_82315D70);
REX_HOOK_RAW(sub_82315D70) {
  const uint32_t player = ctx.r3.u32;
  if (IsSkippedPlayer(player)) {
    if (ctx.r4.u32 >= 0x1000) gmem::WriteU32(base, ctx.r4.u32, 0);
    ctx.r3.u64 = 7;
    return;
  }
  __imp__sub_82315D70(ctx, base);
}

REX_EXTERN(__imp__eot_GLAPIMovie_PlayMovie);
REX_HOOK_RAW(eot_GLAPIMovie_PlayMovie) {
  if (ShouldSkipStartupMovie(base, ctx.r4.u32, false)) {
    REXGPU_INFO("[movie] skipped startup logo movie");
    ctx.r3.u64 = 1;
    return;
  }
  __imp__eot_GLAPIMovie_PlayMovie(ctx, base);
}

REX_EXTERN(__imp__eot_GLAPIMovie_PlayMovieFromResource);
REX_HOOK_RAW(eot_GLAPIMovie_PlayMovieFromResource) {
  if (ShouldSkipStartupMovie(base, ctx.r5.u32, true)) {
    REXGPU_INFO("[movie] skipped startup resource logo movie");
    ctx.r3.u64 = 1;
    return;
  }
  __imp__eot_GLAPIMovie_PlayMovieFromResource(ctx, base);
}
