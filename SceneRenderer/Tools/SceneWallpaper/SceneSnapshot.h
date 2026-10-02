// SceneSnapshot — writes a still of the scene's live frame to a file.
//
// macOS uses the still for desktop integration; Linux exposes the same capture
// contract to MirageQt and encodes it as PNG.
//
// The desktop scene presents through MoltenVK on a real Vulkan swapchain, so
// there is no NSImage to ask for. The engine's existing C ABI
// `SceneRendererSetLiveFrameCallback` is the reusable readback: it delivers the
// composited RGBA8 frame from FinPass::finishFrameDump, and costs nothing at all
// while no callback is registered.
//
// Blocking: called from the control-channel thread, waits for the render thread
// to deliver one frame, then encodes on the calling thread — never on the render
// thread, which only memcpy's the bytes out.

#pragma once

#include <string>
#include <functional>

namespace mirage {

// Returns true when `path` was written (HEIC/JPEG on macOS, PNG on Linux).
// `path` must be a non-empty UTF-8 filesystem path owned by the caller;
// request_frame runs after the capture callback is installed, allowing paused
// and on-demand scenes to supply one frame without resuming. This function is
// thread-safe between the control thread and render callback, but serializes
// concurrent captures because the engine exposes one process-wide callback.
bool WriteSceneSnapshot(const std::string& path, double timeout_seconds = 4.0,
                        std::function<void()> request_frame = {});

} // namespace mirage
