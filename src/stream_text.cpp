#include "stream_text.hpp"

#include <algorithm>
#include <unordered_set>
#include <cstdint>

namespace streamui::text {

    namespace {

        using Lines = std::vector<std::string>;

        // every category gets its own bucket first, then they are concatenated in order, so a
        // category is just a range and picking one is O(1)
        struct Buckets {
            std::array<Lines, 16> b;
        };

        void put(Lines& v, std::string const& s) {
            if (!s.empty()) {
                v.push_back(s);
            }
        }

        void putAll(Lines& v, std::initializer_list<char const*> list) {
            for (auto* s : list) {
                put(v, s);
            }
        }

        // phrases x tails
        void cross(Lines& v, std::initializer_list<char const*> heads, std::initializer_list<char const*> tails) {
            for (auto* h : heads) {
                for (auto* t : tails) {
                    put(v, std::string(h) + t);
                }
            }
        }

        // "that last jump was criminal", "the wave part hurt to watch"
        void cross3(Lines& v, std::initializer_list<char const*> parts, char const* mid, std::initializer_list<char const*> tails) {
            for (auto* p : parts) {
                for (auto* t : tails) {
                    put(v, std::string(p) + mid + t);
                }
            }
        }

        // same two, for arrays declared with `static char const* X[] = {...}` (an array can't turn
        // into an initializer_list implicitly)
        template <size_t N, size_t M>
        void cross(Lines& v, char const* (&heads)[N], char const* (&tails)[M]) {
            for (auto* h : heads) {
                for (auto* t : tails) {
                    put(v, std::string(h) + t);
                }
            }
        }

        template <size_t N, size_t M>
        void cross3(Lines& v, char const* (&parts)[N], char const* mid, char const* (&tails)[M]) {
            for (auto* p : parts) {
                for (auto* t : tails) {
                    put(v, std::string(p) + mid + t);
                }
            }
        }

        // Chat says the same thing with a different tail all the time: "that hurt", "that hurt bro",
        // "that hurt lol". Giving every line a few extra forms makes the bag deep enough that the
        // chat never visibly loops. Deterministic (no RNG), and only the original lines are expanded,
        // so a line can never collect two tails.
        void expandFrom(Lines& v, size_t baseCount, std::initializer_list<char const*> tails, int perLine, size_t salt) {
            std::vector<std::string> tl;
            tl.reserve(tails.size());
            for (auto* t : tails) {
                tl.emplace_back(t);
            }
            if (tl.empty() || perLine <= 0 || baseCount > v.size()) {
                return;
            }
            std::vector<std::string> added;
            added.reserve(baseCount * static_cast<size_t>(perLine));
            for (size_t i = 0; i < baseCount; ++i) {
                std::string const base = v[i];
                bool question = !base.empty() && base.back() == '?';
                bool emoji = base.find("\xF0\x9F") != std::string::npos || base.find("\xE2\x9D") != std::string::npos ||
                    base.find("\xF0\x9F\x92") != std::string::npos;
                for (int k = 0; k < perLine; ++k) {
                    auto const& t = tl[(i * 7 + static_cast<size_t>(k) * 5 + salt) % tl.size()];
                    if (t.empty()) {
                        continue;
                    }
                    if (question && t.find('?') == std::string::npos && k > 0) {
                        continue;  // a question keeps at most one tag
                    }
                    if (emoji && t.find("\xF0\x9F") != std::string::npos) {
                        continue;  // no double emoji trains
                    }
                    // don't repeat the last word the line already has ("chat is tweaking chat")
                    if (!t.empty() && t.front() == ' ') {
                        size_t sp = t.find(' ', 1);
                        std::string word = t.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
                        if (base.size() >= word.size() && base.compare(base.size() - word.size(), word.size(), word) == 0) {
                            continue;
                        }
                    }
                    added.push_back(base + t);
                }
            }
            v.insert(v.end(), added.begin(), added.end());
        }

        // FrameWave's fonts are bitmap atlases: they have no emoji glyphs. The database keeps the
        // emoji (they read naturally in the source and the donation TTS still speaks the words), but
        // every line that reaches the overlay goes through here, which turns emoji into the Twitch
        // emote people type for the same thing. "97 💀" becomes "97 KEKW", which is what chat looks
        // like in the screenshots anyway (colourful names, emote words, no unicode glyphs).
        // collapse the "word  word" that appears when a fragment and its tail both bring a space
        std::string squish(std::string const& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c == ' ' && !out.empty() && out.back() == ' ') {
                    continue;
                }
                out.push_back(c);
            }
            while (!out.empty() && out.back() == ' ') {
                out.pop_back();
            }
            return out;
        }

        char const* emoteFor(uint32_t cp) {
            switch (cp) {
                case 0x1F480: return "KEKW";     // skull
                case 0x1F525: return "catJAM";   // fire
                case 0x1F62D: return "LULW";     // crying
                case 0x1F64F: return "Prayge";   // praying
                case 0x1FAE1: return "o7";       // salute
                case 0x1F440: return "monkaS";   // eyes
                case 0x1F4AF: return "EZ";       // hundred
                case 0x1F4C8: return "Pog";      // chart up
                case 0x1F680: return "HYPERS";   // rocket
                case 0x1F389:                     // party
                case 0x1F973: return "PogChamp";
                case 0x1F631:                     // screaming
                case 0x1F633: return "monkaW";
                case 0x1F92F: return "GIGACHAD";  // exploding head
                case 0x1FAE8: return "monkaE";    // shaking
                case 0x1FAE0: return "Copium";    // melting
                case 0x2620: return "Sadge";      // skull and crossbones
                case 0x1F494: return "Sadge";     // broken heart
                case 0x1F972: return "Sadge";     // holding back tears
                case 0x1F635: return "D:";        // dizzy
                case 0x1F62E:                     // exhale
                case 0x1F9D8: return "Prayge";    // meditation
                case 0x1F56F: return "Prayge";    // candle
                case 0x1F340: return "Prayge";    // clover
                case 0x1FA84: return "Pog";       // magic wand
                case 0x1FAF5: return "pepePoint"; // pointing
                case 0x1F451: return "PepeRegal"; // crown
                case 0x1F985: return "EAGLE";     // eagle
                case 0x1F410: return "GOAT";      // goat
                case 0x1F3AC: return "cinema";    // clapper
                case 0x2764: return "<3";         // heart
                default: return "KEKW";
            }
        }

        // utf8 pass: emoji out, emote words in, runs of spaces collapsed, trailing space trimmed
        std::string sanitize(std::string const& in) {
            std::string out;
            out.reserve(in.size() + 8);
            for (size_t i = 0; i < in.size();) {
                unsigned char c = static_cast<unsigned char>(in[i]);
                if (c < 0x80) {
                    out.push_back(static_cast<char>(c));
                    ++i;
                    continue;
                }
                // decode one UTF-8 code point
                int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
                uint32_t cp = extra == 3 ? (c & 0x07u) : extra == 2 ? (c & 0x0Fu) : extra == 1 ? (c & 0x1Fu) : 0;
                i += 1;
                for (int k = 0; k < extra && i < in.size(); ++k, ++i) {
                    cp = (cp << 6) | (static_cast<unsigned char>(in[i]) & 0x3Fu);
                }
                // zero width joiner / variation selector / skin tone: the previous emote already
                // stands for the whole thing, so these are dropped
                if (cp == 0x200D || cp == 0xFE0F || (cp >= 0x1F3FB && cp <= 0x1F3FF)) {
                    continue;
                }
                if (cp == 0x2014 || cp == 0x2013) {  // en/em dash
                    out.push_back('-');
                    continue;
                }
                if (cp == 0x00A0) {
                    out.push_back(' ');
                    continue;
                }
                std::string word = emoteFor(cp);
                if (!out.empty() && out.back() != ' ') {
                    out.push_back(' ');
                }
                out += word;
                out.push_back(' ');
            }
            // collapse spaces
            std::string squashed;
            squashed.reserve(out.size());
            for (char ch : out) {
                if (ch == ' ' && !squashed.empty() && squashed.back() == ' ') {
                    continue;
                }
                squashed.push_back(ch);
            }
            while (!squashed.empty() && squashed.back() == ' ') {
                squashed.pop_back();
            }
            if (squashed.empty()) {
                squashed = "KEKW";
            }
            return squashed;
        }

        // ---------------- normal conversation ----------------
        void buildNormal(Lines& v) {
            putAll(v, {
                "anyone else lagging", "chat is moving fast today", "how long has he been trying this",
                "what level is this", "level name?", "how many attempts", "what is the level id",
                "how long is this level", "is this the hard version", "what difficulty is this",
                "bro has been on this for 3 hours", "first time here, what's happening",
                "just got here, how far is he", "did he beat it while i was gone",
                "i swear he does the same thing every time", "who else is supposed to be working right now",
                "3am and i'm watching gd, life is good", "this stream is my therapy",
                "the music in this level is actually insane", "what song is this",
                "i can't hear the song over the clicking", "he types faster than i read",
                "who is the guy in the corner", "why is there a heart monitor on screen",
                "is that his real heart rate", "bro is wearing a heart monitor now",
                "the production value keeps going up", "we went from 12 viewers to this",
                "chat remembers when this was a small stream", "OG viewers rise up",
                "i've been watching since the first stream", "just subbed",
                "does he read chat", "he never reads chat", "he reads chat once a week",
                "how does he not get dizzy", "my hands hurt just watching",
                "i tried this level for 10 minutes and gave up", "you make this look easy",
                "actually how are you not tilting", "the consistency is unreal",
                "how do you stay calm", "the practice mode grind is real",
                "i learned so much from watching you play", "this is why i still watch gd",
                "gd community is so different now", "the old levels were harder, change my mind",
                "what's your hardest", "what is your hardest demon",
                "have you beaten bloodbath", "when's the next video",
                "youtube upload when", "is there a vod",
                "what keyboard do you use", "mechanical keyboard asmr",
                "i can hear every single click", "that clicking is satisfying",
                "who's the mod in chat", "mods are asleep",
                "hello everyone", "hi chat", "first", "second", "new here",
                "the timer on the left is going up", "how many deaths is that",
                "attempt counter is at a scary number", "this attempt feels different",
                "the level looks pretty", "the deco on this level goes hard",
                "this part looks impossible", "that part is definitely impossible",
                "the bar at the bottom is so satisfying", "that progress bar is crawling",
                "i came from the youtube clip", "the clip brought me here",
                "that clip of the 99 death is why i'm here",
            });

            cross(v, { "anyone know", "quick question,", "question for chat", "someone tell me" },
                { " how the ship works", " how to get better at wave", " what the best level to start with is",
                    " how long this level is", " how many attempts that is", " what song this is",
                    " what that song is called", " which mod does that", " how many attempts he's on" });

            cross(v, { "i'm", "bro i'm", "honestly i'm", "not gonna lie i'm" },
                { " so invested in this run", " nervous and i'm not even playing", " sweating watching this",
                    " vibing in chat", " here for the whole stream", " actually shaking",
                    " eating while watching this", " supposed to be asleep" });

            cross(v, { "this is", "this level is", "that was", "this attempt is" },
                { " actually insane", " so satisfying to watch", " a movie", " not normal",
                    " the best content on twitch", " harder than it looks", " giving me anxiety" });

            cross(v, { "how", "wait how", "explain how" },
                { " did he survive that", " is that possible", " did you do that",
                    " do people beat this", " is that fair", " does that work" });

            cross(v, { "imagine", "imagine if", "picture this:" },
                { " dying at 99", " dying to the first spike", " beating it first try",
                    " choking at the end", " doing this on mobile", " playing this with a trackpad",
                    " no deaths on this run" });
        }

        // ---------------- short reactions ----------------
        void buildReact(Lines& v) {
            putAll(v, {
                "wtf was that", "WHAT", "no way", "bro", "BRO", "yo", "HUH",
                "did that just happen", "ok", "okay", "sure", "cool cool cool",
                "oof", "OOF", "yikes", "bruh", "BRUH", "pain", "PAIN",
                "that was close", "almost", "SO close", "clutch", "CLUTCH",
                "insane", "actually insane", "that was insane",
                "how", "HOW", "what", "WHAT???",
                "look at that", "look at the timer", "look at the percentage",
                "he's so locked in", "the focus is unreal", "not a single blink",
                "the hands", "those hands", "hand cam when",
                "ok that was actually clean", "clean", "CLEAN",
                "that was not clean", "that was scuffed", "scuffed but it worked",
                "my heart", "my heart can't take this", "my hands are sweating",
                "i'm not even playing and i'm stressed", "the tension in this chat",
                "hold on", "wait", "WAIT", "wait wait wait",
                "nope", "NOPE", "not today", "here we go", "HERE WE GO",
                "this is it", "this is the one", "this is THE attempt",
                "called it", "i called it", "i jinxed it",
                "don't jinx it", "chat don't jinx", "whoever jinxed it",
                "he hesitated", "that was hesitation", "don't hesitate",
                "that recovery though", "the recovery", "recovered",
                "he saved it", "SAVED", "clutch save",
                "the timing on that", "perfect timing", "inhuman timing",
                "bro read that", "he read the level", "reading like a book",
                "oh no", "OH NO", "oh no no no", "please no",
                "nice", "NICE", "nice one", "clean one",
                "why would you do that", "that was risky", "risky click",
                "my palms", "palms are sweaty", "knees weak",
                "that's the run", "that's it", "that's the attempt",
                "i felt that click", "the click", "THE CLICK",
            });

            cross(v, { "he's", "bro is", "this guy is", "chat is" },
                { " actually cooking", " not human", " built different", " on another level",
                    " locked in", " so calm", " losing it", " tweaking", " goated",
                    " a machine", " insane today", " not missing today" });

            cross(v, { "that", "this", "everything" },
                { " hurts to watch", " is so clean", " looks impossible", " was too close",
                    " is cinema", " is peak gd", " felt personal" });
        }

        // ---------------- spam ----------------
        void buildSpam(Lines& v) {
            putAll(v, {
                "GO", "GO GO", "GO GO GO", "GOOO", "GOOOO", "GOOOOO", "GOOOOOO",
                "MUGA", "MUGAAAA", "MUGAAAAAAAA",
                "LOCK IN", "LOCK IN CHAT", "CHAT LOCK IN", "LOCK IN LOCK IN",
                "WE ARE SO BACK", "LET'S GO", "LETS GO", "LET'S GO CHAT",
                "ABSOLUTE CINEMA", "CINEMA", "this is cinema",
                "CLICK", "CLICK IT", "CLICK BRO", "PRESS IT",
                "DON'T DIE", "DONT DIE", "DO NOT DIE", "DON'T YOU DARE",
                "PLEASE", "PLEASEEEEE", "PLEASE BRO", "PLEASEEEEEE",
                "ONE MORE", "ONE MORE CLICK", "ONE CLICK", "JUST ONE MORE",
                "KEEP GOING", "KEEP GOING BRO", "DON'T STOP", "DONT STOP",
                "FOCUS", "FOCUS BRO", "FOCUS CHAT",
                "BREATHE", "BREATHE BRO", "DON'T BLINK",
                "HE'S DOING IT", "HE'S ACTUALLY DOING IT", "ACTUALLY GOING",
                "NO WAY", "NO WAYYYY", "NO WAYYYYY",
                "HOLY SHIT", "HOLY", "HOLY SHIT BRO",
                "CHAT", "CHAT CHAT CHAT", "CHAT LOOK",
                "IT'S HAPPENING", "ITS HAPPENING",
                "WE'RE SO CLOSE", "SO CLOSE", "CLOSE CLOSE CLOSE",
                "E", "EZ", "EZ CLAP", "W", "WWWW", "----------------", "LLLLLLLLLLLL",
                "????????????????", "!!!!!!!!!!!!!!!", "1111111", "7777777",
            });

            // stretched letters: GOOOOOO..., LET'S GOOOO, NO WAYYYYY, MUGAAAAA
            for (int n = 3; n <= 14; ++n) {
                put(v, "GO" + std::string(static_cast<size_t>(n), 'O'));
            }
            for (int n = 2; n <= 7; ++n) {
                put(v, "LET'S G" + std::string(static_cast<size_t>(n) + 1, 'O'));
                put(v, "NO WAY" + std::string(static_cast<size_t>(n), 'Y'));
                put(v, "MUG" + std::string(static_cast<size_t>(n) + 3, 'A'));
            }
            for (int n = 2; n <= 6; ++n) {
                put(v, "LOCK IN" + std::string(static_cast<size_t>(n), '!'));
                put(v, "HOLY" + std::string(static_cast<size_t>(n), '!'));
                put(v, "CHAT" + std::string(static_cast<size_t>(n), '!'));
                put(v, "W" + std::string(static_cast<size_t>(n) + 1, 'W'));
                put(v, std::string(static_cast<size_t>(n), 'A'));
            }
            // THE GO WALL. In a GD chat this is the single most typed thing there is: at a new
            // best, at the last 5%, at a completion, the feed stops being sentences and becomes
            // GOOOOOO with the O stretched further every line. buildGo() is a category of its own
            // so the overlay can call it for those moments instead of hoping a weighted roll
            // lands on it (GO used to be ~1.6% of all lines, which never reads as spam).
            //
            // copypasta style repeats
            put(v, "97 again 97 again 97 again");
            put(v, "he dies at 97 he dies at 97 he dies at 97");
            put(v, "GO GO GO GO GO GO GO GO GO");
            put(v, "LOCK IN LOCK IN LOCK IN LOCK IN");
            put(v, "MUGA MUGA MUGA MUGA MUGA");
            put(v, "F F F F F F F F");
            put(v, "no no no no no no no");
            put(v, "please please please please please");
            put(v, "one more one more one more one more");
        }

        // ---------------- the GO wall ----------------
        // Every length from GO to the point where the line is all O, plus the phrases that get
        // the same treatment. The overlay picks these as a run of several lines in a row, so the
        // feed reads like a wall rather than one stray GO.
        void buildGo(Lines& v) {
            putAll(v, { "GO", "GO GO", "GO GO GO", "GO GO GO GO", "GO GO GO GO GO",
                "GO GO GO GO GO GO GO", "GO GO GO GO GO GO GO GO GO" });
            for (int n = 2; n <= 22; ++n) {
                put(v, "G" + std::string(static_cast<size_t>(n), 'O'));
            }
            for (int n = 1; n <= 9; ++n) {
                put(v, "LET'S G" + std::string(static_cast<size_t>(n) + 1, 'O'));
                put(v, "LETS G" + std::string(static_cast<size_t>(n) + 1, 'O'));
                put(v, "LET'S GO" + std::string(static_cast<size_t>(n), '!'));
            }
            putAll(v, { "GG", "GG WP", "GGs", "W", "WWWW", "WWWWW", "WWWWWWWW",
                "CLIP IT", "CLIP THAT", "CLIPPED", "SAVE THE CLIP", "SOMEBODY CLIP IT",
                "GG BRO", "GG CHAT", "EZ", "EZ CLAP", "EZZZZ", "NO WAY", "NO WAYYYY",
                "HOLY", "HOLY SHIT", "HOLY!!!!", "LETSGOOOOO", "LETS FUCKING GO",
                "WE DID IT", "IT'S DONE", "DONE", "FINALLY", "FINALLY BRO",
                "HE DID IT", "HE ACTUALLY DID IT", "MASSIVE", "HUGE", "INSANE" });
            for (int n = 2; n <= 8; ++n) {
                put(v, "GG" + std::string(static_cast<size_t>(n), 'G'));
                put(v, "EZ" + std::string(static_cast<size_t>(n), 'Z'));
                put(v, "W" + std::string(static_cast<size_t>(n) + 1, 'W'));
            }
            putAll(v, { "GO GO GO GO GO GO GO GO GO GO GO", "GOOOOOOOOOOOOOOOOOOOO",
                "LET'S GOOOOOOOOOOOOOOO", "CHAT WE'RE GOING", "WE'RE GOING", "WE GO",
                "GO FOR IT", "SEND IT", "FULL SEND", "NOW NOW NOW", "DO IT NOW",
                "DON'T STOP NOW", "KEEP IT GOING", "NEVER STOP" });
        }

        // ---------------- emotes ----------------
        void buildEmotes(Lines& v) {
            putAll(v, {
                "💀", "🔥", "😭", "😭😭", "💀💀", "💀💀💀", "🔥🔥", "🔥🔥🔥",
                "🙏", "🙏🙏", "🫡", "🫡🫡", "👀", "👀👀", "😤", "😤😤", "🤝", "🥶", "🧠", "⚡",
                "💯", "📈", "🚀", "🎉", "🥳", "😱", "😳", "🤯", "🫨", "🫠", "☠️", "💔", "🥲", "😵",
                "😮‍💨", "🧘", "🕯️", "🕯️🕯️", "🍀", "🍀🍀", "🪄", "🫵", "👑", "🦅", "🐐", "🎬",
                "KEKW", "KEKW KEKW", "Pog", "POG", "PogChamp", "PogU", "monkaS", "monkaW",
                "Sadge", "SadgeDespair", "PepeHands", "Copium", "COPIUM", "HOPIUM", "catJAM",
                "HYPERCLAP", "WICKED", "WidePeepoHappy", "peepoHappy", "Prayge", "PRAYGE",
                "Clap", "Clap Clap Clap", "GIGACHAD", "gigachad", "BASED", "Bedge", "Jebaited",
                "xqcL", "OMEGALUL", "LULW", "LUL", "TRUEING", "TRUE", "Susge", "Sussy",
                "pepeD", "FeelsGoodMan", "FeelsBadMan", "PauseChamp", "NOW HMM", "HUH",
                "5Head", "3Head", "catKISS", "ppHop", "peepoCheer", "peepoClap",
                "MICROWAVE", "Poggers", "poggers", "KEKWait", "YEP", "COPIUM COPIUM",
            });

            char const* A[] = { "💀", "🔥", "😭", "🙏", "👀", "🤯", "🫡", "💯" };
            char const* B[] = { "💀", "🔥", "😭", "🫡", "🤯" };
            for (auto* a : A) {
                for (auto* b : B) {
                    put(v, std::string(a) + " " + b);
                }
            }
        }

        // ---------------- predictions ----------------
        void buildPredict(Lines& v) {
            putAll(v, {
                "he's dying at 97 i'm calling it", "death at 97 incoming",
                "calling death at 96", "death at the last jump, guaranteed",
                "he dies at 95+ i've seen this movie", "this run ends at 97, calling it now",
                "predicting a death at 88", "i bet he dies at the hardest part",
                "he's not making it past 90", "no shot he passes 95 today",
                "first try after the break, trust", "he beats it tonight, i feel it",
                "next attempt is the one", "he'll beat it in 20 more attempts",
                "he beats it before the stream ends", "he's beating it in the next hour",
                "he chokes at 99, i've seen it twice", "death incoming in 10 seconds",
                "he survives this", "he's dead in 3", "he's timing out",
                "betting my whole inventory he dies at 97", "everyone say 97",
                "chat say 97 for good luck", "type 97 in chat", "97 energy only",
                "the 97 curse is real", "he dies exactly where the chat says",
                "my prediction: 94", "my prediction: 98", "my prediction: 91",
                "if he passes this he's beating it", "the hardest part is next",
                "easiest part of the level now", "this is the easy bit, don't relax",
                "he's going to die at the part he always dies at",
                "the pre-drop is where it ends", "watch, he dies at the dual",
                "he dies to the orb, watch", "the last spike gets him",
                "he's been at 97 for two hours", "97 is his wall",
                "the chat called it last time", "i have a perfect record on predictions today",
                "statistically he should beat this soon",
                "law of averages says this is the run",
                "he's due for a win", "he's overdue",
            });

            cross(v, { "prediction:", "my call:", "betting", "i bet", "guarantee", "calling it:" },
                { " 97", " 98", " 99 and a choke", " a death at the next spike",
                    " he beats it this hour", " he dies at the dual", " death at the last jump",
                    " he's not getting past 95", " he beats it tonight", " a triple death at 97" });

            cross(v, { "calling", "i'm calling", "officially calling" },
                { " death at 97", " a choke at 99", " the win this attempt",
                    " death at the last spike", " a pb right now" });
        }

        // ---------------- hype ----------------
        void buildHype(Lines& v) {
            putAll(v, {
                "oh he's cooking", "BRO IS COOKING", "bro is cooking fr",
                "he's locked in", "LOCKED IN", "actually locked in",
                "don't say anything chat", "chat be quiet", "everyone shut up he's focused",
                "no one say a word", "hold your breath chat",
                "this feels different", "this run feels different", "different energy today",
                "we are so back", "WE ARE SO BACK", "he's back",
                "the warmup is over", "that was the warmup",
                "he's hitting every click", "not a single wasted input",
                "he's not missing", "he hasn't missed once", "flawless so far",
                "the consistency right now", "he's in the zone", "flow state",
                "he's in a flow state chat", "nobody talk",
                "this is the run", "i can feel it", "chat can feel it",
                "the way he clicked that", "did you see that click",
                "that was frame perfect", "frame perfect", "pixel perfect",
                "he's playing like it's practice mode",
                "he's so calm it's scary", "how is he this calm",
                "the run is clean", "clean run so far", "this is the cleanest run today",
                "don't touch the keyboard chat", "hands off chat",
                "he's pacing himself", "pacing is perfect",
                "that was the hard part and he's through it",
                "hard part done", "worst part is behind him",
                "nothing can stop this run", "this run is blessed",
                "chat we're witnessing something", "we are witnessing it",
                "this is the best he's looked all day",
            });

            cross(v, { "he's", "bro is", "chat," },
                { " actually doing it", " not missing", " in the zone", " cooking right now",
                    " locked in like never before", " built for this level",
                    " about to make history", " making it look easy", " different today",
                    " in his bag", " on a different plane" });

            cross(v, { "keep", "just keep", "don't stop", "whatever you do, keep" },
                { " going", " it steady", " clicking", " breathing", " the same pace",
                    " your eyes on the screen" });
        }

        // ---------------- panic ----------------
        void buildPanic(Lines& v) {
            putAll(v, {
                "NO WAYYYYY", "NO WAY BRO", "no way he's doing it",
                "ACTUALLY GOING", "he's ACTUALLY going", "IT'S ACTUALLY HAPPENING",
                "HOLY SHIT BRO GO", "HOLY SHIT HE'S DOING IT",
                "PLEASEEEEE", "PLEASE BRO PLEASE", "PLEASE DON'T DIE",
                "ONE MORE CLICK", "ONE MORE", "JUST ONE MORE CLICK",
                "CHAT LOCK IN", "LOCK IN CHAT LOCK IN", "EVERYONE LOCK IN",
                "GG ATTEMPT", "GG ATTEMPT HOLY", "gg attempt",
                "MY HEART", "MY HEART IS RACING", "my hands are shaking",
                "I'M SHAKING", "i can't watch", "i'm watching through my fingers",
                "DON'T CHOKE", "DO NOT CHOKE", "PLEASE DON'T CHOKE",
                "HOLD IT TOGETHER", "HOLD", "HOLD HOLD HOLD",
                "SO CLOSE", "WE'RE SO CLOSE", "SO CLOSE I CAN TASTE IT",
                "THE LAST JUMP", "THE LAST SPIKE", "IT'S THE LAST PART",
                "BREATHE BRO BREATHE", "EYES ON THE SCREEN",
                "99 PLEASE", "97 PLEASE", "PLEASE BE 100",
                "I'M NOT BLINKING", "NOBODY BLINK", "DON'T BLINK CHAT",
                "THIS IS THE MOMENT", "THE MOMENT", "IT'S THE MOMENT",
                "MY BLOOD PRESSURE", "BRO MY HEART RATE", "the heart monitor is going crazy",
                "LOOK AT THE BPM", "the monitor knows", "even the monitor is stressed",
                "if he dies here i'm done", "i'm turning the stream off if he dies",
                "the bpm on the right is not okay", "his heart rate is at 200 help",
                "that number on the right keeps climbing",
                "chat hold hands", "we're all holding hands right now",
                "i'm holding my breath", "still holding my breath",
                "do not sneeze", "nobody sneeze",
                "he's one spike away", "one spike away from history",
                "this is the most stressful thing i've watched",
                "my whole body is tense", "i need a break after this",
            });

            cross(v, { "PLEASE", "PLEASEE", "BRO PLEASE", "CHAT PLEASE" },
                { " DON'T DIE", " FINISH IT", " ONE MORE", " NOT NOW", " NOT AT 97" });

            cross(v, { "he's at", "bro is at", "he's literally at" },
                { " the last part", " the final jump", " 97", " the end", " the drop",
                    " the part he always dies at" });

            cross(v, { "if he", "if he doesn't", "the second he" },
                { " dies now i'm crying", " dies here i'm done",
                    " passes this he wins", " beats this i'm subbing for a year",
                    " chokes i'm leaving", " survives this it's over" });
        }

        // ---------------- celebration ----------------
        void buildCelebrate(Lines& v) {
            putAll(v, {
                "GG", "GGGG", "GGGGGG", "GG BRO", "GG CHAT", "GGWP", "GGS",
                "LET'S GOOOOO", "LETS GOOOOOO", "LET'S GOOOO CHAT",
                "HE DID IT", "HE ACTUALLY DID IT", "BRO DID IT",
                "FINALLY", "FINALLY BRO", "AFTER ALL THIS TIME",
                "MASSIVE", "MASSIVE W", "W STREAM", "W RUN",
                "WE WON", "WE ACTUALLY WON", "WE DID IT CHAT",
                "ABSOLUTE CINEMA", "CINEMA", "THIS IS CINEMA",
                "MUGA", "MUGAAAAA", "MUGAAAAAAAA",
                "97 NO MORE", "THE 97 CURSE IS BROKEN", "HE BEAT THE CURSE",
                "new hardest?", "is this his new hardest", "that's a new hardest",
                "somebody clip that", "CLIP IT", "clipped",
                "i was here", "i was here for this", "i can say i was here",
                "chat remembers this", "historic", "HISTORIC",
                "proud of you bro", "genuinely proud", "so deserved",
                "the grind paid off", "worth every death", "all those attempts paid off",
                "goosebumps", "actual goosebumps", "i got goosebumps",
                "not gonna lie i teared up", "i'm crying", "happy tears",
                "standing ovation", "clapping in my room", "everyone clap",
                "1 0 0", "ONE HUNDRED", "THE BIG 100",
                "deserved it more than anyone", "best streamer", "greatest of all time",
                "GOAT", "GOATED", "he's goated", "that was flawless",
                "cleanest completion i've seen", "textbook run",
                "the last click though", "that final click was perfect",
                "hold on i need to breathe", "i need a minute",
                "chat we did it", "we did it together",
                "put it on the channel", "this goes in the highlight reel",
                "best moment of my week", "this made my day",
                "i'm telling everyone at work about this",
                "the heart rate just went down lol", "his bpm is finally relaxing",
                "look at the bpm drop", "the monitor says calm now",
            });

            for (int n = 2; n <= 10; ++n) {
                put(v, "G" + std::string(static_cast<size_t>(n) + 1, 'G'));
            }
            for (int n = 3; n <= 9; ++n) {
                put(v, "LET'S G" + std::string(static_cast<size_t>(n), 'O') + "!");
            }
            for (int n = 1; n <= 4; ++n) {
                put(v, std::string(static_cast<size_t>(n), 'W') + " STREAM");
            }
            cross(v, { "GG", "congrats", "gratz", "well played", "so happy for" },
                { " bro", " chat", " everyone", " you", " king", " legend" });
            cross(v, { "that was", "this was", "genuinely" },
                { " beautiful", " incredible", " unreal", " the best run of the day",
                    " worth the wait", " cinema" });
        }

        // ---------------- death ----------------
        void buildDeath(Lines& v) {
            putAll(v, {
                "nooo", "NOOOO", "noooooo", "NOOOOOO",
                "F", "FFFF", "FFFFFFFF", "big F", "F in chat",
                "pain", "PAIN", "actual pain", "that hurt",
                "bro", "BRO", "BROOOO",
                "so close", "SO CLOSE", "that was so close",
                "my day is ruined", "day ruined", "stream ruined",
                "he was right there", "right at the end", "so close to the end",
                "gg attempt", "GG attempt", "ggs though",
                "unlucky", "UNLUCKY", "so unlucky",
                "that's gd", "welcome to gd", "that's geometry dash baby",
                "next one", "next attempt", "run it back", "RUN IT BACK",
                "reset", "RESET", "again", "AGAIN",
                "i felt that", "i felt that in my soul", "that physically hurt",
                "the game is rigged", "rigged", "that was rigged",
                "he deserves better", "robbed", "ROBBED",
                "one frame", "that was one frame off", "one input",
                "we go again", "WE GO AGAIN", "we go again chat",
                "ok that one was not his fault", "the hitbox", "THE HITBOX",
                "unlucky hitbox", "gd hitboxes are a joke",
                "chat needs a moment of silence", "moment of silence", "🕯️",
                "see you at 97 again", "back to 97 we go",
                "that was just disrespectful", "the level hates him",
                "the level is personal now", "he has beef with this level",
                "chat can we get an F", "everyone type F",
                "my heart sank", "my heart dropped",
                "i closed the stream and came back", "i looked away for one second",
                "one second of looking away", "i blame myself",
                "don't think about it, next attempt",
                "shake it off", "shake it off and go again",
                "the practice mode run was easier huh",
                "see this is why we practice",
                "we've seen worse", "he's come back from worse",
                "that's the worst one today", "that's the worst death of the stream",
            });

            cross(v, { "F", "rip", "RIP", "o7", "gg" },
                { " bro", " chat", " attempt", " run", " in chat" });
            cross(v, { "he was at", "bro was at", "at" },
                { " 97 again", " the last part", " the end", " the final jump", " 98 like always" });
            cross(v, { "that", "the", "he" },
                { " hurt more than it should", " was so close", " deserved 100", " got robbed" });
        }

        // ---------------- personal best ----------------
        void buildPB(Lines& v) {
            putAll(v, {
                "PB", "NEW PB", "PB BABY", "PB CHAT", "THAT'S A PB",
                "new best", "NEW BEST", "that's a new best",
                "personal best", "career best", "record",
                "we take those", "we take those chat", "progress",
                "that's progress", "progress is progress", "still progress",
                "big progress", "huge progress", "massive progress",
                "look at that number go", "the number went up", "number went up",
                "he's improving", "he's improving chat", "improvement arc",
                "consistency is paying off", "the grind is working", "it's working",
                "i told you the method works", "practice mode paying off",
                "the run was dead and he saved it", "saved it",
                "that's a pb right", "is that a pb", "pb or not that was clean",
                "bar moved", "the bar moved", "moved the bar",
                "he's getting closer", "closer every run", "closer and closer",
                "next stop 100", "next stop is 100", "one step closer to 100",
                "i saw that pb live", "witnessed a pb", "pb on stream",
                "that's the farthest he's been", "farthest run today",
                "deepest run of the day", "record run",
                "that's it, that's the one that leads to 100",
                "he's figured the level out", "the level is solved",
                "he learned the pattern", "pattern recognition is insane",
            });

            cross(v, { "PB", "new pb", "that's a pb", "nice pb" },
                { " at 97", " at the worst possible place", " but we take those",
                    " and it felt close", " bro" });
            cross(v, { "slowly", "gradually", "one attempt at a time" },
                { " getting there", " getting closer", " but surely", " moving up" });
        }

        // ---------------- donations ----------------
        void buildGifts(std::array<Lines, 16>& b) {
            auto& fun = b[G_FUN];
            putAll(fun, {
                "do you think you'll die at 97 again?", "serious question: do you actually enjoy this level?",
                "how do you still have hands after 3 hours?", "what's the plan if you never beat this",
                "how many keyboards have you broken", "do you sleep or is that a myth",
                "how do you not get angry, teach me", "what do you eat during a 6 hour session",
                "be honest: how many attempts today",
                "would you rather beat this or never play gd again",
                "what's your warmup level", "how long was your longest death streak",
                "do your neighbours complain about the clicking",
                "what song do you listen to before a run",
                "when you close your eyes do you see the level",
                "rate the level out of 10 honestly",
                "if you die at 97 do you have to give me my money back",
                "how is your bpm not 300 right now",
                "does the chat ever make you lose focus",
                "how do you pick which level to grind",
                "what's the hardest thing you've ever beaten",
                "how many hours a week is this",
                "do you ever dream about the level",
                "what's the worst death you've ever had",
                "at what percentage do you start getting nervous",
                "do you read chat during attempts",
            });
            cross(fun, { "how do you", "how did you", "when did you" },
                { " stay this calm", " get this good at wave", " not break your keyboard",
                    " learn to straight fly", " start playing gd" });

            // Donors in a GD chat are not a support group. They are the loudest people in the
            // room: short, loud, certain, and taking the mickey. No "we believe in you" speeches.
            auto& push = b[G_PUSH];
            putAll(push, {
                "$5 for the win, don't make me look stupid",
                "GO GO GO GO GO", "LOCK IN", "LOCK IN CHAT", "CLUTCH IT", "DON'T CHOKE",
                "TAKE IT HOME", "FINISH IT", "ONE MORE", "CLICK IT", "HOLD IT", "SEND IT",
                "cooking. absolutely cooking", "he's cooking, everyone stay calm",
                "BRO IS COOKING", "BRO IS LOCKED IN", "WE ARE SO BACK", "IT'S HAPPENING",
                "ABSOLUTE CINEMA", "this is cinema", "no shot he chokes this",
                "if you die here i'm refunding", "no pressure but everything is on this",
                "i paid $20 to watch you click one block",
                "do it for the clip", "clip it if he wins", "this is going in the compilation",
                "don't you dare die at 99", "beat it so i can go to bed",
                "i've bet my rent on this attempt", "my money is on the win, go",
                "only donated to say GO", "donating every attempt until you win",
                "go on then, prove the chat wrong",
                "you've got one job", "it's one block. hit it",
                "this attempt feels different, don't waste it",
                "i'm not even watching, i'm just here for the reaction",
                "send it chat, he needs the noise",
                "all in on this run", "let's see it", "we're watching, do something",
                "WAKE UP", "MUGA", "LETS GO CHAT", "CLEAN IT", "TAKE THE WIN",
            });

            // short and loud, the way people actually donate to a GD stream
            putAll(b[G_PUSH], {
                "GO GO GO GO GO GO", "NO WAY", "NO WAYYYY", "HOLY SHIT", "BRO IS COOKING",
                "BRO IS ACTUALLY COOKING", "LOCK IN", "LOCK IN CHAT", "MUGA", "WE ARE SO BACK",
                "WE ARE NEVER SO BACK", "ABSOLUTE CINEMA", "97", "97 AGAIN", "THE 97 CURSE",
                "CLIP THAT", "CLIP IT", "DON'T CHOKE", "CHOKING INCOMING", "I CAN'T WATCH",
                "I'M NOT BREATHING", "ONE CLICK", "ONE MORE", "PLEASE", "PLEASE GOD",
                "FINISH IT", "SEND IT", "TAKE THE WIN", "HIT THE LAST BLOCK",
                "IF YOU DIE HERE I'M DONE", "IF YOU WIN I'LL DONATE AGAIN",
                "DO IT FOR THE COMPILATION", "BEAT IT SO I CAN SLEEP",
                "I CAME HERE TO SEE A CHOKE", "PROVE ME WRONG", "PROVE CHAT WRONG",
                "THAT'S THE RUN", "THAT'S THE ONE", "THIS IS THE ATTEMPT",
                "LET'S GOOOOO", "LET'S GO CHAT", "WAKE UP CHAT", "STAY LOCKED IN",
                "HE'S LOCKED IN", "HE'S COOKING", "KING", "WHAT A RUN",
                "GOOD LUCK BRO", "GOOD LUCK", "WIN IT", "SMASH IT", "CLEAN IT",
                "GG IF YOU WIN", "GG EITHER WAY", "I'M SHAKING", "MY HEART",
                "DON'T THINK JUST CLICK", "TRUST YOUR HANDS", "SAME PACE",
                "MOMENTUM", "KEEP THE RUN", "DON'T RESET", "PLAY IT OUT",
                "SEND THE CLIP", "PUT IT ON YOUTUBE", "THIS IS CINEMA",
                "BEST STREAM TODAY", "1 MORE ATTEMPT", "LAST ONE I PROMISE",
                "OK NOW SERIOUSLY WIN", "GOOD LUCK ON THE LAST PART",
                "NOT THE ENDING PLEASE", "THE ENDING IS CURSED",
                "YOU HAD IT", "SO CLOSE", "PAIN", "ACTUAL PAIN",
                "I FELT THAT", "THAT ONE HURT", "THAT WAS PERSONAL",
            });

            auto& pred = b[G_PREDICT];
            putAll(pred, {
                "predicting you die at 97", "i bet you beat this within an hour",
                "my prediction: 99 then a choke",
                "you're beating this today, i can feel it",
                "you die at the exact same spot again, prove me wrong",
                "my bet is on the dual part", "you're getting to 99 this attempt",
                "i predict a new pb in the next 10 attempts",
                "you beat this by the end of the stream",
                "you die at the last jump, i've seen the timestamps",
                "next 5 attempts, one of them is the one",
                "i'm calling it: this hour", "you're going to choke at 99 one more time",
                "my money is on a death at the easiest part",
                "you're beating it before i finish my tea",
            });

            auto& joke = b[G_JOKE];
            putAll(joke, {
                "97 is your personality at this point",
                "you and 97 have a special relationship",
                "at least you're consistent at dying at 97",
                "97 should be your stream title",
                "you've died at 97 more times than i've blinked today",
                "have you considered not dying at 97",
                "if 97 was a person they'd have a restraining order",
                "you died at 99 on my birthday, thanks",
                "you should send 97 a christmas card",
                "97 is basically your second home",
                "your gravestone is going to say 'died at 97'",
                "the level should be renamed to 97 simulator",
                "you've sponsored 97 at this point",
                "i'm starting a support group for people who watched you die at 97",
                "my therapist knows about your 97 deaths",
            });

            auto& high = b[G_HIGH];
            putAll(high, {
                "that 98 was personal", "98 hurt to watch, i can't imagine playing it",
                "the last jump is cursed, confirmed",
                "you were one click away, one click",
                "i have never screamed at a screen like that",
                "the fact you're still going after that is insane",
                "that death at 99 broke something in me",
                "i was standing up for that attempt",
                "my heart rate is higher than yours right now",
                "that was the closest i've seen anyone get",
                "you had it, you literally had it",
                "i closed the tab and opened it again",
                "those last 3% were the longest 20 seconds of my life",
                "i was not prepared for that", "nobody was prepared for that",
                "i'm still recovering from that attempt",
            });

            auto& gd = b[G_GD];
            putAll(gd, {
                "this is the hardest thing i've seen since bloodbath",
                "the hitboxes in this game are a war crime",
                "gd players are built different",
                "this level is deco carried and i said it",
                "practice mode is the real carry",
                "show me your hardest demon before you flex",
                "is this harder than slaughterhouse",
                "the ship part is harder than the wave part, disagree",
                "everyone says wave is hard until you play cube straight fly",
                "buff this level, it's too easy for you",
                "gd on mobile should be an olympic sport",
                "the firework at the end always gets me emotional",
                "i have 400 hours in gd and i can't do this",
                "this is why gd is the best game ever made",
                "one day i'll be half as good as you",
                "the community made this level, insane",
            });

            auto& st = b[G_STREAM];
            putAll(st, {
                "what's the level called, i want to play it and suffer too",
                "i'm downloading this level so i can hate it myself",
                "the song in this level is so good",
                "i've been here for 4 hours and i regret nothing",
                "$10 says you die at 97 again",
                "if you beat this i'll donate $50, and i know you won't",
                "i'm only here for the choke and you keep not choking",
                "donated so i can say i was here for the win",
                "donated so i can say i was here for the choke, whichever",
                "this is my fourth donation tonight, win something",
                "i have donated more than you've gained percent",
                "beat it and i'll stop donating. so don't.",
                "5 dollars to tell you the ending is cursed",
                "money is on 99. prove me wrong",
            });
        }

        // ---------------- the big expansion ----------------
        // Curated clause groups. A template only ever crosses clauses from the same group, so the
        // combinations stay grammatical ("that last jump", "the wave part", "this attempt") while
        // the reaction part stays varied ("was criminal", "hurt to watch").
        void buildClauseChat(std::array<Lines, 16>& b) {
            static char const* PARTS[] = { "that last jump", "the wave part", "that dual", "this attempt",
                "the ending", "that drop", "the ship part", "the pre-drop", "that orb",
                "the last spike", "this run", "that click", "the first part", "that transition" };

            static char const* HYPE_TAILS[] = { "is cooking right now", "is not missing a single click",
                "is locked in like never before", "is in the zone", "is playing out of his mind",
                "is so calm it's scary", "is making this look easy", "is built for this level",
                "is on another level today", "is actually going to do it" };
            static char const* HYPE_HEADS[] = { "he ", "bro ", "this guy ", "chat he " };
            cross(b[C_HYPE], HYPE_HEADS, HYPE_TAILS);

            // "X% is where it always ends" style lines read the percentage in the overlay
            static char const* PCT_HEADS[] = { "we always die at ", "it's always ", "of course it's ",
                "back to ", "everyone say " };
            static char const* PCT_TAILS[] = { "97", "98", "the same spot", "the last jump", "the final click" };
            cross(b[C_PREDICT], PCT_HEADS, PCT_TAILS);

            static char const* CELEB_HEADS[] = { "i was here for the ", "clipping the ", "the ", "put the " };
            static char const* CELEB_TAILS[] = { "completion", "final click", "last jump", "win", "celebration",
                "moment" };
            cross(b[C_CELEBRATE], CELEB_HEADS, CELEB_TAILS);

            // small talk between viewers, which is what makes a chat feel alive at low hype
            static char const* TALK_HEADS[] = { "i've been watching for ", "he's been playing for ",
                "stream started ", "i got here ", "been here " };
            static char const* TALK_TAILS[] = { " three hours straight", " an hour now", " since the beginning",
                " like ten minutes ago", " way too long", " all night", " since yesterday's stream" };
            cross(b[C_NORMAL], TALK_HEADS, TALK_TAILS);

            static char const* ASK_HEADS[] = { "does anyone know ", "can someone tell me ", "seriously, ",
                "real question: ", "chat, " };
            static char const* ASK_TAILS[] = { "how long this level is", "what his hardest is",
                "how many attempts that was", "what the timer is counting",
                "how many hours he's played today", "which demon this is", "how far he's got before" };
            cross(b[C_NORMAL], ASK_HEADS, ASK_TAILS);

            static char const* SMALL_HEADS[] = { "hi ", "hey ", "hello ", "yo ", "morning ", "good luck " };
            static char const* SMALL_TAILS[] = { "chat", "everyone", "all", "fellow viewers", "gamers",
                "first time here", "o/", ":)" };
            cross(b[C_NORMAL], SMALL_HEADS, SMALL_TAILS);

            static char const* SPAM_HEADS[] = { "GO ", "LOCK IN ", "CLICK ", "PLEASE ", "HOLD ", "DON'T DIE ",
                "KEEP GOING ", "FOCUS ", "BREATHE " };
            static char const* SPAM_TAILS[] = { "bro", "chat", "chat chat", "right now", "please",
                "i'm begging", "one more time", "just once", "for the love of god" };
            cross(b[C_SPAM], SPAM_HEADS, SPAM_TAILS);
        }

        void buildClauseGifts(std::array<Lines, 16>& b) {
            static char const* HEADS[] = { "i've been watching you die for ", "i've been in this chat for ",
                "been here for ", "watching since " };
            static char const* TAILS[] = { " three hours and you still can't do the last jump",
                " two months and you're still at the same percent", " the whole day, absolute cinema",
                " the beginning and you haven't improved once", " an hour and i've seen 40 deaths",
                " years, the level always wins", " one stream and i already know how it ends" };
            cross(b[G_STREAM], HEADS, TAILS);

            static char const* PUSH_HEADS[] = { "go on, ", "send it, ", "lock in, ", "clutch it, ", "one more, " };
            static char const* PUSH_TAILS[] = { "the level can't beat you forever",
                "chat is not going anywhere until you win", "every attempt is one closer to the choke",
                "you're closer than you think and that's the problem", "the win is coming, probably",
                "we'll be here when you die again", "no pressure, everything is on this",
                "we're all screaming at you, in a nice way" };
            cross(b[G_PUSH], PUSH_HEADS, PUSH_TAILS);

            static char const* FUN_HEADS[] = { "honest question: ", "real question: ", "asking for a friend: ",
                "quick one: ", "important question: " };
            static char const* FUN_TAILS[] = { "how do you stay focused for hours",
                "do you ever get bored of this level", "what's your pre-run ritual",
                "how many breaks do you take", "does the clicking ever annoy you",
                "what would you do if you beat this first try", "how do you pick your levels",
                "would you ever do a 24 hour stream", "do you play other games",
                "what's your favourite gd level" };
            cross(b[G_FUN], FUN_HEADS, FUN_TAILS);

            static char const* GD_HEADS[] = { "this level ", "the deco ", "the gameplay ",
                "the song ", "the ending " };
            static char const* GD_TAILS[] = { "deserves way more attention", "is criminally underrated",
                "is better than most featured levels", "makes me want to open the editor",
                "feels like a megacollab", "is peak gd design", "goes so hard at the drop" };
            cross(b[G_GD], GD_HEADS, GD_TAILS);

            static char const* HIGH_HEADS[] = { "i physically stood up when you hit ", "the entire chat was screaming at ",
                "nobody was ready for ", "i'm still thinking about ", "i can't stop thinking about " };
            static char const* HIGH_TAILS[] = { "that last jump", "98%", "the final click", "the 99 death",
                "the ending", "that attempt" };
            cross(b[G_HIGH], HIGH_HEADS, HIGH_TAILS);

            static char const* JOKE_HEADS[] = { "at this point ", "honestly ", "not gonna lie, ",
                "everyone knows " };
            static char const* JOKE_TAILS[] = { "97 is your final boss", "the last jump has a personal grudge against you",
                "you and that spike need couples therapy", "the level is winning on purpose",
                "you should just live at 97%", "chat has seen more 97 deaths than the level designer" };
            cross(b[G_JOKE], JOKE_HEADS, JOKE_TAILS);

            static char const* PRED_HEADS[] = { "my bet: ", "predicting ", "calling it now: ", "the prophecy says " };
            static char const* PRED_TAILS[] = { "a death at 97", "you get it in the next 20 minutes",
                "a new pb within 5 attempts", "you choke one more time then win",
                "you beat it right after a break", "the win comes at attempt 300" };
            cross(b[G_PREDICT], PRED_HEADS, PRED_TAILS);
        }

        // ---- lines about the level that's being played ($L) ----
        // These only make sense once the level's name is known, so they're kept in their own bucket
        // and the overlay only picks them when gLevelName isn't empty. `$L` is replaced there.
        void buildLevelChat(Lines& v) {
            putAll(v, {
                "$L is such a good level", "$L again, chat we've been here all day",
                "this part of $L always gets him", "the ending of $L is criminal",
                "i've played $L, it's actually harder than it looks",
                "$L is the best level he's played on stream",
                "how many times can one person play $L",
                "if he beats $L today i'm buying the merch",
                "$L should be in the next gd awards", "the song in $L is stuck in my head",
                "$L deco is unreal", "$L is a masterpiece and i will not elaborate",
                "he's been on $L for so long", "$L is his white whale at this point",
            });
            cross(v, { "the ", "this part of ", "that bit in " },
                { "$L is so unfair", "$L is actually beautiful", "$L deserves a top 10 spot" });
            cross(v, { "i want to play ", "i could never play ", "i am NOT playing " },
                { "$L", "$L without practice mode", "$L on mobile", "$L after watching this" });
            cross(v, { "$L's ", "the " },
                { "wave part is the hardest", "ending is the best part",
                    "first drop is so good", "last jump is criminal" });
        }

        // lines that read the actual run: $P = percentage, $A = attempts
        void buildGiftTemplates(Lines& v) {
            putAll(v, {
                "you're at $P% and i'm not breathing",
                "$P% is not a real number to be dying at",
                "i will donate again if you don't die at $P",
                "the $P curse is watching",
                "you die at $P and i'm going to bed",
                "beat it before attempt $A",
                "$A attempts and still going, respect",
                "attempt number $A is the one, i've done the math",
                "you were at $P and i blinked",
                "i've watched $A attempts today, please finish this",
                "chat please don't let him die at $P",
                "if you get past $P you're winning",
                "$P is where it always ends, prove me wrong",
                "you're $P in and i've already accepted the worst",
                "at $P my heart rate matched yours",
            });
        }

        template <size_t N>
        Table finish(std::array<Lines, N>& b, int cats) {
            Table t;
            for (int c = 0; c < cats; ++c) {
                t.bounds[static_cast<size_t>(c)] = static_cast<int>(t.lines.size());
                for (auto& s : b[static_cast<size_t>(c)]) {
                    t.lines.push_back(s);
                }
            }
            t.bounds[static_cast<size_t>(cats)] = static_cast<int>(t.lines.size());
            for (size_t c = static_cast<size_t>(cats) + 1; c < t.bounds.size(); ++c) {
                t.bounds[c] = t.bounds[static_cast<size_t>(cats)];
            }
            t.size = static_cast<int>(t.lines.size());

            // de-duplicate (keeping the first), then rebuild the bounds: the fragments cross in
            // ways that occasionally produce the same string twice, and echoes inside a category
            // read like a looping bot
            std::unordered_set<std::string> seen;
            std::vector<std::string> unique;
            std::array<int, 16> nb{};
            unique.reserve(t.lines.size());
            int count = 0;
            for (int c = 0; c < cats; ++c) {
                nb[static_cast<size_t>(c)] = count;
                for (int i = t.first(c); i < t.first(c) + t.count(c); ++i) {
                    std::string line = sanitize(squish(t.at(i)));
                    if (seen.insert(line).second) {
                        unique.push_back(std::move(line));
                        ++count;
                    }
                }
            }
            nb[static_cast<size_t>(cats)] = count;
            for (size_t c = static_cast<size_t>(cats) + 1; c < nb.size(); ++c) {
                nb[c] = count;
            }
            t.lines = std::move(unique);
            t.bounds = nb;
            t.size = count;
            return t;
        }

        Table makeChat() {
            std::array<Lines, 16> b;
            buildNormal(b[C_NORMAL]);
            buildReact(b[C_REACT]);
            buildSpam(b[C_SPAM]);
            buildEmotes(b[C_EMOTE]);
            buildPredict(b[C_PREDICT]);
            buildHype(b[C_HYPE]);
            buildPanic(b[C_PANIC]);
            buildCelebrate(b[C_CELEBRATE]);
            buildDeath(b[C_DEATH]);
            buildPB(b[C_PB]);
            buildClauseChat(b);
            buildLevelChat(b[C_LEVEL]);
            buildGo(b[C_GO]);

            // de-duplicate first, then give each line a couple of tail variants: expanding before
            // de-duplicating would spend most of the expansion on strings that already exist
            auto tidy = [](Lines& v) {
                std::unordered_set<std::string> seen;
                Lines out;
                out.reserve(v.size());
                for (auto& s : v) {
                    if (seen.insert(s).second) {
                        out.push_back(s);
                    }
                }
                v = std::move(out);
            };
            for (auto& v : b) {
                tidy(v);
            }

            std::array<size_t, 16> base{};
            for (int c = 0; c < C_COUNT; ++c) {
                base[static_cast<size_t>(c)] = b[static_cast<size_t>(c)].size();
            }

            // first pass: two tails per line
            expandFrom(b[C_NORMAL], b[C_NORMAL].size(), { " lol", " bro", " chat", " fr", " ngl", " XD", " pls", " honestly" }, 2, 0);
            expandFrom(b[C_REACT], b[C_REACT].size(), { "", " lol", " bro", " chat", " fr", " 💀", " lmao", " ngl" }, 2, 1);
            expandFrom(b[C_PREDICT], b[C_PREDICT].size(), { " trust", " i'm calling it", " watch", " prove me wrong", " chat" }, 2, 2);
            expandFrom(b[C_HYPE], b[C_HYPE].size(), { " chat", " bro", " fr", " let's go", " today" }, 2, 3);
            expandFrom(b[C_PANIC], b[C_PANIC].size(), { " please", " bro", " chat", " 😭", " 💀", " now" }, 2, 4);
            expandFrom(b[C_CELEBRATE], b[C_CELEBRATE].size(), { " chat", " bro", " fr", " 💀", "!!!" }, 2, 5);
            expandFrom(b[C_DEATH], b[C_DEATH].size(), { "", " bro", " chat", " 💀", " lmao", " again" }, 2, 6);
            expandFrom(b[C_PB], b[C_PB].size(), { " chat", " bro", " fr", " at last", "!!!" }, 2, 7);

            // second pass: one more, on the originals only, to get past five thousand lines
            expandFrom(b[C_REACT], base[C_REACT], { " dude", " man", " honestly", " again", " i can't" }, 1, 0);
            expandFrom(b[C_HYPE], base[C_HYPE], { " let's go", " no way", " i'm shaking", " 💀" }, 1, 1);
            expandFrom(b[C_PANIC], base[C_PANIC], { " i can't breathe", " again", " not again", " 🙏" }, 1, 2);
            expandFrom(b[C_CELEBRATE], base[C_CELEBRATE], { " finally", " let's go", " deserved", " 🎉" }, 1, 3);
            expandFrom(b[C_DEATH], base[C_DEATH], { " man", " dude", " i'm done", " pls" }, 1, 4);
            expandFrom(b[C_PB], base[C_PB], { " let's go", " finally", " nice", " 🎉" }, 1, 5);
            expandFrom(b[C_NORMAL], base[C_NORMAL], { " 😭", " tbh", " honestly", " though" }, 1, 6);
            expandFrom(b[C_SPAM], base[C_SPAM], { " now", " please", " chat", "!!!", " again" }, 3, 7);
            expandFrom(b[C_PREDICT], base[C_PREDICT], { " guaranteed", " watch", " chat", " lol" }, 1, 8);
            return finish(b, C_COUNT);
        }

        Table makeGifts() {
            std::array<Lines, 16> b;
            buildGifts(b);
            buildClauseGifts(b);

            auto tidy = [](Lines& v) {
                std::unordered_set<std::string> seen;
                Lines out;
                for (auto& s : v) {
                    if (seen.insert(s).second) {
                        out.push_back(s);
                    }
                }
                v = std::move(out);
            };
            for (auto& v : b) {
                tidy(v);
            }
            std::array<size_t, 16> gbase{};
            for (int c = 0; c < G_COUNT; ++c) {
                gbase[static_cast<size_t>(c)] = b[static_cast<size_t>(c)].size();
            }
            expandFrom(b[G_PUSH], gbase[G_PUSH], { " do it for the clip", " and i mean it",
                " keep the pace", " we believe", " after all this time", " no pressure" }, 3, 0);
            expandFrom(b[G_FUN], gbase[G_FUN], { "", " asking for a friend", " genuinely curious", " chat" }, 2, 1);
            expandFrom(b[G_JOKE], gbase[G_JOKE], { " and i stand by it", " not sorry", " honestly", " chat agrees" }, 2, 2);
            expandFrom(b[G_HIGH], gbase[G_HIGH], { " and i'm still not over it", " chat was unwell", " i needed a break" }, 2, 3);
            expandFrom(b[G_GD], gbase[G_GD], { " and that's a fact", " no notes", " respect", " trust me" }, 2, 4);
            expandFrom(b[G_STREAM], gbase[G_STREAM], { "", " and i mean that", " absolute cinema" }, 2, 5);
            expandFrom(b[G_PREDICT], gbase[G_PREDICT], { "", " prove me wrong", " i'll be here", " chat" }, 2, 6);
            expandFrom(b[G_STREAM], gbase[G_STREAM], { " unreal", " every time", " chat is right here" }, 1, 7);
            expandFrom(b[G_GD], gbase[G_GD], { " fact check me", " i said what i said", " gd 4 life" }, 1, 8);
            expandFrom(b[G_HIGH], gbase[G_HIGH], { " i'm not okay", " chat was not okay", " oh my god" }, 1, 9);
            expandFrom(b[G_JOKE], gbase[G_JOKE], { " and that's the truth", " sorry", " facts" }, 1, 10);
            expandFrom(b[G_PUSH], gbase[G_PUSH], { " GO", " now", " don't think about it" }, 1, 11);
            return finish(b, G_COUNT);
        }

        std::vector<std::string> makeTemplates() {
            Lines v;
            buildGiftTemplates(v);
            return v;
        }

    }  // namespace

    Table const& chat() {
        static Table t = makeChat();
        return t;
    }

    Table const& gifts() {
        static Table t = makeGifts();
        return t;
    }

    std::vector<std::string> const& giftTemplates() {
        static std::vector<std::string> v = makeTemplates();
        return v;
    }

}  // namespace streamui::text
