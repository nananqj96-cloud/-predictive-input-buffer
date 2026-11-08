// FrameWave v3.3: the stream overlay engine.
//
// What this does, in one paragraph: it reads the real percentage from PlayLayer every frame and
// runs a chat feed, a donation alert feed and a heart-rate monitor on top of the game. The overlay
// is built from cocos2d draws and bitmap labels only: no textures, no network, no audio files. It
// never allocates nodes while a level runs (chat rows are recycled), and nothing it displays is
// real: no viewers, no money, no heartbeat.
//
// The overlay deliberately shows no feature labels (no "fake", no "simulated"): it's styled like a
// normal streaming layout. Only the settings menu uses those words.

#include "stream.hpp"
#include "stream_text.hpp"
#include "stream_names.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <cocos2d.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace geode::prelude;
using namespace cocos2d;

namespace streamui {

    bool dirty = false;
    int publishedChat = 0;
    int publishedGifts = 0;

    namespace {

        // =====================================================================
        // utilities
        // =====================================================================

        inline uint32_t rnd32(uint32_t& s) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s;
        }

        inline float rndf(uint32_t& s) {
            return static_cast<float>(rnd32(s) >> 8) / 16777216.f;
        }

        inline int rndInt(uint32_t& s, int lo, int hi) {
            if (hi <= lo) {
                return lo;
            }
            return lo + static_cast<int>(rnd32(s) % static_cast<uint32_t>(hi - lo + 1));
        }

        inline float bell(uint32_t& s) {
            return (rndf(s) + rndf(s) + rndf(s)) / 3.f;
        }

        ccColor3B fromHex(unsigned rgb) {
            return { static_cast<GLubyte>((rgb >> 16) & 0xff), static_cast<GLubyte>((rgb >> 8) & 0xff),
                static_cast<GLubyte>(rgb & 0xff) };
        }

        ccColor4F withAlpha(ccColor3B c, float a) {
            return { c.r / 255.f, c.g / 255.f, c.b / 255.f, a };
        }

        std::string number(long long v) {
            std::string s = std::to_string(v < 0 ? -v : v);
            std::string out;
            int n = 0;
            for (auto it = s.rbegin(); it != s.rend(); ++it) {
                if (n && n % 3 == 0) {
                    out.push_back(',');
                }
                out.push_back(*it);
                ++n;
            }
            std::reverse(out.begin(), out.end());
            return (v < 0 ? "-" : "") + out;
        }

        // ---- the name colours (Twitch's classic palette) ----
        unsigned const PALETTE[] = { 0xFF0000, 0x0000FF, 0x00FF00, 0xB22222, 0xFF7F50, 0x9ACD32, 0xFF4500,
            0x2E8B57, 0xDAA520, 0xD2691E, 0x5F9EA0, 0x1E90FF, 0xFF69B4, 0x8A2BE2, 0x00FF7F, 0x1B9E77,
            0x66C2A5, 0xE6AB02, 0xA6761D, 0x7570B3, 0xFF6B6B, 0x4ECDC4, 0x45B7D1, 0xF7B32B, 0x7B68EE,
            0xE75480, 0x00CED1, 0xADFF2F, 0xFFA07A, 0x9370DB };

        char const* const PALETTE_NAMES[] = { "red", "blue", "green", "brick", "coral", "yellow green", "orange red",
            "sea green", "goldenrod", "chocolate", "cadet blue", "dodger blue", "hot pink", "blue violet",
            "spring green", "teal", "mint", "amber", "brown", "slate", "salmon", "turquoise", "sky blue",
            "mustard", "violet", "rose", "cyan", "lime", "apricot", "lavender" };

        ccColor3B paletteColor(int index) {
            return fromHex(PALETTE[static_cast<size_t>(std::clamp(index, 0, 29))]);
        }

        // =====================================================================
        // settings
        // =====================================================================

        struct Settings {
            bool chat = true;
            bool viewers = true;
            bool gifts = true;
            bool heart = true;
            bool tts = false;
            int pos = 3;
            float cx = 0.78f;
            float cy = 0.30f;
            float width = 300.f;
            float height = 240.f;
            float opacity = 0.92f;
            float speed = 6.f;
            int participants = 100;
            int viewersMin = 800;
            int viewersMax = 2400;
            int viewersExact = 0;
            bool viewersRandom = true;
            bool viewersGradual = true;
            int ttsVoice = 0;
            float ttsVolume = 0.8f;
            float ttsRate = 1.f;
            float ttsDelay = 1.5f;
            bool ttsMessage = true;
            bool showTime = true;
            bool chip = true;          // the little "menu" handle on the overlay
            bool ecg = true;           // the cyan trace beside the heart monitor
            bool header = false;       // "GdStream" above the chat panel
            int vspread = 120;         // how far the viewer count wanders, as a % of the range
            float headScale = 0.42f;   // "GdStream" size (PUSAB units)
            float ecgWidth = 1.0f;     // trace thickness multiplier
            float scale = 1.f;
        };

        Settings S;
        bool gChipRuntime = true;   // the menu hides the chip while its own window is open

        void loadSettings() {
            auto& m = *Mod::get();
            S.chat = m.getSavedValue<bool>("stream-chat", true);
            S.viewers = m.getSavedValue<bool>("stream-viewers", true);
            S.gifts = m.getSavedValue<bool>("stream-gifts", true);
            S.heart = m.getSavedValue<bool>("stream-heart", true);
            S.tts = m.getSavedValue<bool>("stream-tts", false);
            S.pos = std::clamp(m.getSavedValue<int>("stream-pos", 3), 0, 4);
            S.cx = std::clamp(m.getSavedValue<float>("stream-cx", 0.78f), 0.f, 1.f);
            S.cy = std::clamp(m.getSavedValue<float>("stream-cy", 0.30f), 0.f, 1.f);
            S.width = std::clamp(m.getSavedValue<float>("stream-w", 300.f), 150.f, 900.f);
            S.height = std::clamp(m.getSavedValue<float>("stream-h", 240.f), 100.f, 900.f);
            S.opacity = std::clamp(m.getSavedValue<float>("stream-opacity", 0.92f), 0.15f, 1.f);
            S.speed = std::clamp(m.getSavedValue<float>("stream-speed", 6.f), 0.2f, 60.f);
            S.participants = std::clamp(m.getSavedValue<int>("stream-participants", 100), 1, 5000);
            S.viewersMin = std::max(0, m.getSavedValue<int>("stream-vmin", 800));
            S.viewersMax = std::max(S.viewersMin, m.getSavedValue<int>("stream-vmax", 2400));
            S.viewersExact = std::max(0, m.getSavedValue<int>("stream-vexact", 0));
            S.viewersRandom = m.getSavedValue<bool>("stream-vrand", true);
            S.viewersGradual = m.getSavedValue<bool>("stream-vgrad", true);
            S.ttsVoice = std::max(0, m.getSavedValue<int>("stream-tts-voice", 0));
            S.ttsVolume = std::clamp(m.getSavedValue<float>("stream-tts-vol", 0.8f), 0.f, 1.f);
            S.ttsRate = std::clamp(m.getSavedValue<float>("stream-tts-rate", 1.f), 0.5f, 2.f);
            S.ttsDelay = std::clamp(m.getSavedValue<float>("stream-tts-delay", 1.5f), 0.f, 15.f);
            S.ttsMessage = m.getSavedValue<bool>("stream-tts-message", true);
            S.showTime = m.getSavedValue<bool>("stream-timestamps", true);
            S.chip = m.getSavedValue<bool>("stream-chip", true);
            S.ecg = m.getSavedValue<bool>("stream-ecg", true);
            S.header = m.getSavedValue<bool>("stream-header", false);
            S.vspread = std::clamp(m.getSavedValue<int>("stream-vspread", 120), 20, 400);
            S.headScale = std::clamp(m.getSavedValue<float>("stream-head-scale", 0.42f), 0.10f, 1.20f);
            S.ecgWidth = std::clamp(m.getSavedValue<float>("stream-ecg-width", 1.f), 0.3f, 2.5f);
            S.scale = std::clamp(m.getSavedValue<float>("stream-scale", 1.f), 0.25f, 3.f);
        }

        // =====================================================================
        // bots
        // =====================================================================

        std::vector<Bot> gBots;
        std::vector<Bot> gCustom;
        std::vector<int> gActive;
        std::vector<float> gNext;
        std::vector<std::string> gCustomGifts;

        void poolOfBots() {
            auto namesList = names::generate(0x5EED97u, 620);
            gBots.clear();
            gBots.reserve(namesList.size() + gCustom.size());
            uint32_t s = 0xB07B07u;
            for (auto const& n : namesList) {
                Bot b;
                b.name = n;
                b.color = static_cast<int>(rnd32(s) % 30u);
                // a handful of badges, like the screenshots: mod icons next to a few names
                uint32_t roll = rnd32(s) % 100u;
                b.badge = roll < 2 ? 1 : roll < 7 ? 2 : roll < 22 ? 3 : roll < 27 ? 4 : 0;
                b.freq = 0.35f + rndf(s) * 1.65f;
                b.hype = rndInt(s, 5, 95);
                b.style = static_cast<int>(rnd32(s) % 5u);
                b.custom = false;
                gBots.push_back(std::move(b));
            }
            for (auto const& c : gCustom) {
                gBots.push_back(c);
            }
        }

        void rebuildActive(uint32_t seed = 0x1234ABCDu) {
            gActive.clear();
            int want = std::clamp(S.participants, 1, 5000);
            size_t firstCustom = gBots.size() - gCustom.size();
            for (size_t i = firstCustom; i < gBots.size() && static_cast<int>(gActive.size()) < want; ++i) {
                gActive.push_back(static_cast<int>(i));
            }
            // a stable shuffle of the generated pool, so "who is in chat" differs per setting but
            // stays the same between sessions
            std::vector<int> order;
            order.reserve(gBots.size());
            for (size_t i = 0; i < firstCustom; ++i) {
                order.push_back(static_cast<int>(i));
            }
            uint32_t s = seed;
            for (size_t i = order.size(); i > 1; --i) {
                std::swap(order[i - 1], order[rnd32(s) % i]);
            }
            for (int idx : order) {
                if (static_cast<int>(gActive.size()) >= want) {
                    break;
                }
                gActive.push_back(idx);
            }
            gNext.assign(gActive.size(), 0.f);
            for (size_t i = 0; i < gNext.size(); ++i) {
                gNext[i] = rndf(s) * 0.8f;
            }
        }

        std::string savePath(char const* file) {
            return (Mod::get()->getSaveDir() / file).string();
        }

        void loadCustom() {
            gCustom.clear();
            std::ifstream in(savePath("stream-bots.txt"));
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                std::vector<std::string> parts;
                size_t start = 0;
                while (true) {
                    size_t tab = line.find('\t', start);
                    parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                    if (tab == std::string::npos) {
                        break;
                    }
                    start = tab + 1;
                }
                if (parts.size() < 6 || !names::acceptable(parts[0])) {
                    continue;
                }
                Bot b;
                b.name = parts[0];
                b.color = std::clamp(atoi(parts[1].c_str()), 0, 29);
                b.badge = std::clamp(atoi(parts[2].c_str()), 0, 4);
                b.freq = std::clamp(static_cast<float>(atof(parts[3].c_str())), 0.1f, 5.f);
                b.hype = std::clamp(atoi(parts[4].c_str()), 0, 100);
                b.style = std::clamp(atoi(parts[5].c_str()), 0, 4);
                b.custom = true;
                for (size_t i = 6; i < parts.size(); ++i) {
                    if (!parts[i].empty()) {
                        b.lines.push_back(parts[i]);
                    }
                }
                gCustom.push_back(std::move(b));
            }
        }

        void saveCustom() {
            std::ofstream out(savePath("stream-bots.txt"), std::ios::trunc);
            if (!out) {
                return;
            }
            out << "# FrameWave custom chat bots. One per line: name<TAB>colour<TAB>badge<TAB>frequency<TAB>hype<TAB>style<TAB>messages...\n";
            for (auto const& b : gCustom) {
                out << b.name << '\t' << b.color << '\t' << b.badge << '\t' << b.freq << '\t' << b.hype << '\t' << b.style;
                for (auto const& l : b.lines) {
                    out << '\t' << l;
                }
                out << '\n';
            }
        }

        void loadCustomGifts() {
            gCustomGifts.clear();
            std::ifstream in(savePath("stream-donations.txt"));
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line[0] != '#') {
                    gCustomGifts.push_back(line);
                }
            }
        }

        void saveCustomGifts() {
            std::ofstream out(savePath("stream-donations.txt"), std::ios::trunc);
            if (!out) {
                return;
            }
            out << "# FrameWave custom donation messages, one per line\n";
            for (auto const& g : gCustomGifts) {
                out << g << '\n';
            }
        }

        // =====================================================================
        // text measuring (a reused probe label: no per-message node churn)
        // =====================================================================

        CCLabelBMFont* probe(char const* fnt) {
            // one probe label per font: measuring with the wrong font used to make every
            // non-chat measurement come out at whatever font happened to be asked for first
            static std::vector<std::pair<std::string, CCLabelBMFont*>> cache;
            for (auto& e : cache) {
                if (e.first == fnt) {
                    return e.second;
                }
            }
            auto* label = CCLabelBMFont::create("Ag", fnt);
            if (!label) {
                // a font that isn't there: fall back to the game's own, never crash the measure
                label = CCLabelBMFont::create("Ag", "bigFont.fnt");
            }
            label->retain();
            cache.emplace_back(fnt, label);
            return label;
        }

        float textW(std::string const& s, float scale, char const* fnt) {
            if (s.empty()) {
                return 0.f;
            }
            auto* p = probe(fnt);
            p->setString(s.c_str());
            return p->getContentSize().width * scale;
        }

        float lineHeight(float scale, char const* fnt) {
            auto* p = probe(fnt);
            p->setString("Ag");
            return p->getContentSize().height * scale;
        }

        // wraps to maxW: returns the lines, and the height through outH
        std::vector<std::string> wrapText(std::string const& in, float scale, char const* fnt, float maxW, float* outH) {
            std::vector<std::string> lines;
            std::string cur;
            std::string word;
            auto flush = [&]() {
                if (word.empty()) {
                    return;
                }
                std::string next = cur.empty() ? word : cur + " " + word;
                if (textW(next, scale, fnt) > maxW && !cur.empty()) {
                    lines.push_back(cur);
                    cur = word;
                }
                else {
                    cur = std::move(next);
                }
                word.clear();
            };
            for (char ch : in) {
                if (ch == ' ') {
                    flush();
                }
                else {
                    word.push_back(ch);
                }
            }
            flush();
            if (!cur.empty()) {
                lines.push_back(cur);
            }
            if (lines.empty()) {
                lines.emplace_back();
            }
            if (outH) {
                *outH = static_cast<float>(lines.size());
            }
            return lines;
        }

        // =====================================================================
        // drawing helpers
        // =====================================================================

        // cocos2d's drawCircle takes (center, radius, angle, segments, colour, borderWidth, borderColour);
        // these two wrappers keep the call sites readable
        // drawCircle(centre, radius, fill, borderWidth, borderColour, segments)
        void circleFill(CCDrawNode* d, CCPoint at, float r, ccColor4F fill, unsigned seg = 18) {
            d->drawCircle(at, r, fill, 0.f, fill, seg);
        }

        void circleRing(CCDrawNode* d, CCPoint at, float r, ccColor4F fill, float width, ccColor4F ring, unsigned seg = 18) {
            d->drawCircle(at, r, fill, width, ring, seg);
        }

        void rectFill(CCDrawNode* d, CCRect r, ccColor4F fill) {
            d->drawRect(r, fill, 0.f, fill);
        }


        void roundRect(CCDrawNode* d, CCRect r, float rad, ccColor4F fill, float border = 0.f,
            ccColor4F borderColor = { 0, 0, 0, 0 }) {
            if (rad <= 0.f) {
                if (fill.a > 0.f) {
                    CCPoint v[] = { { r.origin.x, r.origin.y }, { r.getMaxX(), r.origin.y }, { r.getMaxX(), r.getMaxY() },
                        { r.origin.x, r.getMaxY() } };
                    d->drawPolygon(v, 4, fill, 0.f, { 0, 0, 0, 0 });
                }
            }
            else {
                rad = std::min(rad, std::min(r.size.width, r.size.height) / 2.f);
                if (fill.a > 0.f) {
                    CCPoint v[] = { { r.origin.x + rad, r.origin.y }, { r.getMaxX() - rad, r.origin.y },
                        { r.getMaxX(), r.origin.y + rad }, { r.getMaxX(), r.getMaxY() - rad },
                        { r.getMaxX() - rad, r.getMaxY() }, { r.origin.x + rad, r.getMaxY() },
                        { r.origin.x, r.getMaxY() - rad }, { r.origin.x, r.origin.y + rad } };
                    d->drawPolygon(v, 8, fill, 0.f, { 0, 0, 0, 0 });
                    circleFill(d, { r.origin.x + rad, r.origin.y + rad }, rad - 0.3f, fill, 12);
                    circleFill(d, { r.getMaxX() - rad, r.origin.y + rad }, rad - 0.3f, fill, 12);
                    circleFill(d, { r.origin.x + rad, r.getMaxY() - rad }, rad - 0.3f, fill, 12);
                    circleFill(d, { r.getMaxX() - rad, r.getMaxY() - rad }, rad - 0.3f, fill, 12);
                }
            }
            if (border > 0.f) {
                float rr = std::min(std::max(0.5f, rad), std::min(r.size.width, r.size.height) / 2.f);
                if (rr < 1.2f) {
                    // small boxes: the four straight runs read as a rounded frame on their own
                    rr = 1.2f;
                }
                // four straight runs
                d->drawSegment({ r.origin.x + rr, r.origin.y }, { r.getMaxX() - rr, r.origin.y }, border, borderColor);
                d->drawSegment({ r.origin.x + rr, r.getMaxY() }, { r.getMaxX() - rr, r.getMaxY() }, border, borderColor);
                d->drawSegment({ r.origin.x, r.origin.y + rr }, { r.origin.x, r.getMaxY() - rr }, border, borderColor);
                d->drawSegment({ r.getMaxX(), r.origin.y + rr }, { r.getMaxX(), r.getMaxY() - rr }, border, borderColor);
                // four quarter arcs, each one sweeping outwards from its own corner centre. The old
                // version had the bottom-right and top-left sweeps rotated into the box, which is
                // what drew those stray white hooks over the panel.
                constexpr int ARC = 8;
                struct Corner {
                    float cx, cy, from;
                };
                Corner arcs[4] = { { r.origin.x + rr, r.origin.y + rr, 180.f }, { r.getMaxX() - rr, r.origin.y + rr, 270.f },
                    { r.getMaxX() - rr, r.getMaxY() - rr, 0.f }, { r.origin.x + rr, r.getMaxY() - rr, 90.f } };
                for (auto const& a : arcs) {
                    CCPoint prev{};
                    for (int i = 0; i <= ARC; ++i) {
                        float ang = (a.from + 90.f * static_cast<float>(i) / ARC) * 3.14159265f / 180.f;
                        CCPoint cur{ a.cx + rr * std::cos(ang), a.cy + rr * std::sin(ang) };
                        if (i) {
                            d->drawSegment(prev, cur, border, borderColor);
                        }
                        prev = cur;
                    }
                }
            }
        }


        // a pixel heart, drawn from blocks: the monitor's icon in the reference art
        void pixelHeart(CCDrawNode* d, CCPoint at, float px, ccColor3B c) {
            // 9 wide x 8 tall block heart
            static char const* SHAPE[] = { ".XX...XX.",
                "XXXXXXXXX", "XXXXXXXXX", "XXXXXXXXX", ".XXXXXXX.", "..XXXXX..", "...XXX...", "....X...." };
            auto col = withAlpha(c, 1.f);
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 9; ++x) {
                    if (SHAPE[y][x] != 'X') {
                        continue;
                    }
                    CCPoint v[] = { { at.x + x * px, at.y + (7 - y) * px }, { at.x + (x + 1) * px, at.y + (7 - y) * px },
                        { at.x + (x + 1) * px, at.y + (8 - y) * px }, { at.x + x * px, at.y + (8 - y) * px } };
                    d->drawPolygon(v, 4, col, 0.f, { 0, 0, 0, 0 });
                }
            }
        }

        // twitch-style moderator sword / vip diamond / sub star, 8x8 blocks
        void pixelBadge(CCDrawNode* d, int kind, CCPoint at, float px) {
            ccColor3B c = kind == 1 ? ccColor3B { 0, 173, 3 } : kind == 2 ? ccColor3B { 224, 5, 185 }
                                                                        : kind == 3 ? ccColor3B { 130, 130, 255 }
                                                                                    : ccColor3B { 255, 190, 60 };
            auto col = withAlpha(c, 1.f);
            auto put = [&](int x, int y, int w = 1, int h = 1) {
                CCPoint v[] = { { at.x + x * px, at.y + y * px }, { at.x + (x + w) * px, at.y + y * px },
                    { at.x + (x + w) * px, at.y + (y + h) * px }, { at.x + x * px, at.y + (y + h) * px } };
                d->drawPolygon(v, 4, col, 0.f, { 0, 0, 0, 0 });
            };
            if (kind == 1) {
                // sword
                put(3, 1, 2, 5);
                put(2, 5, 4, 1);
                put(3, 6, 2, 1);
                put(2, 0, 1, 1);
                put(5, 0, 1, 1);
            }
            else if (kind == 2) {
                // diamond
                put(3, 0, 2, 1);
                put(2, 1, 4, 1);
                put(1, 2, 6, 1);
                put(2, 3, 4, 1);
                put(3, 4, 2, 1);
            }
            else if (kind == 3) {
                // star
                put(3, 0, 2, 2);
                put(1, 2, 6, 2);
                put(2, 4, 4, 2);
                put(2, 6, 1, 1);
                put(5, 6, 1, 1);
            }
            else {
                // founder: a little crown
                put(1, 2, 1, 3);
                put(3, 3, 1, 2);
                put(6, 2, 1, 3);
                put(1, 1, 6, 1);
            }
        }

        // =====================================================================
        // the overlay
        // =====================================================================

        struct LineView {
            std::string user;
            std::string body;
            ccColor3B color{ 255, 255, 255 };
            int badge = 0;
            std::string stamp;
            // wrapped once, when the line arrives (and again only if the panel is resized): a busy
            // chat must not re-measure its whole history for every new message
            std::vector<std::string> wrapped;
            float height = 0.f;
            float headWidth = 0.f;   // where the body starts on the first line
        };

        struct Row {
            CCNode* node = nullptr;
            CCLabelBMFont* stamp = nullptr;
            CCDrawNode* badge = nullptr;
            CCLabelBMFont* user = nullptr;
            CCLabelBMFont* body[3] = { nullptr, nullptr, nullptr };
            bool used = false;
            LineView view;      // kept so a row can be repaired without rebuilding the panel
        };

        struct Gift {
            std::string user;
            int amount = 5;
            std::string message;
            ccColor3B color{ 255, 255, 255 };
            int badge = 0;
            float t = 0.f;
            float life = 8.f;
            bool queued = true;
            // The wrapped message and the card height are worked out once, when the donation
            // starts, not on every frame of the alert: wrapText is a word loop and this was doing
            // it twice per frame plus once more for the height, for eight seconds, per donation.
            // A donation whose text never changes cannot need re-wrapping 480 times. The size is
            // part of the key because resizing the panel changes the wrap.
            std::string wrapKey;
            std::vector<std::string> wrapped;
            float cardH = 0.f;
        };

        struct Overlay {
            CCNode* root = nullptr;
            CCNode* chat = nullptr;
            CCDrawNode* chatBg = nullptr;
            CCLabelBMFont* headLabel = nullptr;
            CCNode* viewers = nullptr;
            CCDrawNode* viewersBg = nullptr;
            CCLabelBMFont* viewersLabel = nullptr;
            CCDrawNode* viewersIcon = nullptr;

            std::vector<Row> rows;
            size_t rowUsed = 0;

            CCNode* heart = nullptr;
            CCDrawNode* heartPill = nullptr;
            CCDrawNode* heartIcon = nullptr;
            CCDrawNode* ecg = nullptr;
            CCLabelBMFont* bpmLabel = nullptr;
            CCDrawNode* ecgHead = nullptr;

            CCNode* grip = nullptr;        // bottom-right corner: drag to resize
            CCDrawNode* gripDraw = nullptr;
            // the size / scale / corner / chat-visible the tree was last placed with
            struct LayoutSig {
                float w = -1.f, h = -1.f, scale = -1.f, pos = -1.f, chat = -1.f;
            } laidOutFor;

            CCNode* header = nullptr;             // optional "GdStream" wordmark above the panel
            CCLabelBMFont* headGd = nullptr;
            CCLabelBMFont* headStream = nullptr;
            CCNode* rowsHost = nullptr;   // parent of the row stack
            CCNode* chip = nullptr;        // small settings chip, shown while the menu is closed
            CCDrawNode* chipDraw = nullptr;
            std::function<void()> onChip = nullptr;

            CCNode* gift = nullptr;
            CCDrawNode* giftBg = nullptr;
            CCDrawNode* giftBar = nullptr;
            CCLabelBMFont* giftName = nullptr;
            CCLabelBMFont* giftAmount = nullptr;
            CCLabelBMFont* giftMsg[2] = { nullptr, nullptr };
            CCLabelBMFont* giftFoot = nullptr;

            std::vector<LineView> lines;
            std::deque<Gift> gifts;
            Gift activeGift;
            bool giftBusy = false;
            bool giftLeaving = false;
            float giftGap = 12.f;

            float lineH = 12.f;
            float bpmShown = 74.f;
            float bpmReal = 74.f;   // kept in step with bpmShown by heartSpike / the jump timer
            float bpmTimer = 2.f;
            float beatPhase = 0.f;
            float ecgT = 0.f;
            float viewerValue = 0.f;
            float viewerTarget = 0.f;
            float viewerNudge = 1.f;
            float viewerTimer = 4.f;
            float chatClock = 0.f;
            std::string lastViewers;
            float healTimer = 1.5f;
            float completeT = 0.f;   // seconds since the level was beaten
            std::string lastBpm;
            bool built = false;
        };

        Overlay O;

        // the level being played: chat and donations mention it when they can
        std::string gLevelName;

        std::string fillRun(std::string s);

        // ---- live gameplay state
        float gPct = 0.f;
        float gBest = 0.f;
        float gAttemptMax = 0.f;
        int gAttempts = 0;
        bool gComplete = false;
        float gCompleteT = 0.f;
        float gClock = 0.f;
        bool gDeadThisAttempt = false;

        // ---- scheduling
        float gChatTimer = 0.f;
        float gBurst = 0.f;
        float gBurstGap = 0.f;
        uint32_t gSeed = 0x51DE77;
        std::vector<int> gBag;
        size_t gBagPos = 0;
        int gBagCat = -1;
        float gNextGiftIn = 22.f;
        float gEventCooldown = 0.f;
        float gDeathCooldown = 0.f;
        bool gWelcomed = false;
        std::deque<std::pair<int, float>> gEvents;   // category to flush, delay
        // The GO wall: chat doesn't send one GO, it sends fifteen in a row. A wall is a count of
        // lines still to send plus the gap between them, so it reads as a wall and not as a
        // coincidentally dense second.
        int gWallLeft = 0;
        int gWallLines = 0;
        float gWallGap = 0.f;
        float gWallBase = 0.13f;    // the gap the wall was asked for: the slowest between-lines gap
        float gWallCooldown = 0.f;  // so a run of personal bests can't wall every percent

        // ---- dragging
        bool gDragging = false;
        bool gResizing = false;
        bool gMoved = false;
        CCPoint gGrabOffset{};
        CCPoint gResizeOrigin{};
        float gResizeW = 0.f;
        float gResizeH = 0.f;

        void ttsEnqueue(std::string const& user, int amount, std::string const& message, float delay);
        void buildOverlay();
        void layoutOverlay();
        float giftCardHeight(Gift const& g);
        void wrapGift(Gift& g);
        void addLine(LineView line);
        void relayoutRows();
        void rewrapAll();
        void pushEvent(int cat, int count, float spread = 0.55f);
        void startWall(int lines, float gap);
        void startWallSoft(int lines, float gap);

        // ---- hype weights: how the audience behaves at a given percentage
        struct Weights {
            float w[text::C_COUNT];
            // The GO wall is not a weighted category: it is sent as a run of consecutive lines
            // when a moment calls for it, so a random roll never has to land on it.
        };

        Weights weightsFor(float pct) {
            Weights o{};
            auto W = [&o](int c, float v) { o.w[c] = v; };
            if (pct >= 99.5f) {
                W(text::C_CELEBRATE, 52.f); W(text::C_SPAM, 16.f); W(text::C_EMOTE, 12.f);
                W(text::C_REACT, 10.f); W(text::C_PANIC, 8.f); W(text::C_NORMAL, 2.f);
            }
            else if (pct >= 95.f) {
                W(text::C_PANIC, 34.f); W(text::C_SPAM, 24.f); W(text::C_HYPE, 14.f);
                W(text::C_REACT, 12.f); W(text::C_EMOTE, 10.f); W(text::C_PREDICT, 4.f); W(text::C_NORMAL, 2.f);
            }
            else if (pct >= 85.f) {
                W(text::C_PANIC, 26.f); W(text::C_HYPE, 24.f); W(text::C_SPAM, 18.f);
                W(text::C_REACT, 14.f); W(text::C_PREDICT, 10.f); W(text::C_EMOTE, 6.f); W(text::C_NORMAL, 2.f);
            }
            else if (pct >= 70.f) {
                W(text::C_HYPE, 30.f); W(text::C_REACT, 20.f); W(text::C_PANIC, 14.f);
                W(text::C_SPAM, 14.f); W(text::C_PREDICT, 12.f); W(text::C_EMOTE, 6.f); W(text::C_NORMAL, 4.f);
            }
            else if (pct >= 50.f) {
                W(text::C_HYPE, 26.f); W(text::C_REACT, 24.f); W(text::C_NORMAL, 14.f);
                W(text::C_PREDICT, 14.f); W(text::C_SPAM, 10.f); W(text::C_EMOTE, 8.f); W(text::C_PANIC, 4.f);
            }
            else if (pct >= 30.f) {
                W(text::C_NORMAL, 26.f); W(text::C_REACT, 26.f); W(text::C_HYPE, 16.f);
                W(text::C_PREDICT, 14.f); W(text::C_EMOTE, 10.f); W(text::C_SPAM, 6.f); W(text::C_PANIC, 2.f);
            }
            else if (pct >= 10.f) {
                W(text::C_NORMAL, 36.f); W(text::C_REACT, 22.f); W(text::C_PREDICT, 16.f);
                W(text::C_EMOTE, 12.f); W(text::C_HYPE, 10.f); W(text::C_SPAM, 4.f);
            }
            else {
                W(text::C_NORMAL, 48.f); W(text::C_REACT, 18.f); W(text::C_PREDICT, 14.f);
                W(text::C_EMOTE, 14.f); W(text::C_HYPE, 6.f);
            }
            // talking about the level itself happens most in the calm stretches
            if (!gLevelName.empty()) {
                W(text::C_LEVEL, pct < 50.f ? 10.f : 4.f);
            }
            return o;
        }

        int pickCategory(uint32_t& s, float pct) {
            auto w = weightsFor(pct);
            float total = 0.f;
            for (int i = 0; i < text::C_COUNT; ++i) {
                total += w.w[i];
            }
            float roll = rndf(s) * total;
            for (int i = 0; i < text::C_COUNT; ++i) {
                roll -= w.w[i];
                if (roll <= 0.f) {
                    return i;
                }
            }
            return text::C_NORMAL;
        }

        // the bag: every line of a category is used once before any repeats
        // The panel is only ~300px wide by default, which fits roughly 40 characters on the body
        // line. In a narrow panel the pool is filtered to lines that fit, so messages are short
        // instead of being cut with an ellipsis. A wide panel keeps the whole pool.
        bool lineFits(std::string const& body) {
            // Two measurements, and the larger wins: the font probe, and a plain character-count
            // estimate (~6.6px per character at this scale, generous on purpose). If the probe ever
            // measures something odd, the estimate still keeps sane lines in the pool.
            float panelW = O.chat ? O.chat->getContentSize().width : 300.f;
            float budget = std::max(60.f, panelW - 150.f);
            float measured = textW(body, 0.62f, "chatFont.fnt");
            float estimated = static_cast<float>(body.size()) * 6.6f;
            return std::max(measured, estimated) <= budget;
        }

        // the short-line index, built once per category per panel width (measuring every candidate
        // with the font on every category change would be a stutter)
        std::vector<int> gShortCache[text::C_COUNT];
        int gShortWidth = -1;

        void buildShortCache(int cat) {
            auto const& t = text::chat();
            gShortCache[cat].clear();
            int n = t.count(cat);
            for (int i = 0; i < n; ++i) {
                int idx = t.first(cat) + i;
                if (lineFits(fillRun(t.at(idx)))) {
                    gShortCache[cat].push_back(idx);
                }
            }
        }

        int nextLine(uint32_t& s, int cat) {
            auto const& t = text::chat();
            int n = t.count(cat);
            if (n <= 0) {
                return 0;
            }
            if (cat != gBagCat || gBagPos >= gBag.size()) {
                // A 300px panel fits ~28 characters of body text behind a name. Short lines are
                // preferred there, but the filter can only ever *narrow* the pool: if it comes back
                // with nothing (a narrow panel, a wide panel whose font measured oddly, a category
                // with nothing short in it) the whole category is used. The chat must never go
                // blank, so there is no path here that can leave the bag empty.
                float panelW = O.chat ? O.chat->getContentSize().width : 0.f;
                // The GO wall is exempt: its whole character is the long stretched line, and a
                // "GO" that has been filtered down to fit is not a wall.
                bool narrow = panelW >= 160.f && panelW < 420.f && cat != text::C_GO;
                int widthKey = static_cast<int>(panelW);
                if (widthKey != gShortWidth) {
                    for (auto& c : gShortCache) {
                        c.clear();
                    }
                    gShortWidth = widthKey;
                }
                gBag.assign(static_cast<size_t>(n), 0);
                for (int i = 0; i < n; ++i) {
                    gBag[static_cast<size_t>(i)] = t.first(cat) + i;
                }
                if (narrow) {
                    if (gShortCache[cat].empty()) {
                        buildShortCache(cat);
                    }
                    if (gShortCache[cat].size() >= 4) {
                        gBag = gShortCache[cat];
                    }
                }
                for (size_t i = gBag.size(); i > 1; --i) {
                    std::swap(gBag[i - 1], gBag[rnd32(s) % i]);
                }
                gBagPos = 0;
                gBagCat = cat;
            }
            if (gBag.empty()) {
                return 0;   // can only happen if the category itself is empty
            }
            return gBag[std::min(gBagPos++, gBag.size() - 1)];
        }

        // $P = percentage, $A = attempt, $L = the level's name (nothing if it isn't known)
        std::string fillRun(std::string s) {
            auto sub = [&s](char const* from, std::string const& to) {
                size_t p = 0;
                while ((p = s.find(from, p)) != std::string::npos) {
                    s.replace(p, std::strlen(from), to);
                    p += to.size();
                }
            };
            sub("$P", std::to_string(std::clamp(static_cast<int>(std::lround(gPct)), 1, 99)));
            sub("$A", std::to_string(std::max(1, gAttempts)));
            sub("$L", gLevelName);
            return s;
        }

        std::string clockStamp() {
            // Twitch shows the local clock time next to each line
            std::time_t now = std::time(nullptr);
            std::tm tmv{};
#ifdef GEODE_IS_WINDOWS
            localtime_s(&tmv, &now);
#else
            localtime_r(&now, &tmv);
#endif
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d:%02d", tmv.tm_hour, tmv.tm_min);
            return buf;
        }

        // ---- how a bot writes: some are calm, some are all caps, some stretch words out
        std::string styleOf(std::string text, Bot const& b, uint32_t& s) {
            if (b.custom) {
                return text;   // user-written lines are shown exactly as typed
            }
            bool shortLine = text.size() < 26;
            switch (b.style) {
                case 1: {
                    // all caps
                    for (auto& c : text) {
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    }
                    break;
                }
                case 2: {
                    // stretched: the last word's vowel run grows
                    if (!text.empty() && text.back() != '!' && rndf(s) < 0.5f) {
                        size_t i = text.find_last_of("aeoAEO");
                        if (i != std::string::npos) {
                            text.insert(i + 1, std::string(2 + rnd32(s) % 4, text[i]));
                        }
                    }
                    break;
                }
                case 3: {
                    // emotes on the end
                    if (rndf(s) < 0.6f) {
                        static char const* E[] = { " KEKW", " LULW", " Pog", " 💀x2", " Prayge", " catJAM", " monkaS" };
                        text += E[rnd32(s) % 7u];
                    }
                    break;
                }
                case 4: {
                    // one-word shouters
                    if (shortLine && rndf(s) < 0.4f) {
                        for (auto& c : text) {
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        }
                        text += "!!";
                    }
                    break;
                }
                default: break;
            }
            // the hyped bots shout when it matters
            if (b.hype > 70 && gPct > 70.f && rndf(s) < 0.45f) {
                for (auto& c : text) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
            }
            return text;
        }

        // =====================================================================
        // building the node tree (once per level, then reused)
        // =====================================================================

        CCNode* newNode(CCNode* parent, int z = 0) {
            auto* n = CCNode::create();
            parent->addChild(n, z);
            return n;
        }

        void buildOverlay() {
            if (O.built) {
                return;
            }
            O.built = true;
            auto* root = CCNode::create();
            root->retain();   // owned by the overlay; released in resetOverlay
            O.root = root;

            // ---------------- chat panel ----------------
            O.chat = newNode(root, 10);
            O.chatBg = CCDrawNode::create();
            O.chat->addChild(O.chatBg, 0);

            O.headLabel = CCLabelBMFont::create("STREAM CHAT", "chatFont.fnt");
            O.headLabel->setAnchorPoint({ 0.f, 0.5f });
            O.headLabel->setScale(0.6f);
            O.chat->addChild(O.headLabel, 1);

            // viewer pill: an eye/dot + "12,450"
            O.viewers = newNode(root, 12);
            O.viewersBg = CCDrawNode::create();
            O.viewers->addChild(O.viewersBg, 0);
            O.viewersIcon = CCDrawNode::create();
            O.viewers->addChild(O.viewersIcon, 1);
            O.viewersLabel = CCLabelBMFont::create("0", "chatFont.fnt");
            O.viewersLabel->setAnchorPoint({ 0.f, 0.5f });
            O.viewersLabel->setScale(0.6f);
            O.viewers->addChild(O.viewersLabel, 1);

            // ---------------- heart monitor ----------------
            O.heart = newNode(root, 12);
            O.ecg = CCDrawNode::create();
            O.heart->addChild(O.ecg, 0);
            O.heartPill = CCDrawNode::create();
            O.heart->addChild(O.heartPill, 1);
            O.heartIcon = CCDrawNode::create();
            O.heart->addChild(O.heartIcon, 2);
            O.bpmLabel = CCLabelBMFont::create("74", "goldFont.fnt");
            O.bpmLabel->setAnchorPoint({ 0.f, 0.5f });
            O.bpmLabel->setScale(0.62f);
            O.bpmLabel->setColor({ 255, 46, 46 });
            O.heart->addChild(O.bpmLabel, 2);
            O.ecgHead = CCDrawNode::create();
            O.heart->addChild(O.ecgHead, 1);

            // ---------------- donation alert ----------------
            O.gift = newNode(root, 20);
            O.gift->setVisible(false);
            O.giftBg = CCDrawNode::create();
            O.gift->addChild(O.giftBg, 0);
            O.giftBar = CCDrawNode::create();
            O.gift->addChild(O.giftBar, 1);
            O.giftName = CCLabelBMFont::create("", "chatFont.fnt");
            O.giftName->setAnchorPoint({ 0.f, 0.5f });
            O.gift->addChild(O.giftName, 2);
            O.giftAmount = CCLabelBMFont::create("", "goldFont.fnt");
            O.giftAmount->setAnchorPoint({ 0.f, 0.5f });
            O.gift->addChild(O.giftAmount, 2);
            O.giftMsg[0] = CCLabelBMFont::create("", "chatFont.fnt");
            O.giftMsg[1] = CCLabelBMFont::create("", "chatFont.fnt");
            for (auto* l : O.giftMsg) {
                l->setAnchorPoint({ 0.f, 0.5f });
                O.gift->addChild(l, 2);
            }
            O.giftFoot = CCLabelBMFont::create("", "chatFont.fnt");
            O.giftFoot->setAnchorPoint({ 0.f, 0.5f });
            O.giftFoot->setScale(0.5f);
            O.gift->addChild(O.giftFoot, 2);

            // ---------------- resize grip + settings chip ----------------
            O.grip = newNode(root, 15);
            O.gripDraw = CCDrawNode::create();
            O.grip->addChild(O.gripDraw, 0);
            // The corner handle is still there for resizing, but it draws nothing: a chat overlay
            // with three white dots in the corner looks like a UI mock, not a stream.
            O.grip->setContentSize({ 16.f, 16.f });
            O.chip = newNode(root, 40);
            O.chipDraw = CCDrawNode::create();
            O.chip->addChild(O.chipDraw, 0);
            O.chip->setContentSize({ 40.f, 14.f });
            roundRect(O.chipDraw, { 0.f, 0.f, 40.f, 14.f }, 7.f, { 0.04f, 0.02f, 0.07f, 0.55f }, 0.f);
            {
                auto* l = CCLabelBMFont::create("menu", "chatFont.fnt");
                l->setScale(0.36f);
                l->setColor({ 200, 200, 215 });
                l->setPosition({ 20.f, 7.f });
                O.chip->addChild(l, 1);
            }

            // ---------------- optional wordmark above the panel ----------------
            // Built on demand by buildHeader(), which needs the mod's resource path. Nothing is
            // created here.

            // ---------------- chat rows ----------------
            // No clipping node here on purpose. cocos2d-x draws a stencil with the modelview matrix
            // loaded to identity, so a stencil built in the panel's local space ends up cutting at
            // the bottom-left corner of the *screen*: every row got clipped away and the panel went
            // blank. Rows are cut by measuring the text instead (see wrapLine), which cannot fail
            // this way.
            O.rowsHost = newNode(O.chat, 1);
            // 110 rows is more than a 900px panel can show at the smallest font, so a resize
            // never runs out
            O.rows.resize(110);
            for (auto& r : O.rows) {
                r.node = newNode(O.rowsHost, 1);
                r.node->setVisible(false);
                r.stamp = CCLabelBMFont::create("", "chatFont.fnt");
                r.stamp->setAnchorPoint({ 0.f, 0.5f });
                r.stamp->setScale(0.52f);
                r.node->addChild(r.stamp, 0);
                r.badge = CCDrawNode::create();
                r.node->addChild(r.badge, 0);
                r.user = CCLabelBMFont::create("", "chatFont.fnt");
                r.user->setAnchorPoint({ 0.f, 0.5f });
                r.node->addChild(r.user, 0);
                for (auto& b : r.body) {
                    b = CCLabelBMFont::create("", "chatFont.fnt");
                    b->setAnchorPoint({ 0.f, 0.5f });
                    r.node->addChild(b, 0);
                }
            }

            // measure the row font once
            O.lineH = lineHeight(0.62f, "chatFont.fnt");
            if (O.lineH < 6.f) {
                O.lineH = 12.f;
            }
            // place everything now: the first frame of a level should already look right
            layoutOverlay();
            O.laidOutFor = { S.width, S.height, S.scale, static_cast<float>(S.pos), S.chat ? 1.f : 0.f };
        }

        // =====================================================================
        // layout
        // =====================================================================

        CCRect panelRect() {
            auto win = CCDirector::get()->getWinSize();
            float w = S.width * S.scale;
            float h = std::min(S.height * S.scale, std::max(60.f, win.height - 16.f));
            w = std::min(w, std::max(60.f, win.width - 16.f));
            float m = 12.f;
            float x = 0.f, y = 0.f;
            switch (S.pos) {
                case 0: x = m; y = win.height - h - m; break;
                case 1: x = win.width - w - m; y = win.height - h - m; break;
                case 2: x = m; y = m; break;
                case 3: x = win.width - w - m; y = m; break;
                default:
                    x = S.cx * win.width - w / 2.f;
                    y = (1.f - S.cy) * win.height - h / 2.f;
                    break;
            }
            x = std::clamp(x, 2.f, std::max(2.f, win.width - w - 2.f));
            // when the wordmark is on, the panel leaves it room instead of pushing it off screen
            float topPad = S.header && S.chat ? 26.f : 2.f;
            y = std::clamp(y, 2.f, std::max(2.f, win.height - h - topPad));
            return { x, y, w, h };
        }

        // =====================================================================
        // the "GdStream" wordmark
        // =====================================================================
        // PUSAB is the typeface Geometry Dash uses, so it is shipped with the mod and loaded through
        // the resource path. If that ever fails on a platform or build, the game's own bitmap font
        // is used instead: the title degrades to a different font, never to no title.
        std::string gHeaderFont;
        bool gHeaderFontBad = false;

        char const* headerFont() {
            if (gHeaderFontBad) {
                return "bigFont.fnt";
            }
            if (gHeaderFont.empty()) {
                std::string p = (Mod::get()->getResourcesDir() / "pusab.fnt").string();
                if (CCFileUtils::sharedFileUtils()->isFileExist(p)) {
                    gHeaderFont = p;
                }
                else {
                    gHeaderFontBad = true;
                    gHeaderFont = "bigFont.fnt";
                }
            }
            return gHeaderFont.c_str();
        }

        void buildHeader() {
            if (O.header || !O.root) {
                return;
            }
            O.header = newNode(O.root, 14);
            O.headGd = CCLabelBMFont::create("Gd", headerFont());
            O.headGd->setAnchorPoint({ 0.f, 0.5f });
            O.headGd->setColor({ 88, 214, 92 });
            O.header->addChild(O.headGd, 0);
            O.headStream = CCLabelBMFont::create("Stream", headerFont());
            O.headStream->setAnchorPoint({ 0.f, 0.5f });
            O.headStream->setColor({ 236, 186, 62 });
            O.header->addChild(O.headStream, 0);
        }

        // the monitor is the trace + the pill, or just the pill when the trace is turned off
        float heartElemW() {
            return (S.ecg ? 164.f : 92.f) * S.scale;
        }

        void layoutOverlay() {
            if (!O.built) {
                return;
            }
            auto win = CCDirector::get()->getWinSize();
            auto r = panelRect();
            bool chatVisible = S.chat;
            O.chat->setVisible(chatVisible);
            O.chat->setPosition(r.origin);
            O.chat->setContentSize(r.size);

            // no header strip: rows start at the very top, only the viewer pill needs a little room
            float headH = S.viewers ? 19.f : 4.f;
            if (O.headLabel) {
                O.headLabel->setVisible(false);   // a real chat overlay has no title bar
            }

            // The panel is a single sheet of glass. There used to be a header band, a hairline
            // under it and a 1px inset frame on top of this, which read as boxes inside boxes and
            // ate the top of the panel. One plate, nothing else.
            O.chatBg->clear();
            if (chatVisible) {
                float a = std::min(0.46f, 0.06f + 0.34f * S.opacity);
                roundRect(O.chatBg, { 0.f, 0.f, r.size.width, r.size.height }, 6.f,
                    { 0.035f, 0.020f, 0.062f, a }, 0.f);
            }

            // viewer pill, top-right of the panel (or alone in the corner when chat is off)
            if (O.viewers) {
                O.viewers->setVisible(S.viewers);
                if (S.viewers) {
                    float pw = 62.f, ph = 14.f;
                    O.viewers->setContentSize({ pw, ph });
                    if (chatVisible) {
                        O.viewers->setPosition({ r.getMaxX() - pw - 5.f, r.getMaxY() - ph - 4.f });
                    }
                    else {
                        auto corner = S.pos == 0 || S.pos == 2 ? CCPoint { 12.f, win.height - ph - 12.f }
                                                              : CCPoint { win.width - pw - 12.f, win.height - ph - 12.f };
                        O.viewers->setPosition(corner);
                    }
                    O.viewersBg->clear();
                    roundRect(O.viewersBg, { 0.f, 0.f, pw, ph }, ph / 2.f, { 0.06f, 0.04f, 0.10f, 0.72f }, 0.f);
                    O.viewersIcon->clear();
                    // a tiny red live dot
                    circleFill(O.viewersIcon, { 7.f, ph / 2.f }, 2.2f, { 0.92f, 0.15f, 0.22f, 1.f }, 10);
                    O.viewersLabel->setPosition({ 12.5f, ph / 2.f });
                }
            }

            // The monitor lives on the right edge, just under the progress bar, and the alert under
            // it. Both stay glued to the right edge: they only slide vertically to keep clear of the
            // chat panel (and only cross to its left if the whole edge is taken).
            auto hits = [&](float x, float y, float w, float h) {
                return chatVisible && x < r.getMaxX() + 6.f && x + w > r.origin.x - 6.f && y < r.getMaxY() + 6.f &&
                    y + h > r.origin.y - 6.f;
            };
            float heartW = heartElemW();   // trace + pill; the trace needs most of it
            float heartH = 32.f * S.scale;
            float heartX = win.width - heartW - 14.f;
            float heartY = win.height - heartH - 56.f;   // clear of the progress bar
            if (hits(heartX, heartY, heartW, heartH)) {
                float below = r.getMinY() - heartH - 10.f;
                float above = r.getMaxY() + 10.f;
                if (below >= 6.f) {
                    heartY = below;                       // under the panel
                }
                else if (above + heartH <= win.height - 6.f) {
                    heartY = above;                       // above it
                }
                else {
                    heartX = r.origin.x - heartW - 10.f;  // no room either way: beside it
                }
            }
            heartY = std::clamp(heartY, 6.f, std::max(6.f, win.height - heartH - 6.f));
            heartX = std::clamp(heartX, 6.f, std::max(6.f, win.width - heartW - 6.f));
            if (O.heart) {
                O.heart->setVisible(S.heart);
                if (S.heart) {
                    O.heart->setPosition({ heartX, heartY });
                    O.heart->setContentSize({ heartW, heartH });
                }
            }

            // donation alert: the same right edge, under the monitor
            if (O.gift) {
                float gw = 260.f * S.scale;
                if (O.giftBusy) {
                    wrapGift(O.activeGift);   // the height below reads the cached wrap
                }
                float gh = O.giftBusy ? giftCardHeight(O.activeGift) : 44.f * S.scale;
                float gx = win.width - gw - 14.f;
                float gy = heartY - gh - 22.f;
                if (hits(gx, gy, gw, gh)) {
                    float below = r.getMinY() - gh - 10.f;
                    if (below >= 6.f) {
                        gy = below;
                    }
                    else if (r.getMaxY() + 10.f + gh <= win.height - 6.f) {
                        gy = r.getMaxY() + 10.f;
                    }
                    else {
                        gx = r.origin.x - gw - 10.f;
                    }
                }
                gy = std::clamp(gy, 6.f, std::max(6.f, win.height - gh - 6.f));
                gx = std::clamp(gx, 6.f, std::max(6.f, win.width - gw - 6.f));
                O.gift->setContentSize({ gw, gh });
                O.gift->setPosition({ gx, gy });
            }

            // the wordmark sits just above the panel's top-left; the panel always leaves room for it
            bool showHead = S.header && chatVisible;
            if (showHead) {
                buildHeader();   // first time the title is switched on
            }
            if (O.header) {
                O.header->setVisible(showHead);
                if (showHead) {
                    // PUSAB is the font Geometry Dash itself uses; the size is its own setting
                    float hsc = S.headScale;
                    O.headGd->setScale(hsc);
                    O.headStream->setScale(hsc);
                    float hh = lineHeight(hsc, headerFont());
                    float wGd = textW("Gd", hsc, headerFont());
                    O.headGd->setPosition({ 0.f, hh * 0.5f });
                    O.headStream->setPosition({ wGd, hh * 0.5f });
                    O.header->setContentSize({ wGd + textW("Stream", hsc, headerFont()), hh });
                    O.header->setPosition({ r.getMinX() + 2.f, r.getMaxY() + 3.f });
                }
            }

            if (O.grip) {
                O.grip->setVisible(chatVisible);
                O.grip->setPosition({ r.getMaxX() - 17.f, r.getMinY() + 2.f });
            }
            if (O.chip) {
                O.chip->setVisible(S.chip && gChipRuntime && chatVisible);
                // directly under the chat panel when there is room, otherwise above it, so it never
                // sits underneath the panel (z 40 is above the chat's z 10)
                float cw = 40.f;
                float chh = 14.f;
                float below = r.getMinY() - chh - 4.f;
                float above = r.getMaxY() + 4.f;
                float x = std::clamp(r.origin.x, 8.f, std::max(8.f, win.width - cw - 8.f));
                float y = below >= 6.f ? below : (above + chh <= win.height - 6.f ? above : r.getMinY() + 4.f);
                O.chip->setPosition({ x, y });
            }

            rewrapAll();
            relayoutRows();
        }

        // =====================================================================
        // chat rows
        // =====================================================================

        void paintRow(Row& r, LineView const& v, float width) {
            float scale = 0.62f;
            float y = O.lineH * 0.5f;
            float x = 6.f;
            if (S.showTime) {
                r.stamp->setString(v.stamp.c_str());
                r.stamp->setColor({ 150, 150, 165 });
                r.stamp->setPosition({ x, y });
                x += textW(v.stamp, 0.52f, "chatFont.fnt") + 4.f;
            }
            else {
                r.stamp->setString("");
            }
            r.badge->clear();
            if (v.badge > 0) {
                pixelBadge(r.badge, v.badge, { x, y - 3.2f }, 0.9f);
                x += 9.f;
            }
            r.user->setString((v.user + ":").c_str());
            r.user->setScale(scale);
            r.user->setColor(v.color);
            r.user->setPosition({ x, y });
            x += textW(v.user + ":", scale, "chatFont.fnt") + 3.5f;

            for (int i = 0; i < 3; ++i) {
                auto* l = r.body[i];
                if (i == 0) {
                    l->setString(v.wrapped.empty() ? v.body.c_str() : v.wrapped[0].c_str());
                    l->setScale(scale);
                    l->setColor({ 226, 226, 236 });
                    l->setPosition({ x, y });
                    l->setVisible(true);
                }
                else {
                    l->setVisible(false);
                }
            }
            r.used = true;
            r.view = v;
            r.node->setVisible(true);
        }

        // stacks the newest line at the bottom, oldest scroll away at the top
        // Some label writes can land as an empty glyph run (a font bitmap used up, a string set while
        // the node was hidden, a re-upload after the overlay was re-parented). This checks the rows
        // that are on screen and repaints one that isn't showing its text. It runs once a second and
        // only ever touches a row that is wrong, so it costs nothing when everything is fine.
        void healRows(float dt) {
            if (!O.built || !S.chat || O.rows.empty() || O.lines.empty()) {
                return;
            }
            O.healTimer -= dt;
            if (O.healTimer > 0.f) {
                return;
            }
            O.healTimer = 1.f;
            for (auto& r : O.rows) {
                if (!r.used || !r.node || !r.node->isVisible()) {
                    continue;
                }
                // getString() is a plain char const*, not a std::string, in this SDK
                auto hasText = [](CCLabelBMFont* l) {
                    char const* str = l ? l->getString() : nullptr;
                    return l && str && *str != '\0';
                };
                bool bodyBad = false;
                for (int i = 0; i < 3; ++i) {
                    bool shouldShow = i < static_cast<int>(r.view.wrapped.size());
                    bool shows = r.body[i] && r.body[i]->isVisible() && hasText(r.body[i]);
                    if (shouldShow && !shows && !r.view.wrapped[static_cast<size_t>(i)].empty()) {
                        bodyBad = true;
                    }
                }
                bool userBad = r.user && !hasText(r.user);
                if (bodyBad || userBad) {
                    paintRow(r, r.view, O.chat->getContentSize().width);
                }
            }
        }

        void relayoutRows() {
            if (!O.built || !S.chat) {
                return;
            }
            float width = O.chat->getContentSize().width;
            float headH = S.viewers ? 19.f : 4.f;
            float top = O.chat->getContentSize().height - headH - 2.f;
            float bottom = 4.f;
            float usable = top - bottom;
            float scale = 0.62f;

            for (auto& r : O.rows) {
                r.node->setVisible(false);
                r.used = false;
            }
            size_t ri = 0;
            float y = bottom;
            for (size_t k = O.lines.size(); k > 0 && ri < O.rows.size(); --k) {
                auto const& v = O.lines[k - 1];
                if (v.wrapped.empty()) {
                    continue;
                }
                if (y + v.height > usable + 1.f) {
                    break;
                }
                auto& r = O.rows[ri++];
                paintRow(r, v, width);
                r.node->setPosition({ 0.f, y });
                // the body under the first line, if it wrapped
                float lh = O.lineH;
                for (int i = 1; i < 3; ++i) {
                    if (i < static_cast<int>(v.wrapped.size())) {
                        r.body[i]->setString(v.wrapped[static_cast<size_t>(i)].c_str());
                        r.body[i]->setScale(scale);
                        r.body[i]->setColor({ 226, 226, 236 });
                        r.body[i]->setPosition({ v.headWidth, lh * 0.5f + static_cast<float>(i) * lh });
                        r.body[i]->setVisible(true);
                    }
                    else {
                        r.body[i]->setVisible(false);
                    }
                }
                y += v.height;
            }
        }

        // wraps one line for the current panel width and caches the result on it
        // cuts a string down until it fits maxW, appending "..." when anything was dropped
        std::string truncateToWidth(std::string const& in, float maxW, float scale, char const* fnt) {
            if (in.empty() || textW(in, scale, fnt) <= maxW) {
                return in;
            }
            std::string cut = in;
            for (int i = 0; i < 200 && !cut.empty(); ++i) {
                size_t sp = cut.find_last_of(' ');
                if (sp == std::string::npos || sp < 8) {
                    break;
                }
                cut = cut.substr(0, sp);
                if (textW(cut + "...", scale, fnt) <= maxW) {
                    return cut + "...";
                }
            }
            // one enormous word: cut characters instead
            while (cut.size() > 4 && textW(cut + "...", scale, fnt) > maxW) {
                cut.pop_back();
            }
            return cut + "...";
        }

        void wrapLine(LineView& v, float panelWidth) {
            float scale = 0.62f;
            float x = 6.f;
            if (S.showTime) {
                x += textW(v.stamp, 0.52f, "chatFont.fnt") + 4.f;
            }
            if (v.badge > 0) {
                x += 9.f;
            }
            x += textW(v.user + ":", scale, "chatFont.fnt") + 3.5f;
            v.headWidth = x;
            float avail = std::max(40.f, panelWidth - x - 6.f);
            float linesH = 0.f;
            v.wrapped = wrapText(v.body, scale, "chatFont.fnt", avail, &linesH);
            if (v.wrapped.size() > 3) {
                v.wrapped.resize(3);
            }
            // nothing may ever be wider than the space it has: the last line is cut if a single
            // word ran past the edge, and the first line too when there is only one
            for (auto& line : v.wrapped) {
                line = truncateToWidth(line, avail, scale, "chatFont.fnt");
            }
            v.height = O.lineH * static_cast<float>(std::max<size_t>(v.wrapped.size(), 1)) + 1.f;
        }

        void rewrapAll() {
            if (!O.chat) {
                return;
            }
            float width = O.chat->getContentSize().width;
            for (auto& v : O.lines) {
                wrapLine(v, width);
            }
        }

        void addLine(LineView line) {
            if (O.chat) {
                wrapLine(line, O.chat->getContentSize().width);
            }
            O.lines.push_back(std::move(line));
            // keep at most what the panel can possibly show
            size_t cap = O.rows.size();
            if (O.lines.size() > cap) {
                O.lines.erase(O.lines.begin(), O.lines.begin() + static_cast<long>(O.lines.size() - cap));
            }
            relayoutRows();
        }

        // =====================================================================
        // chat generation
        // =====================================================================

        int activeBotCountInternal() { return static_cast<int>(gActive.size()); }

        Bot const* pickBot(uint32_t& s) {
            if (gActive.empty()) {
                return nullptr;
            }
            int idx = gActive[rnd32(s) % gActive.size()];
            if (idx < 0 || idx >= static_cast<int>(gBots.size())) {
                return nullptr;
            }
            return &gBots[static_cast<size_t>(idx)];
        }

        // a custom bot that has its own messages uses one of those instead of the database
        bool customLine(Bot const& b, std::string& out, uint32_t& s) {
            if (b.lines.empty()) {
                return false;
            }
            out = b.lines[rnd32(s) % b.lines.size()];
            return true;
        }

        void emitOne(int category, uint32_t& s) {
            Bot const* b = pickBot(s);
            if (!b) {
                return;
            }
            LineView v;
            v.user = b->name;
            v.color = paletteColor(b->color);
            v.badge = b->badge;
            v.stamp = clockStamp();
            std::string text;
            if (!customLine(*b, text, s)) {
                int cat = category < 0 ? pickCategory(s, gPct) : category;
                text = text::chat().at(nextLine(s, cat));
            }
            v.body = styleOf(fillRun(text), *b, s);
            addLine(std::move(v));
        }

        void pushEvent(int cat, int count, float spread) {
            for (int i = 0; i < count; ++i) {
                gEvents.emplace_back(cat, rndf(gSeed) * spread);
            }
        }

        // Starts a run of GO lines. The wall takes over the feed for its length: the ordinary
        // chat clock is held back while it runs, which is exactly what a real GD chat does at a
        // new best or at the last few percent.
        void startWall(int lines, float gap) {
            if (!S.chat || lines <= 0) {
                return;
            }
            // never bury a wall that is already going: extend it instead
            gWallLines = std::max(gWallLines, lines);
            gWallLeft = std::max(gWallLeft, lines);
            gWallBase = std::clamp(gap, 0.03f, 0.4f);
            gWallGap = std::min(gWallGap > 0.f ? gWallGap : gWallBase, gWallBase);
            gWallCooldown = 2.2f + static_cast<float>(lines) * 0.09f;
        }

        // a wall for a moment that may come round often (a new best every percent): allowed maybe
        // once every few seconds, and never stacked on top of a wall that is still running
        void startWallSoft(int lines, float gap) {
            if (gWallLeft > 0 || gWallCooldown > 0.f) {
                return;
            }
            startWall(lines, gap);
        }

        // messages/second at the current percentage: the user's setting is the rate for a calm
        // mid-run moment, and the exciting parts push it up
        float currentRate() {
            float base = S.speed;
            float hype = std::clamp(gPct, 0.f, 100.f) / 100.f;
            float mul = 0.55f + hype * 1.5f;
            if (gComplete) {
                mul = 2.6f;
            }
            return std::max(0.05f, base * mul);
        }

        // =====================================================================
        // heart monitor
        // =====================================================================

        int targetBpm(float pct) {
            struct P {
                float pct;
                float bpm;
            };
            static P T[] = { { 0.f, 78.f }, { 10.f, 95.f }, { 20.f, 115.f }, { 30.f, 130.f }, { 40.f, 145.f },
                { 50.f, 160.f }, { 60.f, 170.f }, { 70.f, 185.f }, { 80.f, 195.f }, { 90.f, 202.f }, { 100.f, 209.f } };
            float p = std::clamp(pct, 0.f, 100.f);
            for (size_t i = 1; i < std::size(T); ++i) {
                if (p <= T[i].pct) {
                    float t = (p - T[i - 1].pct) / (T[i].pct - T[i - 1].pct);
                    return static_cast<int>(T[i - 1].bpm + (T[i].bpm - T[i - 1].bpm) * t);
                }
            }
            return 209;
        }

        void updateHeart(float dt) {
            if (!O.heart || !S.heart) {
                return;
            }
            // A finished run is a relief: the target walks down from the ceiling to a calm rate over
            // about twenty seconds instead of sitting at 200+.
            int target = targetBpm(gPct);
            if (gComplete) {
                float t = std::clamp(O.completeT / 22.f, 0.f, 1.f);
                target = static_cast<int>(targetBpm(0.f) + (target - targetBpm(0.f)) * (1.f - t * 0.92f));
            }

            // irregular jumps of 3-7 bpm, with a wandering delay. Never 1 at a time, never a jump
            // from the resting rate to the ceiling.
            // The readout is an integer that only ever changes by a whole jump, exactly like a real
            // monitor: 150 then 155, never 151/152/153/154. It used to ease toward a sampled float
            // every frame, which printed every number in between.
            O.bpmTimer -= dt;
            if (O.bpmTimer <= 0.f) {
                O.bpmTimer = 0.75f + rndf(gSeed) * 1.6f;
                int shown = static_cast<int>(std::lround(O.bpmShown));
                int diff = target - shown;
                int mag = rndInt(gSeed, 3, 7);
                int next = 0;
                if (std::abs(diff) < 4) {
                    // hovering at the target: a small wander, still a jump of 3+
                    int dir = rndf(gSeed) < 0.5f ? -1 : 1;
                    next = shown + dir * mag;
                }
                else {
                    int dir = diff > 0 ? 1 : -1;
                    // a long way to go (a restart) closes in bigger whole jumps
                    if (std::abs(diff) > 25 && rndf(gSeed) < 0.35f) {
                        mag = rndInt(gSeed, 9, 14);
                    }
                    next = shown + dir * std::min(mag, std::abs(diff));
                }
                next = std::clamp(next, 62, 216);
                // the step must never be 1 or 2, even when the clamp pulled it in
                if (std::abs(next - shown) < 3) {
                    next = std::clamp(shown + (next >= shown ? 3 : -3), 62, 216);
                }
                O.bpmShown = static_cast<float>(next);   // a jump, not an interpolation
            }

            // ---- draw the pill
            auto* pill = O.heartPill;
            pill->clear();
            float w = heartElemW();
            float h = 32.f * S.scale;
            float pillW = 92.f * S.scale;
            float px = w - pillW;
            roundRect(pill, { px, 0.f, pillW, h }, h / 2.f, { 0.03f, 0.01f, 0.03f, 0.92f }, std::max(1.f, 1.4f * S.scale),
                { 1.f, 1.f, 1.f, 0.95f });

            // the heart, pulsing with the beat
            float beat = std::fmod(O.beatPhase, 1.f);
            float pulse = beat < 0.16f ? 1.f + 0.10f * (1.f - beat / 0.16f) : 1.f;
            auto* icon = O.heartIcon;
            icon->clear();
            float pxSize = 2.5f * S.scale * pulse;
            pixelHeart(icon, { px + 8.f * S.scale, h * 0.5f - 4.f * pxSize }, pxSize, { 235, 24, 36 });

            std::string digits = std::to_string(static_cast<int>(std::lround(O.bpmShown)));
            if (digits != O.lastBpm) {
                O.lastBpm = digits;
                O.bpmLabel->setString(digits.c_str());
            }
            O.bpmLabel->setPosition({ px + 44.f * S.scale, h * 0.5f + 1.f });

            // ---- the ECG trace to the left of the pill (optional: some people want just the pill)
            auto* ecg = O.ecg;
            ecg->clear();
            O.ecgHead->clear();
            float span = px - 6.f * S.scale;
            if (S.ecg && span > 20.f) {
                float beatsPerSec = O.bpmShown / 60.f;
                O.ecgT += beatsPerSec;   // one cycle per beat
                float baseY = h * 0.5f;
                float amp = h * 0.42f;
                int const N = 46;
                CCPoint prev{};
                for (int i = 0; i <= N; ++i) {
                    float u = static_cast<float>(i) / N;              // 0 = left, 1 = newest
                    float ph = O.ecgT - (1.f - u) * 1.6f;             // scroll
                    ph -= std::floor(ph);
                    // QRS + T wave
                    float y = 0.f;
                    auto bump = [&](float c, float width, float height) {
                        float d = std::fabs(ph - c);
                        if (d < width) {
                            y += height * (1.f - d / width);
                        }
                    };
                    bump(0.12f, 0.035f, -0.18f);   // Q dip
                    bump(0.18f, 0.022f, 1.0f);     // R spike
                    bump(0.24f, 0.035f, -0.34f);   // S dip
                    bump(0.46f, 0.10f, 0.26f);     // T wave
                    CCPoint p{ u * span, baseY + y * amp };
                    if (i) {
                        // a faint glow under the line, then the line
                        float glowW = 0.9f * S.scale * S.ecgWidth;
                        float lineW = 0.55f * S.scale * S.ecgWidth;
                        ecg->drawSegment(prev, p, lineW * 3.f, { 0.15f, 0.9f, 1.f, 0.10f });
                        ecg->drawSegment(prev, p, lineW, { 0.30f, 0.95f, 1.f, 0.60f });
                    }
                    prev = p;
                }
                // the leading dot
                O.ecgHead->clear();
                float ph = O.ecgT;
                ph -= std::floor(ph);
                float y = 0.f;
                auto bump = [&](float c, float width, float height) {
                    float d = std::fabs(ph - c);
                    if (d < width) {
                        y += height * (1.f - d / width);
                    }
                };
                bump(0.12f, 0.035f, -0.18f);
                bump(0.18f, 0.022f, 1.0f);
                bump(0.24f, 0.035f, -0.34f);
                bump(0.46f, 0.10f, 0.26f);
                CCPoint head{ span, baseY + y * amp };
                circleFill(O.ecgHead, head, 1.5f * S.scale * S.ecgWidth, { 0.55f, 1.f, 1.f, 1.f }, 10);
            }

            // the beat phase advances with the current rate
            O.beatPhase += dt * (O.bpmShown / 60.f);
        }

        // =====================================================================
        // donations
        // =====================================================================

        int randomAmount(uint32_t& s) {
            // 5..1000 with a long tail: most donations are small, $1000 is a moment
            float u = rndf(s);
            float v = 5.f + std::pow(u, 3.6f) * 995.f;
            v = std::round(v / 5.f) * 5.f;
            return static_cast<int>(std::clamp(v, 5.f, 1000.f));
        }

        // A donation in a GD chat is loud shorthand, not a paragraph: it is upper-cased about half
        // the time and short lines get the same shouty treatment chat does.
        std::string giftVoice(std::string text, uint32_t& s) {
            if (text.empty()) {
                return text;
            }
            bool alreadyLoud = std::none_of(text.begin(), text.end(), [](char c) { return c >= 'a' && c <= 'z'; });
            if (alreadyLoud) {
                return text;
            }
            if (rndf(s) < 0.55f) {
                for (auto& c : text) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                return text;
            }
            if (text.size() < 26 && rndf(s) < 0.35f) {
                size_t i = text.find_last_of("aeoAEO");
                if (i != std::string::npos && text[i] >= 'a' && text[i] <= 'z') {
                    text.insert(i + 1, std::string(2 + rnd32(s) % 3, text[i]));
                }
            }
            return text;
        }

        std::string pickGiftMessage(uint32_t& s, int amount) {
            // the user's own lines are used too, and get the same $P / $A / $N treatment
            if (!gCustomGifts.empty() && rndf(s) < 0.25f) {
                return gCustomGifts[rnd32(s) % gCustomGifts.size()];
            }
            auto const& t = text::gifts();
            if (!text::giftTemplates().empty() && rndf(s) < 0.35f) {
                return text::giftTemplates()[rnd32(s) % text::giftTemplates().size()];
            }
            return t.at(static_cast<int>(rnd32(s) % static_cast<uint32_t>(t.size)));
        }

        void startGift() {
            uint32_t s = gSeed;
            // the donor is one of the people in chat, so the name has a colour and badge
            Bot const* b = pickBot(s);
            Gift g;
            g.user = b ? b->name : "viewer";
            g.color = b ? paletteColor(b->color) : ccColor3B { 255, 255, 255 };
            g.badge = b ? b->badge : 0;
            g.amount = randomAmount(s);
            g.message = giftVoice(fillRun(pickGiftMessage(s, g.amount)), s);
            g.t = 0.f;
            g.queued = false;
            O.activeGift = g;
            O.giftBusy = true;
            O.giftLeaving = false;
            if (S.tts) {
                ttsEnqueue(g.user, g.amount, g.message, S.ttsDelay);
            }
            O.gift->setVisible(true);
            O.gift->setScale(0.86f);
            // in from the right edge, with a small overshoot, like the alert tools people use
            float winW = CCDirector::get()->getWinSize().width;
            O.gift->setPosition({ winW, O.gift->getPositionY() });
            float homeX = winW - 250.f * S.scale - 12.f;
            auto slide = CCEaseExponentialOut::create(CCMoveTo::create(0.34f, CCPoint { homeX, O.gift->getPositionY() }));
            O.gift->stopAllActions();
            O.gift->runAction(CCSpawn::create(slide, CCEaseBackOut::create(CCScaleTo::create(0.34f, 1.f)), nullptr));
        }

        // fills in g.wrapped / g.cardH once per donation (and again only if the scale changed)
        void wrapGift(Gift& g) {
            float sc = S.scale;
            // the scale is part of the key: resizing the panel changes the card width, so the wrap
            // has to be redone. Nothing else about a donation changes while it is on screen.
            std::string key = std::to_string(static_cast<long long>(std::lround(sc * 1000.f))) + '|' + g.message;
            if (g.wrapKey == key) {
                return;
            }
            g.wrapKey = key;
            float gw = 260.f * sc;
            float mscale = 0.56f * sc;
            float mlh = lineHeight(mscale, "chatFont.fnt");
            float wrappedH = 0.f;
            g.wrapped = wrapText(g.message, mscale, "chatFont.fnt", gw - 22.f * sc, &wrappedH);
            int n = g.message.empty() ? 0 : (wrappedH > 1.5f ? 2 : 1);
            g.cardH = 40.f * sc + mlh * static_cast<float>(n) + (n ? 6.f * sc : 0.f);
        }

        // the card is exactly as tall as its content: a name row, up to two message lines, and the
        // pink timer bar. It used to be a fixed 92px, which left a dead band under short messages.
        float giftCardHeight(Gift const& g) {
            // measured already: painting happens every frame, this does not
            if (g.cardH > 0.f) {
                return g.cardH;
            }
            float sc = S.scale;
            float gw = 260.f * sc;
            float mscale = 0.56f * sc;
            float mlh = lineHeight(mscale, "chatFont.fnt");
            float wrappedH = 0.f;
            wrapText(g.message, mscale, "chatFont.fnt", gw - 22.f * sc, &wrappedH);
            int n = g.message.empty() ? 0 : (wrappedH > 1.5f ? 2 : 1);
            return 40.f * sc + mlh * static_cast<float>(n) + (n ? 6.f * sc : 0.f);
        }

        void paintGift() {
            if (!O.giftBusy) {
                return;
            }
            // the wrap is cached on the gift: this runs every frame for the length of the alert
            wrapGift(O.activeGift);
            auto const& g = O.activeGift;
            float sc = S.scale;
            float gw = 260.f * sc;
            float mscale = 0.56f * sc;
            float mlh = lineHeight(mscale, "chatFont.fnt");
            auto const& wrapped = g.wrapped;
            float gh = giftCardHeight(g);
            O.gift->setContentSize({ gw, gh });

            // ---- card: dark glass, no frame; the pink bar on top is the alert's timer
            O.giftBg->clear();
            roundRect(O.giftBg, { 0.f, 0.f, gw, gh }, 7.f * sc, { 0.055f, 0.03f, 0.10f, 0.95f }, 0.f);
            float remain = std::clamp(1.f - g.t / g.life, 0.f, 1.f);
            O.giftBar->clear();
            rectFill(O.giftBar, { 0.f, gh - 5.f * sc, gw * remain, 5.f * sc }, { 0.87f, 0.20f, 0.55f, 0.95f });

            // ---- name (its chat colour) with its badge, then "donated" and the amount
            float pad = 11.f * sc;
            float row1 = gh - 17.f * sc;
            O.giftName->setScale(0.62f * sc);
            O.giftName->setColor(g.color);
            O.giftName->setString(g.user.c_str());
            O.giftName->setPosition({ pad, row1 });
            float nx = pad + textW(g.user, 0.62f * sc, "chatFont.fnt") + 4.f * sc;
            if (g.badge > 0) {
                pixelBadge(O.giftBg, g.badge, { nx, row1 - 3.2f * sc }, 0.9f * sc);
                nx += 9.f * sc;
            }

            O.giftFoot->setScale(0.42f * sc);
            O.giftFoot->setColor({ 186, 176, 208 });
            O.giftFoot->setString("DONATED");
            O.giftFoot->setPosition({ nx, row1 });
            float ax = nx + textW("DONATED ", 0.42f * sc, "chatFont.fnt");

            std::string amt = "$" + number(g.amount);
            O.giftAmount->setScale(0.78f * sc);
            O.giftAmount->setColor({ 255, 214, 92 });
            O.giftAmount->setString(amt.c_str());
            O.giftAmount->setPosition({ ax, row1 });

            // ---- the message, under the name line
            float my = row1 - (16.f * sc);
            for (int i = 0; i < 2; ++i) {
                if (i < static_cast<int>(wrapped.size())) {
                    O.giftMsg[i]->setString(wrapped[static_cast<size_t>(i)].c_str());
                    O.giftMsg[i]->setScale(mscale);
                    O.giftMsg[i]->setColor({ 233, 233, 243 });
                    O.giftMsg[i]->setPosition({ pad, my - static_cast<float>(i) * mlh });
                    O.giftMsg[i]->setVisible(true);
                }
                else {
                    O.giftMsg[i]->setVisible(false);
                }
            }
        }

        void updateGifts(float dt) {
            if (!O.gift) {
                return;
            }
            if (O.giftBusy) {
                O.activeGift.t += dt;
                paintGift();
                // slide out at the end
                if (O.activeGift.t > O.activeGift.life - 0.4f && !O.giftLeaving) {
                    O.giftLeaving = true;
                    float winW = CCDirector::get()->getWinSize().width;
                    O.gift->stopAllActions();
                    O.gift->runAction(CCSpawn::create(
                        CCEaseExponentialIn::create(CCMoveTo::create(0.36f, CCPoint { winW, O.gift->getPositionY() })),
                        CCScaleTo::create(0.36f, 0.9f), nullptr));
                }
                if (O.activeGift.t > O.activeGift.life) {
                    O.giftBusy = false;
                    O.giftLeaving = false;
                    O.gift->setVisible(false);
                    O.giftGap = 10.f + rndf(gSeed) * 14.f;
                }
                return;
            }
            O.giftGap -= dt;
            if (!S.gifts) {
                return;
            }
            gNextGiftIn -= dt;
            if (O.giftGap <= 0.f && gNextGiftIn <= 0.f) {
                startGift();
                // the next one is 35-110s away, sooner when the run is exciting
                float hype = std::clamp(gPct, 0.f, 100.f) / 100.f;
                gNextGiftIn = (35.f + rndf(gSeed) * 75.f) * (1.f - 0.45f * hype);
            }
        }

        // =====================================================================
        // viewers
        // =====================================================================

        void updateViewers(float dt) {
            if (!S.viewers) {
                return;
            }
            float lo = static_cast<float>(std::min(S.viewersMin, S.viewersMax));
            float hi = static_cast<float>(std::max(S.viewersMin, S.viewersMax));
            if (S.viewersExact > 0) {
                O.viewerValue = static_cast<float>(S.viewersExact);
                O.viewerTarget = O.viewerValue;
            }
            else if (!S.viewersRandom) {
                O.viewerValue = lo;
                O.viewerTarget = lo;
            }
            else {
                // A viewer count only ever exists as a target plus a lag toward it. The old code
                // re-rolled the target every 2.5-8.5s and also dragged the value toward a *new*
                // random number every single frame, so the two fought and the number parked itself.
                // Now the target wanders up and down on its own timer and the value follows it.
                if (O.viewerValue <= 0.f) {
                    O.viewerTarget = lo + (hi - lo) * bell(gSeed);
                    O.viewerValue = O.viewerTarget;
                }
                if (O.viewerTarget <= 0.f) {
                    O.viewerTarget = O.viewerValue;
                }
                O.viewerTimer -= dt;
                if (O.viewerTimer <= 0.f) {
                    O.viewerTimer = 1.6f + rndf(gSeed) * 5.2f;
                    float range = std::max(1.f, hi - lo);
                    float spread = static_cast<float>(S.vspread) / 100.f;
                    // a run going well slowly pulls the whole audience up
                    float centre = (lo + hi) * 0.5f + range * 0.18f * (std::clamp(gPct, 0.f, 100.f) / 100.f - 0.5f);
                    float newTarget = 0.f;
                    if (S.viewersGradual) {
                        // a real drift: step away from where it is, not a fresh dice roll
                        float drift = range * 0.06f * spread * (rndf(gSeed) * 2.f - 1.f);
                        if (rndf(gSeed) < 0.12f) {
                            drift *= 3.5f;   // now and then somebody raids or a chunk leaves
                        }
                        newTarget = O.viewerTarget + drift;
                        // and a gentle pull back toward the middle so it can't stick to a wall
                        newTarget += (centre - newTarget) * 0.18f;
                    }
                    else {
                        newTarget = lo + range * spread * bell(gSeed);
                    }
                    O.viewerTarget = std::clamp(newTarget, lo, hi);
                }
                float k = std::min(1.f, dt * (S.viewersGradual ? 0.9f : 3.f));
                O.viewerValue += (O.viewerTarget - O.viewerValue) * k;
                // never a still number: a few viewers always come or go, and the nudge is applied
                // to the drift target too so the lag can't just undo it
                O.viewerNudge -= dt;
                if (O.viewerNudge <= 0.f) {
                    O.viewerNudge = 0.6f + rndf(gSeed) * 1.5f;
                    float n = (rndf(gSeed) < 0.5f ? -1.f : 1.f) * static_cast<float>(rndInt(gSeed, 1, 5));
                    O.viewerTarget = std::clamp(O.viewerTarget + n, lo, hi);
                    O.viewerValue = std::clamp(O.viewerValue + n, lo, hi);
                }
            }
            O.viewerValue = std::clamp(O.viewerValue, lo, hi);
            std::string text = number(static_cast<long long>(std::lround(O.viewerValue)));
            if (text != O.lastViewers) {
                O.lastViewers = text;
                O.viewersLabel->setString(text.c_str());
                float pw = 19.f + textW(text, 0.6f, "chatFont.fnt");
                float ph = 14.f;
                O.viewers->setContentSize({ pw, ph });
                O.viewersBg->clear();
                roundRect(O.viewersBg, { 0.f, 0.f, pw, ph }, ph / 2.f, { 0.06f, 0.04f, 0.10f, 0.72f }, 0.f);
                O.viewersIcon->clear();
                circleFill(O.viewersIcon, { 7.f, ph / 2.f }, 2.2f, { 0.92f, 0.15f, 0.22f, 1.f }, 10);
                O.viewersLabel->setPosition({ 12.5f, ph / 2.f });
                // keep the pill pinned inside the panel's top-right corner
                auto r = panelRect();
                if (S.chat) {
                    O.viewers->setPosition({ r.getMaxX() - pw - 5.f, r.getMaxY() - ph - 4.f });
                }
            }
        }

        // =====================================================================
        // the tick
        // =====================================================================

        int currentStepIndex = 0;

        // level events arrive from the hooks; they are turned into chat here, spread over time so
        // a death doesn't dump 15 lines in one frame
        void flushEvents(float dt) {
            for (auto& e : gEvents) {
                e.second -= dt;
            }
            while (!gEvents.empty() && gEvents.front().second <= 0.f) {
                emitOne(gEvents.front().first, gSeed);
                gEvents.pop_front();
            }
        }

    }  // namespace

}  // namespace streamui


namespace streamui {

    namespace {

        // =====================================================================
        // text to speech
        // =====================================================================
        // Speaking has to happen on a worker thread: a platform speech call blocks for seconds and
        // would freeze the level if it ran on the game thread. The worker owns the queue, so spoken
        // lines can never overlap, and the game thread only pushes text and reads a counter.

        struct TtsJob {
            std::string text;
            float delay = 0.f;
            float rate = 1.f;
            float volume = 1.f;
            int voice = 0;
        };

        struct TtsState {
            std::mutex lock;
            std::condition_variable wake;
            std::deque<TtsJob> queue;
            std::atomic<bool> running{ false };
            std::atomic<int> remaining{ 0 };
            // One worker for the life of the process, started on demand. This used to be a
            // std::thread member that was detached straight after being created, so joinable()
            // went back to false and every tick started *another* thread: 60 threads a second,
            // each one waiting on the same condition variable forever. That is the slowdown that
            // got worse the longer a level ran. `alive` is the flag that actually says "there is
            // a thread"; it is only ever set from the game thread.
            bool alive = false;
            std::atomic<bool> quit{ false };
            std::string engineName = "none";
        };

        TtsState gTts;

        bool speakBlocking(TtsJob const& job);

        void ttsWorkerLoop() {
            for (;;) {
                TtsJob job;
                {
                    std::unique_lock<std::mutex> lk(gTts.lock);
                    gTts.wake.wait(lk, [] { return gTts.quit.load() || !gTts.queue.empty(); });
                    if (gTts.quit.load() && gTts.queue.empty()) {
                        return;
                    }
                    job = std::move(gTts.queue.front());
                    gTts.queue.pop_front();
                }
                if (job.delay > 0.f) {
                    // the delay comes from the setting: it's waited out here, not on the game thread
                    std::unique_lock<std::mutex> lk(gTts.lock);
                    gTts.wake.wait_for(lk, std::chrono::milliseconds(static_cast<int>(job.delay * 1000.f)),
                        [] { return gTts.quit.load(); });
                }
                gTts.running = true;
                speakBlocking(job);
                gTts.running = false;
                gTts.remaining.fetch_sub(1);
            }
        }

        void ensureWorker() {
            // `alive`, not joinable(): the worker is detached, so joinable() is false the moment
            // the thread is made and this test never stopped anyone. See TtsState above.
            if (gTts.alive) {
                return;
            }
            gTts.alive = true;
            gTts.quit = false;
            std::thread(ttsWorkerLoop).detach();
        }

        void ttsEnqueue(std::string const& user, int amount, std::string const& message, float delay) {
            if (!S.tts) {
                return;
            }
            // "Username donated 100 dollars: their message"
            std::string text = user + " donated " + std::to_string(amount) + " dollars";
            if (S.ttsMessage && !message.empty()) {
                text += ": " + message;
            }
            text += ".";
            TtsJob job;
            job.text = text;
            job.delay = delay;
            job.rate = S.ttsRate;
            job.volume = S.ttsVolume;
            job.voice = S.ttsVoice;
            {
                std::lock_guard<std::mutex> lk(gTts.lock);
                gTts.queue.push_back(std::move(job));
                gTts.remaining.fetch_add(1);
            }
            gTts.wake.notify_one();
        }

        void ttsClearQueue() {
            std::lock_guard<std::mutex> lk(gTts.lock);
            gTts.queue.clear();
            gTts.remaining = 0;
            gTts.wake.notify_all();
        }

        // ---- per-platform speech ----
        // macOS and iOS can speak for free. Windows needs SAPI, which means linking ole32 and
        // speaking through a COM object; Android needs a JNI call into TextToSpeech. None of that is
        // worth a build that might not link on all five targets, so on those platforms the feature
        // is reported as unavailable and the queue simply stays empty. The setting stays visible so
        // the rest of the panel is identical everywhere.
        std::string ttsEngineLabel() {
#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
            return "system";
#else
            return "unavailable";
#endif
        }

#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
        // macOS `say`: -r words per minute, -v voice, -a device. The volume is applied with
        // AudioServices-free trickery: `say` has no volume flag, so the volume scales a per-utterance
        // gain through the "[[volm x]]" command that `say` understands inside its input.
        bool runProcess(char const* cmd) {
            FILE* p = popen(cmd, "w");
            if (!p) {
                return false;
            }
            return pclose(p) == 0;
        }

        char const* voiceIdFor(int index) {
            // names, not indices: `say -v` matches them case-insensitively on every OS version
            static char const* ID[] = { "", "Samantha", "Alex", "Daniel", "Karen", "Moira", "Tessa", "Fred" };
            return ID[static_cast<size_t>(std::clamp(index, 0, 7))];
        }

        bool speakBlocking(TtsJob const& job) {
            std::string safe;
            safe.reserve(job.text.size());
            for (char c : job.text) {
                if (c == '"' || c == '\\' || c == '`' || c == '$') {
                    safe.push_back('\\');
                }
                safe.push_back(c);
            }
            int wpm = static_cast<int>(175.f * std::clamp(job.rate, 0.5f, 2.f));
            int volume = static_cast<int>(std::clamp(job.volume, 0.f, 1.f) * 100.f);
            // [[volm 0.8]] is a `say` input command: it sets the gain for that utterance
            std::string input = "[[volm " + std::to_string(static_cast<float>(volume) / 100.f) + "]]" + safe;
            std::string voice = voiceIdFor(job.voice);
            std::string cmd = "say -r " + std::to_string(wpm);
            if (!voice.empty()) {
                cmd += " -v " + voice;
            }
            cmd += " \"" + input + "\" 2>/dev/null";
            runProcess(cmd.c_str());
            return true;
        }
#else
        // Windows / Android: no speech engine is wired up, so donations stay silent and the queue
        // stays empty (see the note above). Nothing else changes.
        bool speakBlocking(TtsJob const&) {
            return false;
        }
#endif

    }  // namespace

    // =====================================================================
    // the public names
    // =====================================================================

    bool ttsAvailable() {
        return ttsEngineLabel() != "unavailable";
    }

    int voiceCount() {
        if (!ttsAvailable()) {
            return 1;
        }
#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
        return 8;
#else
        return 1;
#endif
    }

    std::string voiceName(int index) {
        static char const* N[] = { "System default", "Samantha", "Alex", "Daniel", "Karen", "Moira", "Tessa", "Fred" };
        if (!ttsAvailable()) {
            return "Not available on this platform";
        }
        return N[static_cast<size_t>(std::clamp(index, 0, 7))];
    }

    int ttsQueueSize() {
        return gTts.remaining.load();
    }

    void ttsClear() {
        ttsClearQueue();
        dirty = true;
    }

    void speakTest() {
        TtsJob job;
        job.text = "This is how the donation alerts sound.";
        job.rate = S.ttsRate;
        job.volume = S.ttsVolume;
        job.voice = S.ttsVoice;
        {
            std::lock_guard<std::mutex> lk(gTts.lock);
            gTts.queue.push_back(std::move(job));
            gTts.remaining.fetch_add(1);
        }
        ensureWorker();
        gTts.wake.notify_one();
    }

}  // namespace streamui


namespace streamui {

    // =====================================================================
    // the per-frame tick and the level events
    // =====================================================================

    void tick(CCNode* layer, float dt) {
        load();
        if (!layer) {
            return;
        }
        if (dt <= 0.f || dt > 0.5f) {
            dt = 1.f / 60.f;
        }
        gClock += dt;
        // the menu can change size, scale or corner at any time: re-place the tree when it did
        if (O.built && (S.width != O.laidOutFor.w || S.height != O.laidOutFor.h || S.scale != O.laidOutFor.scale ||
                           static_cast<float>(S.pos) != O.laidOutFor.pos || (S.chat ? 1.f : 0.f) != O.laidOutFor.chat)) {
            layoutOverlay();
            O.laidOutFor = { S.width, S.height, S.scale, static_cast<float>(S.pos), S.chat ? 1.f : 0.f };
        }
        ensureWorker();
        uint32_t& s = gSeed;
        rnd32(s);

        // ---- the overlay follows the level node, so leaving the level removes it with the level
        if (!O.root) {
            buildOverlay();
        }
        if (O.root && O.root->getParent() != layer) {
            if (O.root->getParent()) {
                O.root->removeFromParentAndCleanup(false);
            }
            layer->addChild(O.root, 1000);
        }

        // ---- the GO wall takes over the feed while it runs
        if (gWallCooldown > 0.f) {
            gWallCooldown = std::max(0.f, gWallCooldown - dt);
        }
        if (S.chat && gWallLeft > 0) {
            gWallGap -= dt;
            while (gWallLeft > 0 && gWallGap <= 0.f) {
                emitOne(text::C_GO, s);
                --gWallLeft;
                // the wall speeds up as it goes, the way a real one does
                float t = gWallLines > 0 ? 1.f - static_cast<float>(gWallLeft) / gWallLines : 1.f;
                float base = gWallBase > 0.f ? gWallBase : 0.13f;
                // a wall builds: the first lines are deliberate, then they pile in. The whole
                // thing lasts long enough to watch the panel fill up with GO, which is the point.
                gWallGap += std::max(0.045f, base * (1.f - 0.45f * t)) * (0.7f + rndf(s) * 0.7f);
            }
            if (gWallLeft <= 0) {
                // a breath after the wall, then whatever was queued for the moment lands
                gChatTimer = std::max(gChatTimer, 0.30f);
                gWallLines = 0;
            }
        }

        // ---- the chat clock
        if (S.chat) {
            // Events only drain inside the chat clock above, so with the chat switched off every
            // death and every completion piled its lines into the queue and nothing ever took them out.
            flushEvents(dt);

            // base rate from the setting, scaled by how exciting the run is
            float rate = currentRate();
            // bursts: at high hype chat arrives in clumps, which is what real chat does
            if (gBurst > 0.f) {
                gBurst -= dt;
                gBurstGap -= dt;
                if (gBurstGap <= 0.f) {
                    gBurstGap = 0.035f + rndf(s) * 0.09f;
                    emitOne(-1, s);
                }
            }
            // while a wall runs, the ordinary feed is mostly held back: the wall *is* the chat
            if (gWallLeft > 0) {
                rate *= 0.18f;
            }
            gChatTimer -= dt * rate;
            if (gChatTimer <= 0.f) {
                // consume a few slots when the rate is high, so "extreme" really is dense
                int burst = 1;
                if (rate > 16.f) {
                    burst = 1 + static_cast<int>(rndInt(s, 0, 2));
                }
                for (int i = 0; i < burst; ++i) {
                    emitOne(-1, s);
                }
                if (gPct > 78.f && rndf(s) < 0.14f) {
                    gBurst = 0.35f + rndf(s) * 0.7f;
                    gBurstGap = 0.f;
                }
                // spacing jitter: never a metronome
                gChatTimer = 0.55f + rndf(s) * 0.95f;
            }
        }

        // Events only drain inside the chat clock above, so with the chat switched off every
        // death and every completion piled its lines into the queue and nothing ever took them
        // out. They are dropped rather than kept for a chat that isn't on screen.
        if (!S.chat) {
            gEvents.clear();
            gBurst = 0.f;
        }

        healRows(dt);
        updateViewers(dt);
        updateHeart(dt);
        updateGifts(dt);

        // a completed level holds its celebration for a while
        if (gComplete) {
            O.completeT += dt;
            gCompleteT += dt;
        }
    }

    void setLevelName(std::string name) {
        gLevelName = std::move(name);
        if (gLevelName.size() > 40) {
            gLevelName.resize(40);
        }
    }

    void onProgress(float pct) {
        gPct = std::clamp(pct, 0.f, 100.f);
        // A personal best is the moment a GD chat starts typing GO, so the overlay tracks the best
        // percentage it has seen across this session (and remembers the level's real best through
        // setBestPercent) and throws a wall every time it is beaten.
        if (gPct > gAttemptMax) {
            gAttemptMax = gPct;
        }
        if (gPct > gBest + 0.05f) {
            bool wasMeaningful = gBest > 0.f && gPct - gBest >= 1.f;
            gBest = gPct;
            if (wasMeaningful && S.chat && gPct < 99.5f) {
                // a couple of stacked walls at the top end, a short one earlier on
                int lines = gPct > 85.f ? 16 : gPct > 55.f ? 11 : 7;
                startWallSoft(lines, gPct > 85.f ? 0.085f : 0.14f);
                pushEvent(text::C_PB, 2, 1.1f);
            }
        }
        if (gPct >= 100.f) {
            return;
        }
    }

    // the level's own saved best, so the first wall comes at the right moment on a fresh session
    void setBestPercent(float pct) {
        gBest = std::clamp(pct, 0.f, 100.f);
    }

    // a GO wall on demand (the chip's "hype" tap, and the completion/death hooks)
    void hypeWall() {
        load();
        // the menu button: a full wall, long enough to see, starting immediately
        gWallCooldown = 0.f;
        startWall(14, 0.10f);
    }

    void onDeath(float pct) {
        if (!S.chat) {
            return;
        }
        // the death reaction scales with how far the run got: dying at 10% gets a shrug, dying at
        // 97% gets the whole chat screaming
        int count = pct > 85.f ? 10 : pct > 70.f ? 7 : pct > 45.f ? 4 : 2;
        pushEvent(text::C_DEATH, count, pct > 70.f ? 1.4f : 0.7f);
        if (pct > 70.f && S.gifts && rndf(gSeed) < 0.18f) {
            gNextGiftIn = std::min(gNextGiftIn, 3.f);   // someone always tips after a painful death
        }
    }

    void onComplete() {
        gComplete = true;
        gCompleteT = 0.f;
        if (!S.chat) {
            return;
        }
        // the big one: a GO/GG wall, then celebration that keeps going for a while
        startWall(20, 0.075f);
        pushEvent(text::C_CELEBRATE, 26, 3.5f);
        pushEvent(text::C_SPAM, 10, 3.0f);
        pushEvent(text::C_EMOTE, 8, 2.4f);
        // and a donation, because someone always does
        gNextGiftIn = std::min(gNextGiftIn, 2.5f);
        gComplete = true;
    }

    void onRestart() {
        O.completeT = 0.f;
        ++gAttempts;
        gAttemptMax = 0.f;
        if (gComplete) {
            gComplete = false;
            gCompleteT = 0.f;
        }
        // heart rate: no snap back to 70, it eases down over the next several seconds (the sampling
        // in updateHeart walks it down 3-7 at a time)
        gEventCooldown = 0.f;
    }

    void publishStats() {
        publishedChat = text::chat().size;
        publishedGifts = text::gifts().size + static_cast<int>(text::giftTemplates().size());
    }

    void load() {
        static bool once = false;
        if (once) {
            return;
        }
        once = true;
        loadSettings();
        loadCustom();
        loadCustomGifts();
        poolOfBots();
        rebuildActive();
        publishStats();
    }

    bool chatOn() { return S.chat; }
    bool viewersOn() { return S.viewers; }
    bool giftsOn() { return S.gifts; }
    bool heartOn() { return S.heart; }
    int messageCount() { return publishedChat; }
    int giftMessageCount() { return publishedGifts; }
    int activeBotCount() { return static_cast<int>(gActive.size()); }
    int totalBotCount() { return static_cast<int>(gBots.size()); }
    int currentBpm() { return static_cast<int>(std::lround(O.bpmShown)); }
    int currentViewers() { return static_cast<int>(std::lround(O.viewerValue)); }
    int giftQueueSize() { return O.giftBusy ? 1 : 0; }

    std::vector<Bot> const& bots() { return gBots; }

    std::vector<Bot> const& customBots() { return gCustom; }

    bool validName(std::string const& name) {
        return names::acceptable(name);
    }

    void drawBadgePreview(CCDrawNode* d, int kind, CCPoint at, float px) {
        pixelBadge(d, kind, at, px * 1.6f);
    }

    void upsertBot(int index, Bot const& bot) {
        Bot b = bot;
        b.custom = true;
        if (index >= 0 && index < static_cast<int>(gCustom.size())) {
            gCustom[static_cast<size_t>(index)] = b;
        }
        else {
            gCustom.push_back(b);
        }
        saveCustom();
        poolOfBots();
        rebuildActive();
        dirty = true;
    }

    void eraseBot(int index) {
        if (index < 0 || index >= static_cast<int>(gCustom.size())) {
            return;
        }
        gCustom.erase(gCustom.begin() + index);
        saveCustom();
        poolOfBots();
        rebuildActive();
        dirty = true;
    }

    std::vector<std::string> customGiftMessages() {
        return gCustomGifts;
    }

    void addGiftMessage(std::string const& text) {
        if (text.empty()) {
            return;
        }
        gCustomGifts.push_back(text);
        saveCustomGifts();
        dirty = true;
    }

    void eraseGiftMessage(int index) {
        if (index < 0 || index >= static_cast<int>(gCustomGifts.size())) {
            return;
        }
        gCustomGifts.erase(gCustomGifts.begin() + index);
        saveCustomGifts();
        dirty = true;
    }

    char const* colorName(int index) {
        return PALETTE_NAMES[static_cast<size_t>(std::clamp(index, 0, 29))];
    }

    int colorCount() {
        return 30;
    }

    ccColor3B colorOf(int index) {
        return paletteColor(index);
    }

    char const* badgeName(int index) {
        static char const* N[] = { "none", "mod", "vip", "sub", "founder" };
        return N[std::clamp(index, 0, 4)];
    }

    int badgeCount() {
        return 5;
    }

    void refresh() {
        loadSettings();
        rebuildActive();
        if (O.built) {
            layoutOverlay();
        }
        dirty = false;
    }

    // The overlay's node is retained by the overlay (addChild alone wouldn't keep it alive once the
    // PlayLayer it was added to is gone). Coming back to the menu means the level node is about to
    // die, so the overlay is detached first and the whole tree is rebuilt on the next level. This
    // is what keeps O.root from ever being a dangling pointer.
    void dropOverlay() {
        if (O.root) {
            if (O.root->getParent()) {
                O.root->removeFromParentAndCleanup(false);
            }
            O.root->release();
            O.root = nullptr;
        }
        O.built = false;
        O.chat = nullptr;
        O.chatBg = nullptr;
        O.headLabel = nullptr;
        O.viewers = nullptr;
        O.viewersBg = nullptr;
        O.viewersLabel = nullptr;
        O.viewersIcon = nullptr;
        O.heart = nullptr;
        O.heartPill = nullptr;
        O.heartIcon = nullptr;
        O.ecg = nullptr;
        O.bpmLabel = nullptr;
        O.ecgHead = nullptr;
        O.gift = nullptr;
        O.giftBg = nullptr;
        O.giftBar = nullptr;
        O.giftName = nullptr;
        O.giftAmount = nullptr;
        O.giftMsg[0] = O.giftMsg[1] = nullptr;
        O.giftFoot = nullptr;
        O.rowsHost = nullptr;
        O.header = nullptr;
        O.headGd = nullptr;
        O.headStream = nullptr;
        O.grip = nullptr;
        O.gripDraw = nullptr;
        O.chip = nullptr;
        O.chipDraw = nullptr;
        O.rows.clear();
        O.lines.clear();
        O.gifts.clear();
        O.giftBusy = false;
        O.lastBpm.clear();
        O.lastViewers.clear();
    }

    void resetOverlay() {
        gPct = 0.f;
        gAttemptMax = 0.f;
        gComplete = false;
        gAttempts = 0;
        gEvents.clear();
        gBurst = 0.f;
        gWallLeft = 0;
        gWallLines = 0;
        gWallGap = 0.f;
        gWallCooldown = 0.f;
        // gBest is not cleared here: setBestPercent() puts it back to the level's own record right
        // after this, so a fresh attempt doesn't wall at every percent the player has already done
        if (O.built) {
            O.lines.clear();
            relayoutRows();
            O.gifts.clear();
            O.giftBusy = false;
            if (O.gift) {
                O.gift->setVisible(false);
            }
        }
        // start fresh: the previous level's node now owns a tree we shouldn't touch again
        dropOverlay();
    }

    void clearChat() {
        O.lines.clear();
        relayoutRows();
        dirty = true;
    }

    void previewMessage() {
        load();
        if (!O.built) {
            buildOverlay();
        }
        uint32_t s = 0xABCDEFu;
        // a small clump so the preview looks like a real moment
        for (int i = 0; i < 3; ++i) {
            emitOne(-1, s);
        }
    }

    void testDonation() {
        load();
        if (!O.built) {
            buildOverlay();
        }
        startGift();
    }

    void heartSpike() {
        // a scare is a jump as well: 9-14 up, never a crawl
        int up = rndInt(gSeed, 9, 14);
        O.bpmShown = static_cast<float>(std::min(216, static_cast<int>(std::lround(O.bpmShown)) + up));
        O.bpmReal = O.bpmShown;
        O.bpmTimer = 0.f;
    }

    // =====================================================================
    // moving and resizing the panel by touch
    // =====================================================================
    // The overlay sits on top of the level, so it needs its own touch handling. A drag that starts
    // on the chat panel (and isn't a tap on a line) moves it and switches the position to Custom; a
    // drag that starts on the corner grip resizes it. Everything is in window coordinates.

    void setChipAction(std::function<void()> fn) {
        O.onChip = std::move(fn);
    }

    void setChipVisible(bool on) {
        gChipRuntime = on;
        if (O.chip) {
            O.chip->setVisible(on && S.chip);
        }
    }

    // returns true when the touch was taken by the overlay
    bool touchDown(CCPoint world) {
        if (!O.built || !O.root) {
            return false;
        }
        auto r = panelRect();
        // the grip first: it sits on the panel's corner
        if (S.chat) {
            CCRect grip{ r.getMaxX() - 22.f, r.getMinY() - 2.f, 26.f, 20.f };
            if (world.x >= grip.origin.x && world.x <= grip.getMaxX() && world.y >= grip.origin.y && world.y <= grip.getMaxY()) {
                gResizing = true;
                gResizeOrigin = world;
                gResizeW = S.width;
                gResizeH = S.height;
                return true;
            }
        }
        // the chip
        if (O.chip && O.chip->isVisible() && S.chip && gChipRuntime) {
            auto c = O.chip->getPosition();
            auto sz = O.chip->getContentSize();
            if (world.x >= c.x && world.x <= c.x + sz.width && world.y >= c.y && world.y <= c.y + sz.height) {
                if (O.onChip) {
                    O.onChip();
                }
                return true;
            }
        }
        // the panel itself
        if (S.chat && world.x >= r.origin.x && world.x <= r.getMaxX() && world.y >= r.origin.y && world.y <= r.getMaxY()) {
            gDragging = true;
            gMoved = false;
            gGrabOffset = CCPoint { world.x - r.origin.x, world.y - r.origin.y };
            return true;
        }
        return false;
    }

    void touchMove(CCPoint world) {
        auto win = CCDirector::get()->getWinSize();
        if (gResizing) {
            float dx = world.x - gResizeOrigin.x;
            float dy = world.y - gResizeOrigin.y;
            float w = std::clamp(gResizeW + dx / std::max(0.2f, S.scale), 150.f, 900.f);
            float h = std::clamp(gResizeH - dy / std::max(0.2f, S.scale), 100.f, 900.f);
            S.width = std::round(w / 10.f) * 10.f;
            S.height = std::round(h / 10.f) * 10.f;
            Mod::get()->setSavedValue("stream-w", S.width);
            Mod::get()->setSavedValue("stream-h", S.height);
            layoutOverlay();
            return;
        }
        if (gDragging) {
            gMoved = true;
            float x = world.x - gGrabOffset.x;
            float y = world.y - gGrabOffset.y;
            // free positioning: the panel is centred where the user drops it
            S.cx = std::clamp((x + S.width * S.scale / 2.f) / std::max(1.f, win.width), 0.f, 1.f);
            S.cy = std::clamp(1.f - (y + S.height * S.scale / 2.f) / std::max(1.f, win.height), 0.f, 1.f);
            S.pos = 4;
            Mod::get()->setSavedValue("stream-pos", 4);
            Mod::get()->setSavedValue("stream-cx", S.cx);
            Mod::get()->setSavedValue("stream-cy", S.cy);
            layoutOverlay();
        }
    }

    void touchUp() {
        bool wasResize = gResizing;
        bool tapped = gDragging && !gMoved;
        gDragging = false;
        gResizing = false;
        gMoved = false;
        // a tap on the panel (not a drag) opens the settings: it's the discoverable way in on a phone
        if (tapped && O.onChip) {
            O.onChip();
        }
        if (wasResize) {
            dirty = true;  // the menu shows the size, so let it refresh
        }
    }

    bool dragging() {
        return gDragging || gResizing;
    }

    // the level is gone: let go of the overlay's tree before the PlayLayer it lived in dies
    void leaveLevel() {
        dropOverlay();
    }

}  // namespace streamui
