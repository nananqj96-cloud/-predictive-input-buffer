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
                // add to rolling window (1 second = 240 ticks at 240 TPS)
                pressTicks.push_back(tick);
                // remove presses older than 1 second
                int cutoff = tick - 240;
                while (!pressTicks.empty() && pressTicks.front() <= cutoff) {
                    pressTicks.erase(pressTicks.begin());
                }
                // update current CPS
                currentCps = static_cast<int>(pressTicks.size());
                // check if CPS exceeds threshold
                if (currentCps >= spamRateThreshold()) {
                    if (state == State::Idle) {
                        state = State::Detect;
                        detectTimer = 0.f;
                    }
                } else {
                    // pattern broken — go back to Idle if well below threshold
                    if (state == State::Detect && currentCps < spamRateThreshold() / 2) {
                        state = State::Idle;
                        detectTimer = 0.f;
                        pressTicks.clear();
                    }
                }
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
                state = State::Active;
                releaseTimer = 0.f;
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
                    pressTicks.clear();
                    lastPressTick = -1;
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
                // We track this via releaseTimer, which is reset on each press.
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
                    pressTicks.clear();
                    lastPressTick = -1;
                    currentCps = 0;
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
        pressTicks.clear();
        currentCps = 0;
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
        pressTicks.clear();
        currentCps = 0;
    }

    // ---- testable pure logic ----
    Corrector::State Corrector::processEvent(Event const& ev) {
        // snapshot the state before processing
        auto prev = state;
        onInput(ev.down, ev.tick);
        return prev;
    }

}  // namespace spamcorrect
