#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/settings.h"

REXCVAR_DEFINE_BOOL(eot_skip_startup_movies, true, kCvarGroup,
                    "Skip the boot logo movies and go straight to the splash. "
                    "A renderer testing aid: while a logo plays the window "
                    "shows a decoded movie frame rather than anything the game "
                    "drew, which makes rendering bugs impossible to judge by "
                    "eye.");

namespace {

constexpr u32 kStartupMovieBudget = 3;
std::atomic<u32> g_skipped{0};

bool ShouldSkipStartupMovie() {
  if (!REXCVAR_GET(eot_skip_startup_movies))
    return false;
  u32 seen = g_skipped.load(std::memory_order_relaxed);
  while (seen < kStartupMovieBudget) {
    if (g_skipped.compare_exchange_weak(seen, seen + 1,
                                        std::memory_order_relaxed)) {
      EOT_INFO("[movie] skipping startup movie {}/{}", seen + 1,
               kStartupMovieBudget);
      return true;
    }
  }
  return false;
}

}

REX_EXTERN(__imp__eot_GLAPIMovie_PlayMovie);
REX_HOOK_RAW(eot_GLAPIMovie_PlayMovie) {
  if (ShouldSkipStartupMovie()) {
    ctx.r3.u32 = 0;
    return;
  }
  __imp__eot_GLAPIMovie_PlayMovie(ctx, base);
}

REX_EXTERN(__imp__eot_GLAPIMovie_PlayMovieFromResource);
REX_HOOK_RAW(eot_GLAPIMovie_PlayMovieFromResource) {
  if (ShouldSkipStartupMovie()) {
    ctx.r3.u32 = 0;
    return;
  }
  __imp__eot_GLAPIMovie_PlayMovieFromResource(ctx, base);
}
