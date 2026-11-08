// FrameWave v3.4: macro player — list, keybind, and frame-accurate playback.
//
// Plays macros parsed by macro_formats (GDR2, MHR, xdBot) or the existing GDR v1 parser
// in main.cpp. Inputs are injected through the game's own button path (layer->handleButton),
// the way the autoplay macro bot does it, so physics sees them as real presses.
//
// The player is a singleton (macroplayer::get()). main.cpp wires it up:
//   - processCommands calls tick() every game step
//   - handleButton / menu call onKeyPress() / arm() / toggle()
//   - the Macros tab calls refreshList(), bindKey(), etc.
#pragma once

#include "macro_formats.hpp"

#include <functional>
#include <string>
#include <vector>

class PlayLayer;

namespace macroplayer {

    struct Entry {
        std::string file;           // filename in the macros directory
        std::string key;            // keybind string (e.g. "P") or empty
        macrofmt::MacroData data;   // parsed macro (loaded lazily)
        bool loaded = false;
        std::string error;          // parse error if loading failed
    };

    class Player {
    public:
        static Player& get();

        // ---- persistence ----
        void load();
        void save();

        // ---- list management ----
        void refreshList();                    // scan the macros directory
        bool loadEntry(int idx);               // parse a macro file into entries[idx].data
        void addFile(std::string const& filename);
        void removeEntry(int idx);

        // ---- keybinds ----
        void bindKey(int idx, std::string const& keyStr);
        std::string keyOf(int idx) const;
        int findByKey(std::string const& keyStr) const;  // -1 if none

        // ---- playback ----
        void arm(int idx);                      // load + arm a macro
        void toggle();                          // global on/off
        bool isActive() const { return enabled; }
        void stop();
        void onKeyPress(std::string const& keyStr);  // a key was pressed in-game

        // called every game step from processCommands.
        // `now` is the current game step number. `gameTps` is the physics TPS.
        // `inject(down, isP2)` sends a button press to the game (isP2 = player 2).
        void tick(PlayLayer* layer, int now, double gameTps, std::function<void(bool, bool)> inject);

        // ---- state (read by the menu) ----
        bool enabled = false;
        int activeIdx = -1;
        int captureIdx = -1;          // which entry is capturing a key (menu)
        std::vector<Entry> entries;

    private:
        void startPlayback();
        void stopPlayback();

        // playback state
        std::vector<macrofmt::Input> macroInputs;  // raw inputs from the macro
        std::vector<std::pair<int, bool>> ticks;   // (game tick, down) for P1
        std::vector<std::pair<int, bool>> ticks2;  // same for P2
        size_t playIdx = 0;
        size_t playIdx2 = 0;
        bool held = false;
        bool held2 = false;
        int startStep = -1;            // game step the macro started at (-1 = not started)
        bool playing = false;
        double macroFps = 240.0;
    };
}
