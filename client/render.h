#pragma once

// raylib-backed rendering. Kept isolated so raylib types never leak outside
// this pair of files (README §4: bot reuses net_client + prediction, not
// render) — render.h itself stays raylib-free; only render.cpp includes
// <raylib.h>.

#include <cstdint>
#include <string>
#include <vector>

#include "game_types.h"
#include "interpolation.h"
#include "net_client.h" // for GameEvent
#include "prediction.h"

namespace ctf {

// Caller-computed metrics for the HUD (README §6.6) - all things only the
// network layer knows (RTT, unacked count, snapshot buffer depth, actual
// tick rate), not something render.cpp can measure on its own since it only
// sees render frames, not client ticks.
struct HudMetrics {
    double rtt_ms = 0.0;
    uint32_t unacked_input_count = 0;
    double misprediction_rate = 0.0; // Prediction::mispredictions() / snapshot count
    uint32_t snapshot_buffer_depth = 0;
    double actual_tick_rate_hz = 0.0;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(int width, int height, const char* title);
    void shutdown();
    bool should_close() const;

    // Feeds one decoded TCP event so the renderer can track state the
    // per-tick WORLD_SNAPSHOT doesn't carry. Specifically: a dropped flag's
    // position is only ever sent once, in FLAG_DROPPED (README §5.4) -
    // WorldSnapshot (§5.5) carries flag_state_red/blue and
    // flag_carrier_red/blue only, no position field for the Dropped case.
    // Call this for every event NetClient::take_events() returns; other
    // event types are ignored here.
    void on_event(const GameEvent& ev);

    // Draws one frame.
    //   latest         - most recent WORLD_SNAPSHOT (raw, authoritative).
    //                    Used directly for every remote player when F2 is
    //                    off, and for the local player when F1 is off or
    //                    for the F3 ghost.
    //   my_player_id   - which entry in `latest` is "you".
    //   prediction     - supplies the local player's predicted position
    //                    (used when F1 is on, the default).
    //   interpolation  - supplies remote players' smoothed positions (used
    //                    when F2 is on, the default).
    //   hud            - README §6.6's HUD metrics, computed by the caller.
    void draw_frame(const WorldSnapshot& latest, uint8_t my_player_id,
                    const Prediction& prediction,
                    const Interpolation& interpolation, const HudMetrics& hud);

    // Minimal lobby/handshake-wait screen (README §5.6 steps 3-5 take a
    // little wall-clock time - UDP_HELLO resends, LOBBY_STATE broadcasts).
    // Kept here rather than in main.cpp so this stays the only file that
    // issues raylib drawing calls.
    void draw_waiting_screen(const char* line1, const char* line2);

    // Match results screen shown after MATCH_END.
    void draw_results_screen();

    // Debug toggle state (README §6.6). The toggles themselves are polled
    // from raylib key state (F1/F2/F3) inside draw_frame() - this is the
    // one file allowed to touch raylib (README §4) - these accessors exist
    // for tests/HUD text/logging.
    bool prediction_enabled() const { return prediction_enabled_; }
    bool interpolation_enabled() const { return interpolation_enabled_; }
    bool show_server_ghost() const { return show_server_ghost_; }

    // Call when returning to lobby to clear match results state.
    void reset_match_state() { match_ended_ = false; }
    bool match_ended() const { return match_ended_; }

private:
    bool initialized_ = false;
    bool prediction_enabled_ = true;    // F1 (on = predicted, per checklist
                                        // wording "F1 - prediction off")
    bool interpolation_enabled_ = true; // F2
    bool show_server_ghost_ = false;    // F3

    // Cached from FLAG_DROPPED TCP events - see on_event(). Only meaningful
    // while the corresponding flag_state is FlagState::Dropped.
    Vec2Fixed dropped_flag_pos_red_{};
    Vec2Fixed dropped_flag_pos_blue_{};

    // Shot tracers for visual feedback (MsgShotFired events).
    struct Tracer {
        Vec2Fixed origin;
        Vec2Fixed hit_point;
        double spawn_time;
    };
    std::vector<Tracer> tracers_;
    static constexpr double kTracerLifetimeSec = 0.1; // 100ms fade

    // On-screen message feed for game events (kills, flag drops, captures).
    struct GameMessage {
        std::string text;
        double spawn_time;
    };
    std::vector<GameMessage> messages_;
    static constexpr double kMessageLifetimeSec = 4.0;

    // Match results state.
    bool match_ended_ = false;
    Team winner_ = Team::Red;
    uint8_t final_score_red_ = 0;
    uint8_t final_score_blue_ = 0;
};

} // namespace ctf
