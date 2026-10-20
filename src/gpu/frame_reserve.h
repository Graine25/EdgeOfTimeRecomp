// gpu/frame_reserve.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <memory>

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

// The frame's large images, made on a background thread behind the boot from the shapes the
// last session made, for CreateHostTexture to hand out on an exact match.
void FrameReserveStart(plume::RenderDevice *device);

// A never-used image of exactly this description, or null. The caller treats it as new.
std::unique_ptr<plume::RenderTexture> FrameReserveTake(const plume::RenderTextureDesc &desc);

// Notes a surface, resolve mirror, TAA or velocity image of 4 MB or more for the next boot.
void FrameReserveNote(const plume::RenderTextureDesc &desc, const char *tag);

// Writes the shapes when they changed and releases the images nobody took. File I/O.
void FrameReserveHousekeep(bool shutdown);
// The same, on a thread of its own.
void FrameReserveHousekeepAsync();

void FrameReserveStop();

}
