#include "gpu/patches/movie_aspect.h"

#include <atomic>

#include <rex/hook.h>

REX_EXTERN(__imp__eot_Movie_DrawFrame);

namespace {
std::atomic<bool> g_movie_drawn{false};
}

namespace eot::gpu {

bool TakeMovieDrawnFlag() { return g_movie_drawn.exchange(false, std::memory_order_acq_rel); }

}

REX_HOOK_RAW(eot_Movie_DrawFrame) {
  __imp__eot_Movie_DrawFrame(ctx, base);
  g_movie_drawn.store(true, std::memory_order_release);
}
