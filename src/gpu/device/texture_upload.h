#pragma once

#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

void QueueTextureUpload(GuestTexture *tex);

void ForgetTextureUpload(GuestTexture *tex);

void FlushTextureUploads(
    plume::RenderCommandList *cmd,
    std::vector<std::unique_ptr<plume::RenderBuffer>> &keep_alive);

void LogTextureUploadStats();

}
