#pragma once

#include <rex/types.h>

#include <rex/graphics/pipeline/texture/info.h>

namespace eot::gpu {

struct VideoState;
struct GuestTexture;

namespace texrep {

bool Active();

bool TryCreateReplacementImage(VideoState &s, GuestTexture &t,
                               const rex::graphics::TextureInfo &info);

void UploadReplacement(VideoState &s, GuestTexture &t);

void MaybeDumpTexture(const GuestTexture &t, const rex::graphics::TextureInfo &info);

}

}
