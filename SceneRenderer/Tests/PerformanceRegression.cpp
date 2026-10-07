//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "../Tools/SceneWallpaper/ControlChannel.h"
#include "../Vendor/wavsen/src/audio/audio_loopback_analyzer.hpp"

import sr.timer;
import sr.text;
import sr.scene_wallpaper;
import sr.scene_uniform_updater;
import sr.spec_texs;
import wavsen.audio;

namespace
{
int  failures = 0;
void Check(bool ok, const char* message) {
    if (! ok) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void TestTimerCompletion() {
    std::mutex              mutex;
    std::condition_variable changed;
    unsigned                attempts = 0;
    sr::ThreadTimer         timer([&] {
        std::lock_guard lock(mutex);
        ++attempts;
        changed.notify_all();
        return false;
    });
    timer.SetInterval(std::chrono::milliseconds(2));
    timer.Start();
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock,
                               std::chrono::seconds(5),
                               [&] {
                                   return attempts == 1;
                               }),
              "timer attempts a frame at its deadline");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    {
        std::lock_guard lock(mutex);
        Check(attempts == 1, "busy timer does not poll while awaiting completion");
    }
    timer.NotifyReady();
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock,
                               std::chrono::seconds(5),
                               [&] {
                                   return attempts == 2;
                               }),
              "completion wakes an overdue frame");
    }
    timer.Stop();
    timer.Start();
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock,
                               std::chrono::seconds(5),
                               [&] {
                                   return attempts == 3;
                               }),
              "timer can restart after stopping a completion wait");
    }
    timer.Stop();

    std::atomic<unsigned> early_attempts { 0 };
    sr::ThreadTimer*      early_ptr = nullptr;
    sr::ThreadTimer       early([&] {
        if (early_attempts.fetch_add(1) == 0) early_ptr->NotifyReady();
        return false;
    });
    early_ptr = &early;
    early.SetInterval(std::chrono::milliseconds(2));
    early.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (early_attempts.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(early_attempts.load() == 2, "completion before the wait is not lost");
    early.Stop();
}

void TestUnchangedText() {
    auto                blob = sr::text::FontCache::ResolveSystemFont("Arial");
    sr::text::FontCache fonts;
    auto*               face = fonts.GetFace(blob.bytes, 20, blob.face_index);
    Check(face != nullptr, "system font is available for text regression");
    if (! face) return;
    face->Populate(sr::text::DecodeUtf8("AB"));
    auto mesh = std::make_shared<sr::SceneMesh>(true);
    mesh->AddVertexArray(sr::SceneVertexArray(
        sr::MakeAttrSet({ sr::VAttr::Position, sr::VAttr::TexCoord, sr::VAttr::Color }), 4096));
    mesh->AddIndexArray(sr::SceneIndexArray(6144));
    sr::text::TextLayouter text(face, mesh, {}, 1024);
    text.SetText("AB");
    auto&      vertices          = mesh->GetVertexArray(0);
    auto&      indices           = mesh->GetIndexArray(0);
    const auto vertex_generation = vertices.DataGeneration();
    const auto index_generation  = indices.DataGeneration();
    mesh->ConsumeDirtyFlags(sr::SceneMeshDirtyAll);
    text.SetText("AB");
    Check(vertices.DataGeneration() == vertex_generation &&
              mesh->DirtyFlags() == sr::SceneMeshDirtyNone,
          "identical text leaves vertex data and mesh dirty state untouched");
    text.SetAlpha(0.5f);
    Check(vertices.DataGeneration() != vertex_generation, "style changes relayout identical text");
    text.SetText("A");
    Check(vertices.VertexCount() == 4 && indices.RenderDataCount() == 6,
          "short text only uploads and draws its active quad");
    Check(indices.StaticTopology() && indices.DataGeneration() == index_generation,
          "text edits preserve the static index buffer");
    text.SetText("");
    Check(vertices.VertexCount() == 0 && indices.RenderDataCount() == 0,
          "empty text clears draw counts without uploading the unused capacity");
    text.SetText("AB");
    Check(vertices.VertexCount() == 8 && indices.RenderDataCount() == 12,
          "text can grow again after becoming empty");
    auto* larger = fonts.GetFace(blob.bytes, 40, blob.face_index);
    Check(larger != nullptr, "alternate font size initializes");
    if (larger) {
        larger->Populate(sr::text::DecodeUtf8("AB"));
        text.SetFace(larger);
        unsigned retired = 0;
        fonts.TrimUnusedFaces({}, [&](std::string_view) {
            ++retired;
        });
        Check(retired == 1 && fonts.Faces().size() == 1 && text.Face() == larger,
              "font replacement retires the unreferenced face and preserves active layout");
        text.SetText("A");
        Check(vertices.VertexCount() == 4, "layout remains valid after font cache trimming");
    }
}

void TestIdleAudio() {
    wavsen::audio::SoundManager sound;
    Check(! sound.init() && ! sound.is_inited(), "empty scene does not initialize audio output");
    sound.play();
    sound.set_muted(false);
    Check(! sound.is_inited(), "play and unmute without tracks keep audio output idle");
    sound.pause();

    std::array<float, 8192> pcm {};
    wavsen::audio::loopback::ingest(pcm.data(), 4096, 2, 48000);
    wavsen::audio::loopback::SpectrumSnapshot spectrum;
    Check(! wavsen::audio::loopback::snapshot(spectrum), "no FFT is published without a consumer");
    wavsen::audio::AudioCapture capture;
    Check(capture.init(false), "scene-only spectrum consumer initializes without system capture");
    wavsen::audio::loopback::ingest(pcm.data(), 4096, 2, 48000);
    Check(wavsen::audio::loopback::snapshot(spectrum), "subscribed scene output produces spectrum");
    capture.uninit();
    capture.uninit();
}

void TestOnDemandEligibility() {
    sr::Scene scene;
    auto      updater        = std::make_unique<sr::SceneUniformUpdater>(&scene);
    auto*     uniforms       = updater.get();
    scene.shaderValueUpdater = std::move(updater);
    scene.RebuildResourceIndex();
    Check(sr::SceneCanRenderOnDemand(scene), "constant scene can render on demand");
    scene.textures["video"].isVideo = true;
    Check(! sr::SceneCanRenderOnDemand(scene), "video texture requires continuous frames");
    scene.textures.clear();
    scene.textures["sprite"].isSprite = true;
    Check(! sr::SceneCanRenderOnDemand(scene), "sprite animation requires continuous frames");
    scene.textures.clear();
    scene.renderTargets["_rt_history"] = { .width = 16, .height = 16 };
    Check(! sr::SceneCanRenderOnDemand(scene),
          "history/effect targets conservatively stay dynamic");
    scene.renderTargets.clear();
    uniforms->SetCameraParallaxEnabled(true);
    Check(! sr::SceneCanRenderOnDemand(scene), "parallax remains interactive");
    uniforms->SetCameraParallaxEnabled(false);
    sr::SceneNode node;
    uniforms->InitUniforms(&node, [](std::string_view name) {
        return name == "g_Time";
    });
    Check(! sr::SceneCanRenderOnDemand(scene), "time-dependent shaders keep animating");
}

void TestControlShutdown() {
    const int original_stdin = ::dup(STDIN_FILENO);
    int       input[2];
    if (::pipe(input) != 0) {
        Check(false, "create control fixture pipe");
        return;
    }
    ::dup2(input[0], STDIN_FILENO);
    ::close(input[0]);
    {
        sr::SceneWallpaper          wallpaper;
        mirage::SceneControlChannel channel(wallpaper, [] {
        });
        channel.start();
        // No stdin data or EOF: stop must interrupt the blocking read itself.
        channel.stop();
        channel.start();
        channel.stop();
    }
    if (original_stdin >= 0) {
        ::dup2(original_stdin, STDIN_FILENO);
        ::close(original_stdin);
    } else
        ::close(STDIN_FILENO);
    ::close(input[1]);
}
} // namespace

int main() {
    TestTimerCompletion();
    TestUnchangedText();
    TestIdleAudio();
    TestOnDemandEligibility();
    TestControlShutdown();
    if (! failures) std::cout << "PerformanceRegression: ok\n";
    return failures ? 1 : 0;
}
