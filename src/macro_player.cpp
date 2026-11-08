// FrameWave v3.4: macro player implementation.
#include "macro_player.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace macroplayer {

    namespace {
        Player* instance = nullptr;

        std::filesystem::path macroDir() {
            return Mod::get()->getConfigDir() / "macros";
        }

        std::filesystem::path saveFile() {
            return Mod::get()->getSaveDir() / "macro-player" / "list.txt";
        }

        // read a file into a byte vector
        bool readFile(std::filesystem::path const& path, std::vector<uint8_t>& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return false;
            }
            in.seekg(0, std::ios::end);
            auto size = in.tellg();
            in.seekg(0, std::ios::beg);
            if (size < 0) {
                return false;
            }
            out.resize(static_cast<size_t>(size));
            in.read(reinterpret_cast<char*>(out.data()), size);
            return true;
        }

        // escape a string for the simple text save format
        std::string escape(std::string const& s) {
            std::string out;
            for (char c : s) {
                if (c == '|' || c == '\n' || c == '\r') {
                    out += '\\';
                }
                out += c;
            }
            return out;
        }

        std::string unescape(std::string const& s) {
            std::string out;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                }
                out += s[i];
            }
            return out;
        }
    }

    Player& Player::get() {
        if (!instance) {
            instance = new Player();
        }
        return *instance;
    }

    // ---- persistence ----
    void Player::load() {
        std::error_code ec;
        std::ifstream in(saveFile(), std::ios::binary);
        if (!in) {
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            auto bar = line.find('|');
            Entry e;
            if (bar == std::string::npos) {
                e.file = unescape(line);
            } else {
                e.file = unescape(line.substr(0, bar));
                e.key = unescape(line.substr(bar + 1));
            }
            if (!e.file.empty()) {
                entries.push_back(std::move(e));
            }
        }
    }

    void Player::save() {
        std::error_code ec;
        std::filesystem::create_directories(saveFile().parent_path(), ec);
        std::ofstream out(saveFile(), std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << "# FrameWave macro player list\n";
        out << "# format: filename|keybind\n";
        for (auto const& e : entries) {
            out << escape(e.file) << "|" << escape(e.key) << "\n";
        }
    }

    // ---- list management ----
    void Player::refreshList() {
        // keep existing entries (with their keybinds), add new files, remove deleted ones
        std::error_code ec;
        std::filesystem::create_directories(macroDir(), ec);
        std::set<std::string> seen;
        for (auto const& e : entries) {
            seen.insert(e.file);
        }
        std::vector<std::string> newFiles;
        for (auto const& entry : std::filesystem::directory_iterator(macroDir(), ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            auto name = entry.path().filename().string();
            auto ext = entry.path().extension().string();
            // only known macro extensions
            if (ext != ".gdr" && ext != ".gdr2" && ext != ".mhr" && ext != ".json" && ext != ".xd") {
                continue;
            }
            if (seen.count(name) == 0) {
                newFiles.push_back(name);
            }
        }
        for (auto const& f : newFiles) {
            Entry e;
            e.file = f;
            entries.push_back(std::move(e));
        }
    }

    bool Player::loadEntry(int idx) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) {
            return false;
        }
        auto& e = entries[idx];
        if (e.loaded) {
            return true;
        }
        auto path = macroDir() / e.file;
        std::vector<uint8_t> data;
        if (!readFile(path, data)) {
            e.error = "file not found";
            return false;
        }
        macrofmt::MacroData m;
        std::string why;
        if (macrofmt::parseFormat(data, e.file, m, why)) {
            e.data = m;
            e.loaded = true;
            e.error.clear();
            return true;
        }
        e.error = why;
        return false;
    }

    void Player::addFile(std::string const& filename) {
        for (auto const& e : entries) {
            if (e.file == filename) {
                return;
            }
        }
        Entry e;
        e.file = filename;
        entries.push_back(std::move(e));
        save();
    }

    void Player::removeEntry(int idx) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) {
            return;
        }
        if (idx == activeIdx) {
            stop();
        }
        entries.erase(entries.begin() + idx);
        save();
    }

    // ---- keybinds ----
    void Player::bindKey(int idx, std::string const& keyStr) {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) {
            return;
        }
        entries[idx].key = keyStr;
        save();
    }

    std::string Player::keyOf(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(entries.size())) {
            return "";
        }
        return entries[idx].key;
    }

    int Player::findByKey(std::string const& keyStr) const {
        if (keyStr.empty()) {
            return -1;
        }
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (entries[i].key == keyStr) {
                return i;
            }
        }
        return -1;
    }

    // ---- playback ----
    void Player::arm(int idx) {
        if (!loadEntry(idx)) {
            return;
        }
        auto& e = entries[idx];
        if (!e.loaded) {
            return;
        }
        activeIdx = idx;
        enabled = true;
        macroFps = e.data.fps;
        macroInputs = e.data.inputs;
        ticks.clear();
        ticks2.clear();
        playIdx = 0;
        playIdx2 = 0;
        held = false;
        held2 = false;
        startStep = -1;  // set on first tick
        startPlayback();
    }

    void Player::toggle() {
        if (enabled) {
            stop();
        } else if (activeIdx >= 0) {
            enabled = true;
            startPlayback();
        }
    }

    void Player::stop() {
        enabled = false;
        playing = false;
        playIdx = 0;
        playIdx2 = 0;
        held = false;
        held2 = false;
        startStep = -1;
        activeIdx = -1;
        macroInputs.clear();
        ticks.clear();
        ticks2.clear();
    }

    void Player::onKeyPress(std::string const& keyStr) {
        if (keyStr.empty()) {
            return;
        }
        int idx = findByKey(keyStr);
        if (idx >= 0) {
            arm(idx);
        }
    }

    void Player::startPlayback() {
        playing = true;
        playIdx = 0;
        playIdx2 = 0;
    }

    void Player::stopPlayback() {
        playing = false;
        playIdx = 0;
        playIdx2 = 0;
        held = false;
        held2 = false;
    }

    void Player::tick(PlayLayer* layer, int now, double gameTps, std::function<void(bool, bool)> inject) {
        if (!playing || activeIdx < 0) {
            return;
        }
        // first tick: set the start step and convert frames to game ticks
        if (startStep < 0) {
            startStep = now;
            double k = gameTps / macroFps;
            ticks.clear();
            ticks2.clear();
            // separate P1 and P2 inputs, sorted by frame
            std::vector<std::pair<int, bool>> p1, p2;
            for (auto const& in : macroInputs) {
                (in.p2 ? p2 : p1).push_back({ in.frame, in.down });
            }
            auto convert = [k](std::vector<std::pair<int, bool>>& in, std::vector<std::pair<int, bool>>& out) {
                std::stable_sort(in.begin(), in.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
                bool lastDown = false;
                for (auto const& [frame, down] : in) {
                    if (down == lastDown) {
                        continue;
                    }
                    int t = static_cast<int>(std::llround(frame * k));
                    t = std::max(t, out.empty() ? 0 : out.back().first + 1);
                    out.push_back({ t, down });
                    lastDown = down;
                }
            };
            convert(p1, ticks);
            convert(p2, ticks2);
        }
        int elapsed = now - startStep;
        while (playIdx < ticks.size() && ticks[playIdx].first <= elapsed) {
            bool down = ticks[playIdx].second;
            if (down != held) {
                inject(down, false);
                held = down;
            }
            ++playIdx;
        }
        while (playIdx2 < ticks2.size() && ticks2[playIdx2].first <= elapsed) {
            bool down = ticks2[playIdx2].second;
            if (down != held2) {
                inject(down, true);
                held2 = down;
            }
            ++playIdx2;
        }
        if (playIdx >= ticks.size() && playIdx2 >= ticks2.size()) {
            stop();
        }
    }
}
