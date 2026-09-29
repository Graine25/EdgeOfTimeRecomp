#pragma once

#include <string>
#include <vector>

#include <rex/types.h>

namespace eot::gpu {

void NoteTextureResident(u32 header_va);

bool TakeTextureResidentAge(u32 header_va, f64 &age_ms);

bool UploadSeenBefore(u64 storage_key);

void NoteTextureReleased(u32 header_va);

void TakeReleasedTextures(std::vector<u32> &out);

bool TakeAnnouncedHeader(u32 &header_va);

void NoteUnannouncedUpload(u32 width, u32 height, u32 guest_format, bool tiled);
std::string TakeUnannouncedShapes();

}
