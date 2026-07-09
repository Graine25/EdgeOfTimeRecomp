#pragma once

#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

void QueueTextureUpload(GuestTexture *tex);

void ForgetTextureUpload(GuestTexture *tex);

struct StagingPool {
  std::vector<std::unique_ptr<plume::RenderBuffer>> buffers;
  std::vector<u64> capacities;
  u32 used = 0;

  void Reset() { used = 0; }
};

void FlushTextureUploads(plume::RenderCommandList *cmd, StagingPool &pool);

void NoteMipLockSkipped();

GuestTexture *LastUploadedTexture(u32 preferred_w, u32 preferred_h);

void LogTextureUploadStats();

}
