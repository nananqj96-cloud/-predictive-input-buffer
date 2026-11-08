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

namespace stream {

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
            float scale = 1.f;
        };

        Settings S;

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
            S.scale = std::clamp(m.getSavedValue<float>("stream-scale", 1.f), 0.6f, 2.2f);
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
            static CCLabelBMFont* chat = nullptr;
            static CCLabelBMFont* head = nullptr;
            auto get = [&](CCLabelBMFont*& slot) {
                if (!slot) {
                    slot = CCLabelBMFont::create("Ag", fnt);
                    slot->retain();
                }
                return slot;
            };
            return std::strcmp(fnt, "chatFont.fnt") == 0 ? get(chat) : get(head);
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
        void circleFill(CCDrawNode* d, CCPoint at, float r, ccColor4F fill, unsigned seg = 18) {
            d->drawCircle(at, r, 0.f, seg, fill, 0.f, fill);
        }

        void circleRing(CCDrawNode* d, CCPoint at, float r, ccColor4F fill, float width, ccColor4F ring, unsigned seg = 18) {
            d->drawCircle(at, r, 0.f, seg, fill, width, ring);
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
                float rr = std::max(0.5f, rad);
                // four sides + four corner arcs, drawn as segments
                d->drawSegment({ r.origin.x + rr, r.origin.y }, { r.getMaxX() - rr, r.origin.y }, border, borderColor);
                d->drawSegment({ r.origin.x + rr, r.getMaxY() }, { r.getMaxX() - rr, r.getMaxY() }, border, borderColor);
                d->drawSegment({ r.origin.x, r.origin.y + rr }, { r.origin.x, r.getMaxY() - rr }, border, borderColor);
                d->drawSegment({ r.getMaxX(), r.origin.y + rr }, { r.getMaxX(), r.getMaxY() - rr }, border, borderColor);
                constexpr int SEG = 5;
                for (int c = 0; c < 4; ++c) {
                    float cx = c == 0 || c == 3 ? r.origin.x + rr : r.getMaxX() - rr;
                    float cy = c == 0 || c == 1 ? r.origin.y + rr : r.getMaxY() - rr;
                    float base = c == 0 ? 180.f : c == 1 ? 270.f : c == 2 ? 0.f : 90.f;
                    CCPoint prev{};
                    for (int i = 0; i <= SEG; ++i) {
                        float a = (base + 90.f * static_cast<float>(i) / SEG) * 3.14159265f / 180.f;
                        CCPoint p{ cx + rr * std::cos(a), cy + rr * std::sin(a) };
                        if (i) {
                            d->drawSegment(prev, p, border, borderColor);
                        }
                        prev = p;
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
        };

        struct Row {
            CCNode* node = nullptr;
            CCLabelBMFont* stamp = nullptr;
            CCDrawNode* badge = nullptr;
            CCLabelBMFont* user = nullptr;
            CCLabelBMFont* body[3] = { nullptr, nullptr, nullptr };
            bool used = false;
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
            float giftGap = 12.f;

            float lineH = 12.f;
            float bpmShown = 74.f;
            float bpmReal = 74.f;
            float bpmTimer = 2.f;
            float beatPhase = 0.f;
            float ecgT = 0.f;
            float viewerValue = 0.f;
            float viewerTimer = 4.f;
            float chatClock = 0.f;
            std::string lastViewers;
            std::string lastBpm;
            bool built = false;
        };

        Overlay O;

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

        // ---- dragging
        bool gDragging = false;
        bool gResizing = false;
        CCPoint gGrabOffset{};
        CCPoint gResizeOrigin{};
        float gResizeW = 0.f;
        float gResizeH = 0.f;

        void ttsEnqueue(std::string const& user, int amount, std::string const& message, float delay);
        void buildOverlay();
        void layoutOverlay();
        void addLine(LineView line);
        void relayoutRows();
        void pushEvent(int cat, int count, float spread = 0.55f);

        // ---- hype weights: how the audience behaves at a given percentage
        struct Weights {
            float w[text::C_COUNT];
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
        int nextLine(uint32_t& s, int cat) {
            auto const& t = text::chat();
            int n = t.count(cat);
            if (n <= 0) {
                return 0;
            }
            if (cat != gBagCat || gBagPos >= gBag.size()) {
                gBag.resize(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) {
                    gBag[static_cast<size_t>(i)] = t.first(cat) + i;
                }
                for (size_t i = gBag.size(); i > 1; --i) {
                    std::swap(gBag[i - 1], gBag[rnd32(s) % i]);
                }
                gBagPos = 0;
                gBagCat = cat;
            }
            return gBag[gBagPos++];
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
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j <= i; ++j) {
                    circleFill(O.gripDraw, { 12.f - i * 3.5f, 3.5f + j * 3.5f }, 1.f, { 1.f, 1.f, 1.f, 0.55f }, 8);
                }
            }
            O.grip->setContentSize({ 16.f, 16.f });
            O.chip = newNode(root, 40);
            O.chipDraw = CCDrawNode::create();
            O.chip->addChild(O.chipDraw, 0);
            O.chip->setContentSize({ 44.f, 16.f });
            roundRect(O.chipDraw, { 0.f, 0.f, 44.f, 16.f }, 8.f, { 0.04f, 0.02f, 0.07f, 0.55f }, 0.5f,
                { 1.f, 1.f, 1.f, 0.18f });
            {
                auto* l = CCLabelBMFont::create("menu", "chatFont.fnt");
                l->setScale(0.4f);
                l->setColor({ 210, 210, 225 });
                l->setPosition({ 22.f, 8.f });
                O.chip->addChild(l, 1);
            }

            // ---------------- chat rows ----------------
            // 110 rows is more than a 900px panel can show at the smallest font, so a resize
            // never runs out
            O.rows.resize(110);
            for (auto& r : O.rows) {
                r.node = newNode(O.chat, 1);
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
        }

        // =====================================================================
        // layout
        // =====================================================================

        CCRect panelRect() {
            auto win = CCDirector::get()->getWinSize();
            float w = S.width * S.scale, h = S.height * S.scale;
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
            y = std::clamp(y, 2.f, std::max(2.f, win.height - h - 2.f));
            return { x, y, w, h };
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

            float headH = 16.f;
            if (O.headLabel) {
                O.headLabel->setPosition({ 8.f, r.size.height - headH / 2.f + 1.f });
                O.headLabel->setColor({ 214, 214, 224 });
            }

            // the chat background: a soft dark glass with a hairline top light, like the overlay
            // in the reference screenshots but readable over any level
            O.chatBg->clear();
            if (chatVisible) {
                float a = S.opacity;
                roundRect(O.chatBg, { 0.f, 0.f, r.size.width, r.size.height }, 8.f,
                    { 0.043f, 0.027f, 0.078f, 0.72f * a }, 0.f);
                // a brighter band behind the header
                roundRect(O.chatBg, { 0.f, r.size.height - headH - 2.f, r.size.width, headH + 2.f }, 8.f,
                    { 1.f, 1.f, 1.f, 0.05f * a }, 0.f);
                O.chatBg->drawSegment({ 8.f, r.size.height - headH - 2.f }, { r.size.width - 8.f, r.size.height - headH - 2.f },
                    0.5f, { 1.f, 1.f, 1.f, 0.10f * a });
                O.chatBg->drawRect({ 1.f, 1.f, r.size.width - 2.f, r.size.height - 2.f }, { 1.f, 1.f, 1.f, 0.07f * a });
            }

            // viewer pill, top-right of the panel (or alone in the corner when chat is off)
            if (O.viewers) {
                O.viewers->setVisible(S.viewers);
                if (S.viewers) {
                    float pw = 66.f, ph = 15.f;
                    O.viewers->setContentSize({ pw, ph });
                    if (chatVisible) {
                        O.viewers->setPosition({ r.getMaxX() - pw - 6.f, r.getMaxY() - headH + 0.5f });
                    }
                    else {
                        auto corner = S.pos == 0 || S.pos == 2 ? CCPoint { 12.f, win.height - ph - 12.f }
                                                              : CCPoint { win.width - pw - 12.f, win.height - ph - 12.f };
                        O.viewers->setPosition(corner);
                    }
                    O.viewersBg->clear();
                    roundRect(O.viewersBg, { 0.f, 0.f, pw, ph }, ph / 2.f, { 0.08f, 0.05f, 0.12f, 0.85f }, 0.6f,
                        { 1.f, 1.f, 1.f, 0.16f });
                    O.viewersIcon->clear();
                    // a tiny red live dot
                    circleFill(O.viewersIcon, { 8.f, ph / 2.f }, 2.4f, { 0.92f, 0.15f, 0.22f, 1.f }, 10);
                    O.viewersLabel->setPosition({ 14.f, ph / 2.f });
                }
            }

            // heart monitor: right edge of the screen, vertically where the reference puts it
            if (O.heart) {
                O.heart->setVisible(S.heart);
                if (S.heart) {
                    float h = 34.f * S.scale;
                    float w = 126.f * S.scale;
                    float x = win.width - w - 14.f;
                    float y = win.height * 0.58f;
                    // at the top-right of a level the progress bar lives; keep clear of it
                    if (S.pos == 0 || S.pos == 1) {
                        y = win.height * 0.42f;
                    }
                    O.heart->setPosition({ x, y });
                    O.heart->setContentSize({ w, h });
                }
            }

            // donation alert: middle-right of the screen
            if (O.gift) {
                float gw = 250.f * S.scale;
                float gh = 84.f * S.scale;
                O.gift->setContentSize({ gw, gh });
                O.gift->setPosition({ win.width - gw - 12.f, win.height * 0.44f });
            }

            if (O.grip) {
                O.grip->setVisible(chatVisible);
                O.grip->setPosition({ r.getMaxX() - 17.f, r.getMinY() + 2.f });
            }
            if (O.chip) {
                // under the viewers pill when the chat is at the bottom of the screen, else above it
                bool bottom = S.pos == 2 || S.pos == 3;
                auto pos = bottom ? CCPoint { 12.f, 12.f } : CCPoint { 12.f, win.height - 28.f };
                O.chip->setPosition(pos);
            }

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

            // body: wrap if there is room for a second line, else clip to the row
            float avail = std::max(40.f, width - x - 6.f);
            float linesH = 0.f;
            auto wrapped = wrapText(v.body, scale, "chatFont.fnt", avail, &linesH);
            if (wrapped.size() > 3) {
                wrapped.resize(3);
            }
            for (int i = 0; i < 3; ++i) {
                auto* l = r.body[i];
                if (i < static_cast<int>(wrapped.size()) && i == 0) {
                    l->setString((v.body).c_str());
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
            r.node->setVisible(true);
        }

        // stacks the newest line at the bottom, oldest scroll away at the top
        void relayoutRows() {
            if (!O.built || !S.chat) {
                return;
            }
            float width = O.chat->getContentSize().width;
            float headH = 16.f;
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
                // wrapped height for this line
                float x = 6.f;
                if (S.showTime) {
                    x += textW(v.stamp, 0.52f, "chatFont.fnt") + 4.f;
                }
                if (v.badge > 0) {
                    x += 9.f;
                }
                x += textW(v.user + ":", scale, "chatFont.fnt") + 3.5f;
                float avail = std::max(40.f, width - x - 6.f);
                float linesH = 0.f;
                auto wrapped = wrapText(v.body, scale, "chatFont.fnt", avail, &linesH);
                int rowsNeeded = static_cast<int>(std::min<size_t>(wrapped.size(), 3));
                float h = O.lineH * static_cast<float>(std::max(rowsNeeded, 1)) + 1.f;
                if (y + h > usable + 1.f) {
                    break;
                }
                auto& r = O.rows[ri++];
                paintRow(r, v, width);
                r.node->setPosition({ 0.f, y });
                // the body under the first line, if it wrapped
                float lh = O.lineH;
                for (int i = 1; i < 3; ++i) {
                    if (i < static_cast<int>(wrapped.size())) {
                        r.body[i]->setString(wrapped[static_cast<size_t>(i)].c_str());
                        r.body[i]->setScale(scale);
                        r.body[i]->setColor({ 226, 226, 236 });
                        float bx = 6.f;
                        if (S.showTime) {
                            bx += textW(v.stamp, 0.52f, "chatFont.fnt") + 4.f;
                        }
                        if (v.badge > 0) {
                            bx += 9.f;
                        }
                        bx += textW(v.user + ":", scale, "chatFont.fnt") + 3.5f;
                        r.body[i]->setPosition({ bx, lh * 0.5f + static_cast<float>(i) * lh });
                        r.body[i]->setVisible(true);
                    }
                    else {
                        r.body[i]->setVisible(false);
                    }
                }
                y += h;
            }
        }

        void addLine(LineView line) {
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
            v.body = styleOf(text, *b, s);
            addLine(std::move(v));
        }

        void pushEvent(int cat, int count, float spread) {
            for (int i = 0; i < count; ++i) {
                gEvents.emplace_back(cat, rndf(gSeed) * spread);
            }
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
            int target = targetBpm(gPct);

            // irregular jumps of 3-7 bpm, with a wandering delay. Never 1 at a time, never a jump
            // from the resting rate to the ceiling.
            O.bpmTimer -= dt;
            if (O.bpmTimer <= 0.f) {
                O.bpmTimer = 0.75f + rndf(gSeed) * 1.6f;
                float diff = static_cast<float>(target) - O.bpmShown;
                float step = static_cast<float>(rndInt(gSeed, 3, 7));
                if (std::abs(diff) < 2.f) {
                    // hovering at the target: small jitter, still 3+ so it never crawls by one
                    step = static_cast<float>(rndInt(gSeed, 3, 5)) * (rndf(gSeed) < 0.5f ? -1.f : 1.f);
                }
                else {
                    step *= diff > 0.f ? 1.f : -1.f;
                    // when the gap is big (a restart), move a bit further per tick than usual
                    if (std::abs(diff) > 25.f && rndf(gSeed) < 0.35f) {
                        step *= 1.6f;
                    }
                }
                O.bpmReal = std::clamp(O.bpmShown + step, 62.f, 214.f);
                // calm people read lower than tense people; a tiny bias keeps it from looking fake
                if (gPct < 5.f && rndf(gSeed) < 0.3f) {
                    O.bpmReal -= 2.f;
                }
            }
            // the displayed number eases toward the sampled one, the way a watch does
            float k = std::min(1.f, dt * 4.5f);
            O.bpmShown += (O.bpmReal - O.bpmShown) * k;

            // ---- draw the pill
            auto* pill = O.heartPill;
            pill->clear();
            float w = 126.f * S.scale;
            float h = 34.f * S.scale;
            float pillW = 88.f * S.scale;
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
            O.bpmLabel->setPosition({ px + 34.f * S.scale, h * 0.5f + 1.f });

            // ---- the ECG trace to the left of the pill
            auto* ecg = O.ecg;
            ecg->clear();
            float span = px - 6.f * S.scale;
            if (span > 20.f) {
                float beatsPerSec = O.bpmShown / 60.f;
                O.ecgT += beatsPerSec;   // one cycle per beat
                float baseY = h * 0.5f;
                float amp = h * 0.34f;
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
                        ecg->drawSegment(prev, p, 3.2f * S.scale, { 0.15f, 0.9f, 1.f, 0.10f });
                        ecg->drawSegment(prev, p, 1.5f * S.scale, { 0.30f, 0.95f, 1.f, 0.55f });
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
                circleFill(O.ecgHead, head, 2.6f * S.scale, { 0.55f, 1.f, 1.f, 1.f }, 10);
            }

            // the beat phase advances with the current rate
            O.beatPhase += dt * (O.bpmShown / 60.f);
        }

        // =====================================================================
        // donations
        // =====================================================================

        std::string fillTemplate(std::string s, int amount, std::string const& user) {
            auto replace = [&s](std::string const& from, std::string const& to) {
                size_t p = 0;
                while ((p = s.find(from, p)) != std::string::npos) {
                    s.replace(p, from.size(), to);
                    p += to.size();
                }
            };
            replace("$P", std::to_string(std::clamp(static_cast<int>(std::lround(gPct)), 1, 99)));
            replace("$A", std::to_string(std::max(1, gAttempts)));
            replace("$U", user);
            replace("$N", std::to_string(amount));
            return s;
        }

        int randomAmount(uint32_t& s) {
            // 5..1000 with a long tail: most donations are small, $1000 is a moment
            float u = rndf(s);
            float v = 5.f + std::pow(u, 3.6f) * 995.f;
            v = std::round(v / 5.f) * 5.f;
            return static_cast<int>(std::clamp(v, 5.f, 1000.f));
        }

        std::string pickGiftMessage(uint32_t& s, int amount) {
            // the user's own lines are used too, and get the same $P / $A / $N treatment
            if (!gCustomGifts.empty() && rndf(s) < 0.25f) {
                return fillTemplate(gCustomGifts[rnd32(s) % gCustomGifts.size()], amount, "");
            }
            auto const& t = text::gifts();
            if (!text::giftTemplates().empty() && rndf(s) < 0.35f) {
                return fillTemplate(text::giftTemplates()[rnd32(s) % text::giftTemplates().size()], amount, "");
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
            g.message = pickGiftMessage(s, g.amount);
            g.t = 0.f;
            g.queued = false;
            O.activeGift = g;
            O.giftBusy = true;
            if (S.tts) {
                ttsEnqueue(g.user, g.amount, g.message, S.ttsDelay);
            }
            O.gift->setVisible(true);
            O.gift->setOpacity(0);
            O.gift->stopAllActions();
            float baseW = 250.f * S.scale;
            O.gift->setScale(0.9f);
            O.gift->setPositionX(CCDirector::get()->getWinSize().width);
            O.gift->runAction(CCEaseBackOut::create(CCScaleTo::create(0.28f, 1.f)));
            O.gift->runAction(CCEaseExponentialOut::create(CCFadeTo::create(0.22f, 255)));
        }

        void paintGift() {
            if (!O.giftBusy) {
                return;
            }
            auto const& g = O.activeGift;
            float gw = 250.f * S.scale;
            float gh = 84.f * S.scale;
            O.gift->setContentSize({ gw, gh });

            O.giftBg->clear();
            // card
            roundRect(O.giftBg, { 0.f, 0.f, gw, gh }, 7.f * S.scale, { 0.05f, 0.026f, 0.09f, 0.96f },
                std::max(1.f, 1.3f * S.scale), { 1.f, 1.f, 1.f, 0.85f });
            // the pink progress bar along the top, like the alerts people actually use
            float remain = std::clamp(1.f - g.t / g.life, 0.f, 1.f);
            O.giftBar->clear();
            O.giftBar->drawRect({ 3.f, gh - 5.f * S.scale, (gw - 6.f) * remain, 3.f * S.scale },
                { 0.85f, 0.19f, 0.55f, 0.95f });

            O.giftName->setScale(0.62f * S.scale);
            O.giftName->setColor(g.color);
            O.giftName->setString((g.user + " ").c_str());
            O.giftName->setPosition({ 10.f * S.scale, gh - 15.f * S.scale });
            float nx = 10.f * S.scale + textW(g.user + " ", 0.62f * S.scale, "chatFont.fnt");

            int bAmt = 0;
            // "donated $100" under the name, with the amount big
            char const* verb = "donated";
            auto* verbLabel = O.giftFoot;   // reused: the small line under the amount
            float ax = nx;
            // dollar + amount: goldFont so it reads as the headline
            std::string amt = "$" + number(g.amount);
            O.giftAmount->setScale(0.72f * S.scale);
            O.giftAmount->setColor({ 255, 214, 92 });
            O.giftAmount->setString(amt.c_str());
            O.giftAmount->setPosition({ ax, gh - 24.f * S.scale });
            (void)verbLabel;
            (void)verb;
            (void)bAmt;

            // message, wrapped to two lines
            float scale = 0.58f * S.scale;
            float lh = lineHeight(scale, "chatFont.fnt");
            float linesH = 0.f;
            auto wrapped = wrapText(g.message, scale, "chatFont.fnt", gw - 20.f * S.scale, &linesH);
            for (int i = 0; i < 2; ++i) {
                if (i < static_cast<int>(wrapped.size())) {
                    O.giftMsg[i]->setString(wrapped[static_cast<size_t>(i)].c_str());
                    O.giftMsg[i]->setScale(scale);
                    O.giftMsg[i]->setColor({ 232, 232, 242 });
                    O.giftMsg[i]->setPosition({ 10.f * S.scale, gh - 40.f * S.scale - static_cast<float>(i) * lh });
                    O.giftMsg[i]->setVisible(true);
                }
                else {
                    O.giftMsg[i]->setVisible(false);
                }
            }
            O.giftFoot->setString("DONATION");
            O.giftFoot->setScale(0.44f * S.scale);
            O.giftFoot->setColor({ 178, 168, 200 });
            O.giftFoot->setPosition({ 10.f * S.scale, 9.f * S.scale });
        }

        void updateGifts(float dt) {
            if (!O.gift) {
                return;
            }
            if (O.giftBusy) {
                O.activeGift.t += dt;
                paintGift();
                // slide out at the end
                if (O.activeGift.t > O.activeGift.life - 0.35f && O.gift->getOpacity() > 10) {
                    O.gift->stopAllActions();
                    O.gift->runAction(CCEaseExponentialIn::create(CCFadeTo::create(0.32f, 0)));
                }
                if (O.activeGift.t > O.activeGift.life) {
                    O.giftBusy = false;
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
            }
            else if (!S.viewersRandom) {
                O.viewerValue = lo;
            }
            else {
                if (O.viewerValue <= 0.f) {
                    O.viewerValue = lo + (hi - lo) * bell(gSeed);
                }
                O.viewerTimer -= dt;
                if (O.viewerTimer <= 0.f) {
                    O.viewerTimer = 2.5f + rndf(gSeed) * 6.f;
                    float t = S.viewersGradual ? lo + (hi - lo) * bell(gSeed) : rndInt(gSeed, static_cast<int>(lo), static_cast<int>(hi));
                    // a run going well pulls people in
                    t *= 1.f + std::clamp(gPct, 0.f, 100.f) / 100.f * 0.25f;
                    if (!S.viewersGradual) {
                        O.viewerValue = std::clamp(t, lo, hi);
                    }
                }
                if (S.viewersGradual) {
                    float target = std::clamp(lo + (hi - lo) * bell(gSeed), lo, hi);
                    O.viewerValue += (target - O.viewerValue) * std::min(1.f, dt * 0.35f);
                }
            }
            O.viewerValue = std::clamp(O.viewerValue, lo, hi);
            std::string text = number(static_cast<long long>(std::lround(O.viewerValue)));
            if (text != O.lastViewers) {
                O.lastViewers = text;
                O.viewersLabel->setString(text.c_str());
                float pw = 22.f + textW(text, 0.6f, "chatFont.fnt");
                O.viewers->setContentSize({ pw, 15.f });
                O.viewersBg->clear();
                roundRect(O.viewersBg, { 0.f, 0.f, pw, 15.f }, 7.5f, { 0.08f, 0.05f, 0.12f, 0.85f }, 0.6f,
                    { 1.f, 1.f, 1.f, 0.16f });
                O.viewersIcon->clear();
                circleFill(O.viewersIcon, { 8.f, 7.5f }, 2.4f, { 0.92f, 0.15f, 0.22f, 1.f }, 10);
                // keep the pill where the layout wants it
                auto r = panelRect();
                if (S.chat) {
                    O.viewers->setPosition({ r.getMaxX() - pw - 6.f, r.getMaxY() - 15.5f + 0.5f });
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

}  // namespace stream


namespace stream {

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
            std::thread worker;
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
            if (gTts.worker.joinable()) {
                return;
            }
            gTts.quit = false;
            gTts.worker = std::thread(ttsWorkerLoop);
            gTts.worker.detach();
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

}  // namespace stream


namespace stream {

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

        // ---- the chat clock
        if (S.chat) {
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

        updateViewers(dt);
        updateHeart(dt);
        updateGifts(dt);

        // a completed level holds its celebration for a while
        if (gComplete) {
            gCompleteT += dt;
        }
    }

    void onProgress(float pct) {
        gPct = std::clamp(pct, 0.f, 100.f);
        if (gPct > gAttemptMax) {
            gAttemptMax = gPct;
        }
        // a new personal best gets a one-off chat reaction (not every frame, not every attempt)
        if (gPct >= 100.f) {
            return;
        }
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
        // the big one: a wall of celebration that keeps going for a while
        pushEvent(text::C_CELEBRATE, 26, 3.5f);
        pushEvent(text::C_SPAM, 10, 3.0f);
        pushEvent(text::C_EMOTE, 8, 2.4f);
        // and a donation, because someone always does
        gNextGiftIn = std::min(gNextGiftIn, 2.5f);
        gComplete = true;
    }

    void onRestart() {
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

    void resetOverlay() {
        gPct = 0.f;
        gAttemptMax = 0.f;
        gComplete = false;
        gAttempts = 0;
        gEvents.clear();
        gBurst = 0.f;
        if (O.built) {
            O.lines.clear();
            relayoutRows();
            O.gifts.clear();
            O.giftBusy = false;
            if (O.gift) {
                O.gift->setVisible(false);
            }
        }
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
        O.bpmReal = std::min(214.f, O.bpmShown + 22.f);
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
        if (O.chip) {
            O.chip->setVisible(on);
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
        if (O.chip && O.chip->isVisible()) {
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
            gGrabOffset = { world.x - r.origin.x, world.y - r.origin.y };
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
        gDragging = false;
        gResizing = false;
        if (wasResize) {
            dirty = true;  // the menu shows the size, so let it refresh
        }
    }

    bool dragging() {
        return gDragging || gResizing;
    }

}  // namespace stream
