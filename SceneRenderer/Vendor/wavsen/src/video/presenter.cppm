export module wavsen.video:presenter;

import rstd.cppstd;

export namespace wavsen::video {

// PTS → wall-clock pacing helper for the video plugin's render loop.
//
// Behavior (default — no external clock):
//   - First frame primes the baseline (t0_wall = now; t0_pts = pts).
//   - Subsequent frames sleep until t0_wall + (pts - t0_pts).
//   - PTS that drops backwards (loop wrap-around) re-baselines silently.
//   - Frames more than `max_lag` behind schedule are dropped (return
//     false) so a slow consumer or stalled decoder doesn't snowball.
//   - Frames more than `max_sleep` ahead of schedule treat the PTS as
//     a forward discontinuity and re-baseline rather than sleep forever.
//   - Frames with pts<0 (PTS unavailable) skip pacing entirely.
//
// External-clock mode (set_external_clock(fn) with fn returning the
// current audio playback PTS in seconds, NaN if not yet primed):
//   - skew = pts - clock_fn()
//   - skew < -max_lag      ⇒ drop (return false)
//   - skew >  max_sleep    ⇒ treat as discontinuity, present immediately
//   - skew >  0            ⇒ sleep_for(skew)
//   - else                 ⇒ present immediately
//   - If clock_fn returns NaN, fall back to the wall-clock algorithm.
class Presenter {
public:
    using Clock     = std::chrono::steady_clock;
    using Duration  = Clock::duration;
    using TimePoint = Clock::time_point;

    explicit Presenter(Duration max_lag   = std::chrono::milliseconds(250),
                       Duration max_sleep = std::chrono::seconds(1))
        : max_lag_(max_lag), max_sleep_(max_sleep) {}

    // Force the next call to re-prime the baseline. Useful when the
    // caller knows the stream just looped or the decoder was reset.
    void reset() { primed_ = false; t0_pts_ = -1.0; external_drop_streak_ = 0; }

    // Install an external (audio) master clock. Pass nullptr to revert to
    // wall-clock pacing.
    void set_external_clock(std::function<double()> clock_fn) {
        clock_fn_ = std::move(clock_fn);
    }

    // Returns true if the caller should render the frame now (possibly
    // after sleeping); false if the frame is too far behind schedule and
    // should be dropped. Always advances the baseline on drop so we
    // recover instead of dropping every subsequent frame too.
    bool present_frame(double pts_seconds) {
        if (pts_seconds < 0.0) return true;

        if (clock_fn_) {
            const double now_pts = clock_fn_();
            if (!std::isnan(now_pts)) {
                const double skew = pts_seconds - now_pts;
                const double max_lag_s   = std::chrono::duration<double>(max_lag_).count();
                const double max_sleep_s = std::chrono::duration<double>(max_sleep_).count();
                if (skew < -max_lag_s) {
                    // Video far behind the audio clock. Normally drop,
                    // but if we've been dropping for a long stretch the
                    // decoder can't catch up to where audio is — let one
                    // frame through to break the infinite-drop loop and
                    // re-converge on a closer PTS over the next frames.
                    if (++external_drop_streak_ >= kExternalDropStreakReset) {
                        external_drop_streak_ = 0;
                        return true;
                    }
                    return false;
                }
                external_drop_streak_ = 0;
                if (skew >  max_sleep_s) return true;
                if (skew > 0.0) {
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(skew));
                }
                return true;
            }
            // External clock not primed yet — fall through to wall-clock.
        }

        const auto now = Clock::now();
        if (!primed_) {
            t0_wall_ = now;
            t0_pts_  = pts_seconds;
            primed_  = true;
            return true;
        }
        if (pts_seconds < t0_pts_) {
            t0_wall_ = now;
            t0_pts_  = pts_seconds;
            return true;
        }

        const auto delta = std::chrono::duration_cast<Duration>(
            std::chrono::duration<double>(pts_seconds - t0_pts_));
        const auto target = t0_wall_ + delta;

        if (target + max_lag_ < now) {
            t0_wall_ = now;
            t0_pts_  = pts_seconds;
            return false;
        }
        if (now < target) {
            if (target - now > max_sleep_) {
                t0_wall_ = now;
                t0_pts_  = pts_seconds;
                return true;
            }
            std::this_thread::sleep_until(target);
        }
        return true;
    }

private:
    // External-clock drop streak before forcing one present-through.
    // 30 ≈ 1 s @ 30 fps — long enough to ride out a real backpressure
    // burst, short enough that A/V doesn't drift past noticeable.
    static constexpr std::uint32_t kExternalDropStreakReset = 30;

    Duration      max_lag_;
    Duration      max_sleep_;
    TimePoint     t0_wall_ {};
    double        t0_pts_  { -1.0 };
    bool          primed_  { false };
    std::uint32_t external_drop_streak_ { 0 };
    std::function<double()> clock_fn_;
};

} // namespace wavsen::video
