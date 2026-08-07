// SceneSnapshot — writes a still of the scene's live frame to a file.
//
// Mirage.app installs that still as the macOS desktop picture so the menu bar
// and Dock tint agree with the wallpaper (the "override wallpaper" option).
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

namespace mirage {

// Returns true when `path` was written (HEIC, falling back to JPEG on Macs with
// no HEVC encoder). Gives up after roughly `timeout_seconds` without a frame,
// which is what happens when the wallpaper is paused while fully occluded.
bool WriteSceneSnapshot(const std::string& path, double timeout_seconds = 4.0);

} // namespace mirage
