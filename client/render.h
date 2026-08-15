#pragma once

// raylib-backed rendering. Kept isolated so raylib types never leak outside
// this pair of files (README §4: bot reuses net_client + prediction, not
// render).

#include "game_types.h"

namespace ctf {

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(int width, int height, const char* title);
    void draw_frame(const WorldSnapshot& snap);
    bool should_close() const;
    void shutdown();

private:
    bool initialized_ = false;
};

} // namespace ctf
