#pragma once

namespace ecs {

struct PlayerTag {};

// Per-player controller state that isn't physics: the fixed-step accumulator and
// a jump press waiting for the next step.
struct PlayerControl
{
    double accumulator = 0;
    bool   jump_queued = false;
};

} // namespace ecs
