#include "render.h"

#include <cmath>
#include <cstdio>
#include <variant>

#include <raylib.h>

#include "game_config.h"
#include "map.h"
#include "render_policy.h"

// Minimal 2D raylib rendering (README module breakdown, proposal §4.2) plus
// the debug toggles/HUD from README §6.6, built "day one of week 3" per the
// README's own instruction since they double as the evaluation methodology.
//
// Scope note: the implementation_guide.md §3.4 checklist text ("simple
// icons for flags and pickups") carries wording from the original proposal
// (proposal §4.2 module 6). Pickups/powerups are explicitly out of scope in
// README §2 and §12's deviation table ("ITEM_SPAWNED / pickups | Removed").
// Per the ground rule that README wins on conflicts: this file renders
// flags, not pickups.

namespace ctf {

namespace {

// Internal positions are int32 fixed-point, 1/256 px (README §5.1). This
// file draws in raw pixel space 1:1 with map coordinates - it assumes the
// caller sizes the window to match the map (config::kMapWidthPx x
// kMapHeightPx); README doesn't specify a window size, so that sizing
// decision is left to main.cpp (§3.5), not asserted here.
float to_px(int32_t fp) { return static_cast<float>(fp) / config::kFixedScale; }

Color team_color(Team t) { return t == Team::Red ? Color{200, 40, 40, 255}
                                                  : Color{40, 90, 220, 255}; }

void draw_player_box(Vec2Fixed pos, Team team, bool alive, bool is_local) {
    // position is the AABB's top-left corner (verified against
    // shared/map.cpp's aabb_collides: box spans [x, x+size) x [y, y+size),
    // not a center point) - drawn as exactly that box, same anchor the
    // collision system uses.
    const float x = to_px(pos.x);
    const float y = to_px(pos.y);
    const float size = static_cast<float>(config::kPlayerSizePx);
    Color c = team_color(team);
    if (!alive) c = Fade(c, 0.35f); // dead: dim, no collision/render per §7.4
                                    // still drawn faded so death is visible
                                    // rather than the player just vanishing
    DrawRectangle(static_cast<int>(x), static_cast<int>(y),
                 static_cast<int>(size), static_cast<int>(size), c);
    if (is_local) {
        DrawRectangleLines(static_cast<int>(x) - 2, static_cast<int>(y) - 2,
                           static_cast<int>(size) + 4,
                           static_cast<int>(size) + 4, WHITE);
    }
}

void draw_aim_indicator(Vec2Fixed pos, uint16_t aim_angle) {
    const float cx = to_px(pos.x) + config::kPlayerSizePx / 2.0f;
    const float cy = to_px(pos.y) + config::kPlayerSizePx / 2.0f;
    const float radians =
        static_cast<float>(aim_angle) / 65536.0f * 2.0f * PI;
    const float len = config::kPlayerSizePx * 0.9f;
    DrawLineEx(Vector2{cx, cy},
              Vector2{cx + len * cosf(radians), cy + len * sinf(radians)},
              2.0f, YELLOW);
}

void draw_health_bar(Vec2Fixed pos, uint8_t health) {
    const float x = to_px(pos.x);
    const float y = to_px(pos.y) - 6.0f;
    const float w = static_cast<float>(config::kPlayerSizePx);
    const float frac = static_cast<float>(health) / config::kMaxHealth;
    DrawRectangle(static_cast<int>(x), static_cast<int>(y),
                 static_cast<int>(w), 3, DARKGRAY);
    DrawRectangle(static_cast<int>(x), static_cast<int>(y),
                 static_cast<int>(w * frac), 3, GREEN);
}

void draw_map_walls(const Map& map) {
    for (int32_t ty = 0; ty < config::kMapHeightTiles; ++ty) {
        for (int32_t tx = 0; tx < config::kMapWidthTiles; ++tx) {
            if (map.is_wall(tx, ty)) {
                DrawRectangle(tx * config::kTileSizePx, ty * config::kTileSizePx,
                             config::kTileSizePx, config::kTileSizePx, GRAY);
            }
        }
    }
}

void draw_flag(Vec2Fixed pos, Team team) {
    const float cx = to_px(pos.x) + config::kPlayerSizePx / 2.0f;
    const float cy = to_px(pos.y) + config::kPlayerSizePx / 2.0f;
    DrawTriangle(Vector2{cx - 6, cy + 10}, Vector2{cx - 6, cy - 10},
                Vector2{cx + 8, cy - 4}, team_color(team));
    DrawLine(static_cast<int>(cx - 6), static_cast<int>(cy - 10),
             static_cast<int>(cx - 6), static_cast<int>(cy + 10), BLACK);
}

} // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

bool Renderer::init(int width, int height, const char* title) {
    InitWindow(width, height, title);
    if (!IsWindowReady()) return false;
    initialized_ = true;
    return true;
}

void Renderer::shutdown() {
    if (initialized_) {
        CloseWindow();
        initialized_ = false;
    }
}

bool Renderer::should_close() const {
    return !initialized_ || WindowShouldClose();
}

void Renderer::on_event(const GameEvent& ev) {
    if (const auto* dropped = std::get_if<protocol::MsgFlagDropped>(&ev)) {
        if (dropped->flag_team == Team::Red) dropped_flag_pos_red_ = dropped->position;
        else dropped_flag_pos_blue_ = dropped->position;
    }
    // FlagPickedUp/FlagReturned/FlagCaptured all move the flag out of the
    // Dropped state; the cached position is simply not read again until the
    // next FLAG_DROPPED, so nothing needs to happen for those here.
}

void Renderer::draw_waiting_screen(const char* line1, const char* line2) {
    if (!initialized_) return;
    BeginDrawing();
    ClearBackground(Color{20, 20, 24, 255});
    const int w = GetScreenWidth();
    const int h = GetScreenHeight();
    if (line1 != nullptr) {
        const int tw = MeasureText(line1, 24);
        DrawText(line1, (w - tw) / 2, h / 2 - 20, 24, RAYWHITE);
    }
    if (line2 != nullptr) {
        const int tw2 = MeasureText(line2, 18);
        DrawText(line2, (w - tw2) / 2, h / 2 + 14, 18, LIGHTGRAY);
    }
    EndDrawing();
}

void Renderer::draw_frame(const WorldSnapshot& latest, uint8_t my_player_id,
                          const Prediction& prediction,
                          const Interpolation& interpolation,
                          const HudMetrics& hud) {
    if (!initialized_) return;

    // README §6.6: F1/F2/F3 toggles, checked once per frame.
    if (IsKeyPressed(KEY_F1)) prediction_enabled_ = !prediction_enabled_;
    if (IsKeyPressed(KEY_F2)) interpolation_enabled_ = !interpolation_enabled_;
    if (IsKeyPressed(KEY_F3)) show_server_ghost_ = !show_server_ghost_;

    static const Map map; // stateless tile grid (README §7.1)

    BeginDrawing();
    ClearBackground(Color{20, 20, 24, 255});

    draw_map_walls(map);
    DrawRectangle(static_cast<int>(to_px(kRedBasePosition.x)),
                 static_cast<int>(to_px(kRedBasePosition.y)),
                 config::kPlayerSizePx, config::kPlayerSizePx,
                 Fade(team_color(Team::Red), 0.4f));
    DrawRectangle(static_cast<int>(to_px(kBlueBasePosition.x)),
                 static_cast<int>(to_px(kBlueBasePosition.y)),
                 config::kPlayerSizePx, config::kPlayerSizePx,
                 Fade(team_color(Team::Blue), 0.4f));

    // Flags (README §7.4 states; position sourcing per on_event()'s doc
    // comment and render_policy::resolve_flag_position).
    const Vec2Fixed red_flag_pos = render_policy::resolve_flag_position(
        latest.flag_state_red, latest.flag_carrier_red, latest, kRedBasePosition,
        dropped_flag_pos_red_);
    const Vec2Fixed blue_flag_pos = render_policy::resolve_flag_position(
        latest.flag_state_blue, latest.flag_carrier_blue, latest,
        kBlueBasePosition, dropped_flag_pos_blue_);
    draw_flag(red_flag_pos, Team::Red);
    draw_flag(blue_flag_pos, Team::Blue);

    // Players.
    for (uint8_t i = 0; i < latest.player_count; ++i) {
        const PlayerState& raw = latest.players[i];
        const bool is_local = raw.id == my_player_id;

        if (is_local) {
            // F1 off -> raw authoritative position; F1 on (default) ->
            // predicted (README §6.6's own framing: "F1 off shows what the
            // network actually feels like").
            const Vec2Fixed pos = render_policy::resolve_local_position(
                prediction_enabled_, prediction.local_state().position,
                raw.motion.position);
            draw_player_box(pos, raw.team, raw.alive, true);
            if (raw.alive) {
                draw_aim_indicator(pos, raw.aim_angle);
                draw_health_bar(pos, raw.health);
            }

            // F3: translucent outline at the authoritative position,
            // drawn beside the predicted one (README §6.6 - "the single
            // most useful thing you will build"). Drawn regardless of F1
            // state; when F1 is off the two naturally coincide.
            if (show_server_ghost_) {
                const float gx = to_px(raw.motion.position.x);
                const float gy = to_px(raw.motion.position.y);
                DrawRectangleLinesEx(
                    Rectangle{gx, gy, static_cast<float>(config::kPlayerSizePx),
                             static_cast<float>(config::kPlayerSizePx)},
                    2.0f, Fade(WHITE, 0.5f));
            }
        } else {
            // F2 off -> snap directly to the raw per-tick snapshot; F2 on
            // (default) -> smoothed via Interpolation.
            const PlayerState shown = render_policy::resolve_remote_state(
                interpolation_enabled_, raw, interpolation);
            draw_player_box(shown.motion.position, shown.team, shown.alive,
                           false);
            if (shown.alive) {
                draw_aim_indicator(shown.motion.position, shown.aim_angle);
                draw_health_bar(shown.motion.position, shown.health);
            }
        }
    }

    // Scoreboard/timer.
    char score_buf[64];
    std::snprintf(score_buf, sizeof(score_buf), "RED %u - %u BLUE   %u:%02u",
                 latest.score_red, latest.score_blue,
                 latest.seconds_remaining / 60, latest.seconds_remaining % 60);
    DrawText(score_buf, 12, 8, 20, RAYWHITE);

    // HUD (README §6.6): RTT, unacked input count, misprediction rate,
    // snapshot buffer depth, actual tick rate.
    char hud_buf[192];
    std::snprintf(
        hud_buf, sizeof(hud_buf),
        "RTT %.0fms  unacked %u  mispred %.1f%%  snapbuf %u  tick %.1fHz\n"
        "[F1] prediction: %s   [F2] interpolation: %s   [F3] ghost: %s",
        hud.rtt_ms, hud.unacked_input_count, hud.misprediction_rate * 100.0,
        hud.snapshot_buffer_depth, hud.actual_tick_rate_hz,
        prediction_enabled_ ? "ON" : "OFF",
        interpolation_enabled_ ? "ON" : "OFF",
        show_server_ghost_ ? "ON" : "OFF");
    DrawText(hud_buf, 12, config::kMapHeightPx - 44, 16, RAYWHITE);

    EndDrawing();
}

} // namespace ctf
