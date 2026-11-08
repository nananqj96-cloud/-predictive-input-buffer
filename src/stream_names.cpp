#include "stream_names.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_set>

namespace streamui::names {

    namespace {

        // gd-flavoured stems
        char const* GD[] = { "wave", "spike", "cube", "ship", "ufo", "ball", "robot", "spider", "swing",
            "demon", "orb", "portal", "level", "dual", "mini", "shipfly", "straightfly", "frame", "tick",
            "input", "click", "hold", "attempt", "practice", "hardcore", "extreme", "insane", "easy",
            "auto", "layout", "deco", "music", "beat", "drop", "nine", "twenty", "seven", "percent",
            "progress", "record", "attempt", "clip", "replay", "macro", "bot", "ghost", "pixel",
            "hitbox", "slope", "dash", "gravity", "reverse", "mirror", "speed", "portal", "spam" };

        char const* TWITCH[] = { "x", "the", "real", "not", "its", "im", "just", "only", "actually", "lowkey",
            "highkey", "slightly", "very", "kinda", "totally", "definitely", "probably", "daily", "casual",
            "pro", "noob", "grinder", "enjoyer", "watcher", "lurker", "regular", "viewer", "chatter",
            "clipper", "editor", "mod", "vip", "sub", "gifted", "fan", "hater", "expert", "veteran" };

        char const* NOUNS[] = { "guy", "man", "kid", "person", "human", "creature", "goblin", "frog", "cat",
            "dog", "bird", "fox", "panda", "goose", "duck", "moose", "bean", "toast", "sock", "chair",
            "table", "lamp", "potato", "noodle", "waffle", "pickle", "nugget", "brick", "rock", "cloud",
            "storm", "ghost", "wizard", "knight", "pirate", "cowboy", "ninja", "samurai", "monk", "bard",
            "king", "queen", "lord", "boss", "agent", "engineer", "gamer", "player", "runner", "clicker",
            "sweat", "tryhard", "legend", "myth", "gremlin", "menace", "voyager", "wanderer", "observer" };

        char const* ADJ[] = { "sleepy", "angry", "happy", "sad", "tiny", "massive", "silent", "loud", "fast",
            "slow", "cursed", "blessed", "unlucky", "lucky", "chill", "feral", "polite", "rude", "smooth",
            "crunchy", "spicy", "salty", "sweet", "sour", "frozen", "burning", "glowing", "shiny", "rusty",
            "ancient", "futuristic", "illegal", "legal", "certified", "suspicious", "confused", "confident",
            "nervous", "sleep deprived", "overcaffeinated", "underrated", "overrated", "retired", "aspiring" };

        char const* SUFFIX[] = { "GD", "TV", "YT", "owo", "uwu", "XD", "GG", "ez", "hq", "inc", "ltd",
            "prime", "ultra", "max", "zero", "one", "two", "nine", "97", "99", "100", "240", "360" };

        char const* SEP[] = { "", "_", "", "", ".", "", "-", "" };

        // words that must never appear in a generated name
        char const* BANNED[] = { "nazi", "hitler", "rape", "nigger", "nigga", "faggot", "retard", "kys",
            "pedo", "cp", "cum", "cock", "dick", "pussy", "whore", "slut", "bitch", "cunt", "twink",
            "isis", "terror", "schoolshoot" };

        // names that would look like impersonation or like the mod talking
        char const* RESERVED[] = { "framewave", "basu", "geode", "twitch", "twitchstaff", "admin",
            "moderator", "system", "streamer", "streamlab", "streamlabs", "nightbot", "streamelements",
            "moobot", "fossabot", "sery_bot", "wizebot", "botrix", "komodo" };

        bool containsWord(std::string const& lower, char const* w) {
            return lower.find(w) != std::string::npos;
        }

        std::string lower(std::string s) {
            for (auto& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }

    }  // namespace

    bool acceptable(std::string const& name) {
        if (name.size() < 2 || name.size() > 25) {
            return false;
        }
        for (unsigned char c : name) {
            bool ok = std::isalnum(c) || c == '_' || c == '-' || c == '.';
            if (!ok) {
                return false;  // no spaces, no control characters, nothing that breaks the layout
            }
        }
        std::string low = lower(name);
        for (auto* w : BANNED) {
            if (containsWord(low, w)) {
                return false;
            }
        }
        for (auto* w : RESERVED) {
            if (low.find(w) != std::string::npos) {
                return false;
            }
        }
        // all digits reads like a throwaway account
        if (std::all_of(low.begin(), low.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            return false;
        }
        return true;
    }

    std::vector<std::string> generate(unsigned seed, int count) {
        std::vector<std::string> out;
        out.reserve(static_cast<size_t>(std::max(count, 0)));
        std::unordered_set<std::string> seen;
        uint32_t st = seed | 1u;
        auto rnd = [&st]() {
            st = st * 1664525u + 1013904223u;
            return (st >> 8) & 0xffffff;
        };
        auto pick = [&rnd](char const* const* arr, size_t n) { return arr[rnd() % n]; };
        auto gd = [&]() { return pick(GD, std::size(GD)); };
        auto tw = [&]() { return pick(TWITCH, std::size(TWITCH)); };
        auto noun = [&]() { return pick(NOUNS, std::size(NOUNS)); };
        auto adj = [&]() { return pick(ADJ, std::size(ADJ)); };
        auto suf = [&]() { return pick(SUFFIX, std::size(SUFFIX)); };
        auto sep = [&]() { return pick(SEP, std::size(SEP)); };

        int guard = 0;
        while (static_cast<int>(out.size()) < count && guard < count * 40 + 200) {
            ++guard;
            std::string n;
            switch (rnd() % 10) {
                case 0: n = std::string(gd()) + sep() + noun(); break;
                case 1: n = std::string(adj()) + sep() + noun(); break;
                case 2: n = std::string(tw()) + gd() + sep() + noun(); break;
                case 3: n = std::string(gd()) + sep() + gd() + sep() + noun(); break;
                case 4: n = std::string(gd()) + std::to_string(1 + rnd() % 999); break;
                case 5: n = std::string(noun()) + sep() + suf(); break;
                case 6: n = std::string(adj()) + sep() + gd(); break;
                case 7: n = std::string(tw()) + noun() + std::to_string(rnd() % 99); break;
                case 8: n = std::string(gd()) + "_" + std::string(tw()) + std::to_string(rnd() % 9); break;
                default: n = std::string(tw()) + "_" + noun() + sep() + suf(); break;
            }
            // "sleep deprived" has a space in the adjective list: names can't, so strip it
            n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == ' '; }), n.end());
            if (n.size() > 25) {
                n.resize(25);
            }
            if (!acceptable(n) || seen.count(n)) {
                continue;
            }
            seen.insert(n);
            out.push_back(std::move(n));
        }
        // fill the remainder deterministically if the guard tripped
        int n = 0;
        while (static_cast<int>(out.size()) < count) {
            std::string fallback = std::string(gd()) + "_viewer" + std::to_string(++n);
            if (seen.insert(fallback).second) {
                out.push_back(fallback);
            }
        }
        return out;
    }

    unsigned colorFor(std::string const& name) {
        // Twitch's own name colours (the classic palette: red, blue, green, gold, purple, pink...)
        static unsigned const PALETTE[] = { 0xFF0000, 0x0000FF, 0x00FF00, 0xB22222, 0xFF7F50, 0x9ACD32, 0xFF4500,
            0x2E8B57, 0xDAA520, 0xD2691E, 0x5F9EA0, 0x1E90FF, 0xFF69B4, 0x8A2BE2, 0x00FF7F, 0x1B9E77,
            0x66C2A5, 0xE6AB02, 0xA6761D, 0x7570B3, 0xFF6B6B, 0x4ECDC4, 0x45B7D1, 0xF7B32B, 0x7B68EE,
            0xE75480, 0x00CED1, 0xADFF2F, 0xFFA07A, 0x9370DB };
        uint32_t h = 2166136261u;
        for (unsigned char c : name) {
            h ^= c;
            h *= 16777619u;
        }
        return PALETTE[h % std::size(PALETTE)];
    }

}  // namespace streamui::names
