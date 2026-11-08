// FrameWave v3.3: the chat and donation text database.
//
// Everything is generated once at startup from hand-written fragment lists crossed with short
// tails, then de-duplicated. That keeps the source readable while producing several thousand
// distinct lines. Lines are grouped by category so the overlay can pick a category first (weighted
// by how hyped the run is) and then any line inside it, in constant time.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace stream::text {

    // ---- chat categories ----
    enum ChatCat : int {
        C_NORMAL = 0,   // people talking to each other
        C_REACT,        // short reactions to what's on screen
        C_SPAM,         // copypasta, caps, stretched letters
        C_EMOTE,        // emotes and twitch emotes
        C_PREDICT,      // guesses about the run
        C_HYPE,         // 50-85%
        C_PANIC,        // 85-100%
        C_CELEBRATE,    // the level is done
        C_DEATH,        // just died
        C_PB,           // new personal best
        C_COUNT
    };

    // ---- donation message categories ----
    enum GiftCat : int {
        G_FUN = 0,      // funny questions
        G_PUSH,         // encouragement
        G_PREDICT,      // predictions
        G_JOKE,         // jokes about earlier deaths
        G_HIGH,         // reactions to a high percentage
        G_GD,           // geometry dash references
        G_STREAM,       // about the stream / the person
        G_COUNT
    };

    struct Table {
        std::vector<std::string> lines;              // grouped by category
        std::array<int, 16> bounds{};                // category c owns [bounds[c], bounds[c + 1])
        int size = 0;

        std::string const& at(int i) const { return lines[static_cast<size_t>(i)]; }
        int count(int c) const { return bounds[static_cast<size_t>(c) + 1] - bounds[static_cast<size_t>(c)]; }
        int first(int c) const { return bounds[static_cast<size_t>(c)]; }
    };

    Table const& chat();
    Table const& gifts();

    // donation lines that need the current run: "$P" becomes the percentage, "$A" the attempt count
    std::vector<std::string> const& giftTemplates();
}
