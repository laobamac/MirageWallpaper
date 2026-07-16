module;

#include <sys/types.h>
module sr.pkg.parse;
import sr.core;
import sr.scene;
import rstd.cppstd;
import rstd.log;
import rstd; // rstd::io::Result / SeekFrom for IByteStream impl

using namespace sr;

enum class PlaybackMode
{
    Random,
    Loop,
    Single
};

static PlaybackMode ToPlaybackMode(std::string_view s) {
    if (s.compare("loop") == 0)
        return PlaybackMode::Loop;
    else if (s.compare("random") == 0)
        return PlaybackMode::Random;
    else if (s.compare("single") == 0)
        return PlaybackMode::Single;
    return PlaybackMode::Loop;
};

namespace
{

// Adapter: sr::fs::IBinaryStream → wavsen::audio::IByteStream.
class BStreamAdapter : public wavsen::audio::IByteStream {
public:
    explicit BStreamAdapter(std::shared_ptr<fs::IBinaryStream> s): inner(std::move(s)) {}

    auto read(rstd::u8* dst, rstd::usize bytes) -> rstd::io::Result<rstd::usize> override {
        const auto n = inner->Read(dst, bytes);
        return rstd::Ok(static_cast<rstd::usize>(n));
    }

    auto seek(rstd::io::SeekFrom pos) -> rstd::io::Result<rstd::u64> override {
        bool ok = false;
        switch (pos.which) {
        case rstd::io::SeekFrom::Which::Start:
            ok = inner->SeekSet(static_cast<std::int64_t>(pos.offset));
            break;
        case rstd::io::SeekFrom::Which::Current: ok = inner->SeekCur(pos.offset); break;
        case rstd::io::SeekFrom::Which::End: ok = inner->SeekEnd(pos.offset); break;
        }
        if (! ok) {
            return rstd::Err(rstd::io::error::Error::from_kind(
                rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::Other }));
        }
        // sr::fs::IBinaryStream doesn't expose post-seek absolute offset
        // directly, so report 0 — wavsen's stream_decoder ignores the
        // returned position (only checks is_err).
        return rstd::Ok(rstd::u64 { 0 });
    }

private:
    std::shared_ptr<fs::IBinaryStream> inner;
};

struct WPSoundState {
    std::atomic<bool>     playing { false };
    std::atomic<float>    volume { 1.0f };
    std::atomic<uint32_t> play_seq { 0 };
    std::atomic<uint32_t> stop_seq { 0 };
};

class WPSoundControl final : public SceneSoundControl {
public:
    explicit WPSoundControl(std::shared_ptr<WPSoundState> state): m_state(std::move(state)) {}

    void Play() override {
        m_state->playing.store(true, std::memory_order_release);
        m_state->play_seq.fetch_add(1, std::memory_order_acq_rel);
    }
    void Stop() override {
        m_state->playing.store(false, std::memory_order_release);
        m_state->stop_seq.fetch_add(1, std::memory_order_acq_rel);
    }
    void Pause() override { m_state->playing.store(false, std::memory_order_release); }
    bool IsPlaying() const override { return m_state->playing.load(std::memory_order_acquire); }
    void SetVolume(float volume) override {
        m_state->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
    }

private:
    std::shared_ptr<WPSoundState> m_state;
};

} // namespace

class WPSoundStream : public wavsen::audio::SoundStream {
public:
    struct Config {
        float        maxtime { 10.0f };
        float        mintime { 0.0f };
        float        volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
    };
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c,
                  std::shared_ptr<WPSoundState>       state,
                  std::array<std::atomic<float>, 16>* audio_average)
        : vfs(vfs),
          m_config(c),
          m_state(std::move(state)),
          m_soundPaths(paths),
          m_audioAverage(audio_average) {};
    virtual ~WPSoundStream() = default;

    uint64_t next_pcm(void* pData, uint32_t frameCount) override {
        SyncControl();
        if (m_dead) return 0;
        if (! m_state->playing.load(std::memory_order_acquire)) return 0;

        if (! m_curActive) {
            Switch();
        }

        uint64_t frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        if (frameReads == 0 && ! m_dead) {
            m_curActive.reset();
            if (m_config.mode == PlaybackMode::Single) {
                m_state->playing.store(false, std::memory_order_release);
                return 0;
            }
            Switch();
            frameReads = m_curActive ? m_curActive->next_pcm(pData, frameCount) : 0;
        }
        UpdateAudioAverage(pData, frameReads);
        {
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            const auto volume      = m_state->volume.load(std::memory_order_acquire);
            for (unsigned i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= volume;
            }
        }
        return frameReads;
    };
    void pass_desc(const Desc& d) override { m_desc = d; }

    // Walk paths until one opens. If all fail, disable the stream so the
    // audio callback stops re-trying every tick (which spammed FFmpeg's
    // demuxer-probe errors at audio-callback rate).
    void Switch() {
        m_curActive.reset();
        const uint32_t n = static_cast<uint32_t>(m_soundPaths.size());
        if (n == 0) {
            m_dead = true;
            return;
        }
        const uint32_t base = SelectStartIndex(n);
        for (uint32_t tried = 0; tried < n; ++tried) {
            const std::string& path = m_soundPaths[(base + tried) % n];
            auto               bin  = vfs.Open("/assets/" + path);
            if (! bin) continue;
            auto adapter = std::make_shared<BStreamAdapter>(std::move(bin));
            auto stream  = wavsen::audio::make_stream(std::move(adapter), m_desc);
            if (stream) {
                m_curActive = std::move(stream);
                return;
            }
        }
        m_dead = true;
        m_state->playing.store(false, std::memory_order_release);
        rstd::log::warn("WPSoundStream: all {} sound path(s) failed to open; disabling stream", n);
    }
    uint32_t SelectStartIndex(uint32_t n) {
        if (n == 0) return 0;
        if (m_config.mode == PlaybackMode::Random) {
            return Random::get<uint32_t>(0, n - 1);
        }
        if (m_config.mode == PlaybackMode::Single) {
            return Random::get<uint32_t>(0, n - 1);
        }
        uint32_t idx = m_curIndex;
        m_curIndex   = (m_curIndex + 1) % n;
        return idx;
    }

private:
    void SyncControl() {
        const uint32_t stop_seq = m_state->stop_seq.load(std::memory_order_acquire);
        if (stop_seq != m_seenStopSeq) {
            m_seenStopSeq = stop_seq;
            m_curActive.reset();
            m_dead = false;
        }
        const uint32_t play_seq = m_state->play_seq.load(std::memory_order_acquire);
        if (play_seq != m_seenPlaySeq) {
            m_seenPlaySeq = play_seq;
            m_curActive.reset();
            m_dead = false;
        }
    }

    void UpdateAudioAverage(const void* pData, uint64_t frameReads) {
        if (! m_audioAverage || frameReads == 0 || m_desc.channels == 0) return;

        const float* samples = static_cast<const float*>(pData);
        const auto   total   = static_cast<std::size_t>(frameReads * m_desc.channels);
        if (total == 0) return;

        for (std::size_t bin = 0; bin < m_audioAverage->size(); ++bin) {
            const auto begin = bin * total / m_audioAverage->size();
            const auto end   = (bin + 1) * total / m_audioAverage->size();
            if (end <= begin) continue;

            float sum = 0.0f;
            for (std::size_t i = begin; i < end; ++i) sum += std::abs(samples[i]);
            float level = std::clamp(sum / static_cast<float>(end - begin), 0.0f, 1.0f);

            auto&       slot = (*m_audioAverage)[bin];
            const float old  = slot.load(std::memory_order_relaxed);
            slot.store(std::max(old * 0.75f, level), std::memory_order_relaxed);
        }
    }

    fs::VFS&                      vfs;
    Config                        m_config;
    Desc                          m_desc;
    std::shared_ptr<WPSoundState> m_state;
    uint32_t                      m_curIndex { 0 };
    uint32_t                      m_seenPlaySeq { 0 };
    uint32_t                      m_seenStopSeq { 0 };
    bool                          m_dead { false };

    const std::vector<std::string>              m_soundPaths;
    std::unique_ptr<wavsen::audio::SoundStream> m_curActive;
    std::array<std::atomic<float>, 16>*         m_audioAverage { nullptr };
};

std::shared_ptr<SceneSoundControl> WPSoundParser::Parse(const wpscene::SoundObject&  obj,
                                                        fs::VFS&                     vfs,
                                                        wavsen::audio::SoundManager& sm,
                                                        Scene*                       scene) {
    WPSoundStream::Config config { .maxtime = obj.maxtime,
                                   .mintime = obj.mintime,
                                   .volume  = std::clamp(obj.volume, 0.0f, 1.0f),
                                   .mode    = ToPlaybackMode(obj.playbackmode) };

    auto* audio_average = scene ? &scene->audioAverage : nullptr;
    auto  state         = std::make_shared<WPSoundState>();
    state->playing.store(! obj.startsilent, std::memory_order_release);
    state->volume.store(config.volume, std::memory_order_release);
    auto control = std::make_shared<WPSoundControl>(state);
    auto ss      = std::make_unique<WPSoundStream>(obj.sound, vfs, config, state, audio_average);
    sm.mount(std::move(ss));
    return control;
}
