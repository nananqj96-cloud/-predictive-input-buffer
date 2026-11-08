// FrameWave v3.4: alternate spam corrector implementation.
// See spam_correct.hpp for the state machine description.
#include "spam_correct.hpp"

#include <algorithm>

namespace spamcorrect {

    namespace {
        Corrector* instance = nullptr;
    }

    Corrector& Corrector::get() {
        if (!instance) {
            instance = new Corrector();
        }
        return *instance;
    }

    int Corrector::spamRateThreshold() const {
        // minimum presses per second that counts as spam, given sensitivity
        switch (sensitivity) {
            case 1: return 20;   // very fast only
            case 3: return 8;    // eager
            default: return 12;  // medium
        }
    }

    void Corrector::onInput(bool down, int tick) {
        if (!enabled) {
            return;
        }
        if (down) {
            // a press
            if (state == State::Idle || state == State::Detect) {
                if (lastPressTick >= 0) {
                    int gap = tick - lastPressTick;
                    // threshold in ticks at 240 TPS: 1000/threshold_cps * 240 / 1000
                    int threshold = 240 / spamRateThreshold();
                    if (gap <= threshold) {
                        // fast press — potential spam
                        if (state == State::Idle) {
                            state = State::Detect;
                            detectTimer = 0.f;
                        }
                    } else {
                        // pattern broken
                        if (state == State::Detect) {
                            state = State::Idle;
                            detectTimer = 0.f;
                        }
                    }
                }
                lastPressTick = tick;
            } else if (state == State::Active) {
                // player is still pressing — keep generating clicks, reset release timer
                releaseTimer = 0.f;
            } else if (state == State::Release) {
                // player pressed again — go back to Active
                state = State::Active;
                releaseTimer = 0.f;
            }
        } else {
            // a release
            if (state == State::Release) {
                // player is still releasing — keep waiting
                releaseTimer = 0.f;
                state = State::Active;
            }
        }
    }

    void Corrector::tick(float dt, int tickCount, std::function<void(bool)> inject) {
        if (!enabled) {
            return;
        }
        switch (state) {
            case State::Idle:
                break;

            case State::Detect:
                detectTimer += dt;
                if (detectTimer >= detectTime) {
                    state = State::Active;
                    activeTimer = 0.f;
                    clickTimer = 0.f;
                    simulatedDown = false;
                    lastPressTick = -1;  // don't track presses for release detection yet
                }
                break;

            case State::Active:
                activeTimer += dt;
                // generate clicks at CPS
                // each click = press + release, so events per second = 2 * CPS
                {
                    float timePerEvent = 1.0f / std::max(1, 2 * cps);
                    clickTimer += dt;
                    while (clickTimer >= timePerEvent) {
                        clickTimer -= timePerEvent;
                        simulatedDown = !simulatedDown;
                        inject(simulatedDown);
                    }
                }
                // check if player has stopped inputting
                // (no press events for releaseTime seconds)
                // We track this via releaseTimer, which is reset on each input.
                // But in Active, inputs are blocked — we still see them via onInput.
                // If no input for releaseTime, go to Release.
                releaseTimer += dt;
                if (releaseTimer >= releaseTime) {
                    state = State::Release;
                    releaseTimer = 0.f;
                }
                break;

            case State::Release:
                releaseTimer += dt;
                if (releaseTimer >= releaseTime) {
                    // stop clicks, unblock, go to Idle
                    if (simulatedDown) {
                        inject(false);
                        simulatedDown = false;
                    }
                    state = State::Idle;
                    releaseTimer = 0.f;
                    clickTimer = 0.f;
                    lastPressTick = -1;
                }
                break;
        }
    }

    void Corrector::reset() {
        state = State::Idle;
        detectTimer = 0.f;
        releaseTimer = 0.f;
        activeTimer = 0.f;
        clickTimer = 0.f;
        simulatedDown = false;
        lastPressTick = -1;
    }

    void Corrector::panic() {
        // force stop immediately: release any held click, unblock
        state = State::Idle;
        detectTimer = 0.f;
        releaseTimer = 0.f;
        activeTimer = 0.f;
        clickTimer = 0.f;
        simulatedDown = false;
        lastPressTick = -1;
    }

    // ---- testable pure logic ----
    Corrector::State Corrector::processEvent(Event const& ev) {
        // snapshot the state before processing
        auto prev = state;
        onInput(ev.down, ev.tick);
        return prev;
    }

}  // namespace spamcorrect
