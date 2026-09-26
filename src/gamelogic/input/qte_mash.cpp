#include <rex/hook.h>

#include "gamelogic/ui/binds_client.h"

REX_EXTERN(__imp__eot_WebPP_UpdateOpening); // (this r3, dt f1): a frame of the mash

REX_HOOK_RAW(eot_WebPP_UpdateOpening) {
  __imp__eot_WebPP_UpdateOpening(ctx, base);
  if (const auto note = eot::ui::binds::Api().mash_prompt)
    note();
}
