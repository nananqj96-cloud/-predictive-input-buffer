// FrameWave v3.4: alternate spam corrector.
//
// Alternate spamming = alternating between two inputs/fingers to reach very high click rates
// (wave spam, ship spam). The corrector detects it from the press timing, then takes over:
// it blocks the player's raw inputs and generates clicks at a user-set CPS, timed to the
// physics ticks so the rate stays consistent.
//
// State machine:
//   IDLE    — watch recent press timestamps in a rolling window
//   DETECT  — alternate-spam pattern held for `detectTime` → ACTIVE
//   ACTIVE  — block raw inputs, generate clicks at CPS
//   RELEASE — player stops → wait `releaseTime` → stop clicks, unblock, go to IDLE
//
// The detection logic is pure C++ (no Geode): it takes press/release events with tick
// timestamps and returns the next state. The injection (blocking + click generation) uses
// a callback, so the whole thing is testable with a standalone harness.
#pragma once

#include <functional>

namespace spamcorrect {

    class Corrector {
    public:
        static Corrector& get();

        // ---- settings (read from saved values by main.cpp) ----
        bool enabled = false;
        int cps = 30;              // clicks per second (no cap)
        float detectTime = 0.5f;   // seconds the spam pattern must hold → ACTIVE
        float releaseTime = 0.5f;  // seconds of no input → RELEASE
        int sensitivity = 2;       // 1=fast only, 2=medium, 3=eager

        // ---- state machine ----
        enum class State { Idle, Detect, Active, Release };
        State state = State::Idle;

        // ---- input watching ----
        // call on every raw input event (press or release) with the current game tick
        void onInput(bool down, int tick);

        // returns true when raw inputs should be blocked (state == Active)
        bool shouldBlock() const { return state == State::Active; }

        // per-frame tick: called from processCommands.
        // `dt` is the frame delta in seconds. `tickCount` is the game tick.
        // `inject(down)` sends a generated click to the game.
        void tick(float dt, int tickCount, std::function<void(bool)> inject);

        // reset on death, pause, level exit
        void reset();

        // panic key: force stop immediately
        void panic();

        // ---- testable pure logic ----
        // given a sequence of input events, returns the state transitions
        // (used by the standalone harness)
        struct Event { bool down; int tick; };
        State processEvent(Event const& ev);

    private:
        // detection state
        int lastPressTick = -1;     // tick of the last press
        int pressCount = 0;         // presses in the current detection window
        int windowStart = -1;       // tick where the current detection window started
        float detectTimer = 0.f;    // how long the spam pattern has been held
        float releaseTimer = 0.f;   // how long the player has been silent (in Release)
        float activeTimer = 0.f;    // elapsed time in Active

        // click generation
        float clickTimer = 0.f;     // time until the next click
        bool simulatedDown = false; // current state of the simulated click
        int lastInjectTick = -1;

        // helper: the minimum press rate (presses/sec) that counts as spam, given sensitivity
        int spamRateThreshold() const;
    };
}
