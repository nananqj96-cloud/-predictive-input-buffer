// FrameWave v3.3: the username pool.
//
// A Twitch chat of 500 people isn't 500 hand-typed names, but it also isn't "user1234": names are
// built from the parts people actually use (gd words, twitch words, numbers, separators) and then
// screened so nothing crude or impersonating slips in.
#pragma once

#include <string>
#include <vector>

namespace streamui::names {

    // deterministic: the same seed always gives the same pool
    std::vector<std::string> generate(unsigned seed, int count);

    // true if the name is safe to show (used for the pool and for names the user types)
    bool acceptable(std::string const& name);

    // 0xRRGGBB; the Twitch palette, picked from the name so a bot keeps its colour
    unsigned colorFor(std::string const& name);
}
