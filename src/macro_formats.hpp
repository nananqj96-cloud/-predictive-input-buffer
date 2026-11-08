// FrameWave v3.4: macro file format detection and parsing.
//
// Pure C++ (no Geode, no GD): every parser here works on a byte buffer and produces a
// MacroData. That keeps it testable with a standalone harness in /tmp, the way the chat
// database is tested.
//
// Supported formats (detected by extension and/or magic bytes):
//   .gdr2  GDR2 binary   (Mega Hack, xdBot new)   — magic "GDR", varint v2
//   .mhr   MHR binary    (Mega Hack)              — magic "HACK"
//   .mhr.json / .json   MHR JSON (Mega Hack)       — {meta:{fps}, events:[...]}
//   .xd    xdBot text   (xdBot)                   — "fps\nframe|holding|button|player..."
//   .gdr   GDR v1        (Eclipse, old xdBot)     — JSON or MsgPack (handled by main.cpp's
//                                                     existing parseMacro, not here)
//
// The dispatch function parseFormat() tries each known binary/text format in turn. If none
// match, it returns false and the caller falls back to the GDR v1 parser in main.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace macrofmt {

    // one input event in a macro: a button state change at a frame
    struct Input {
        int frame = 0;       // frame number in the macro's own framerate
        bool down = false;   // true = press, false = release
        bool p2 = false;     // true = player 2
        int button = 1;      // 1 = jump, 2 = left, 3 = right (platformer)
    };

    struct MacroData {
        std::string file;        // filename
        std::string author;
        std::string bot;
        std::string levelName;
        int levelId = 0;
        double fps = 240.0;     // the framerate the macro was recorded at
        bool ldm = false;       // low detail mode
        bool platformer = false;
        std::vector<Input> inputs;   // sorted by frame
        int lastFrame = 0;
        bool twoPlayer = false;  // has player-2 inputs
    };

    enum class Format {
        Gdr2,       // .gdr2 binary
        MhrJson,    // .mhr.json / .json
        MhrBin,     // .mhr binary
        XdBot,      // .xd text
        Unsupported,
    };

    // detect the format from the file contents and/or filename
    Format detectFormat(std::vector<uint8_t> const& data, std::string const& filename);

    // parse a macro from raw bytes. Returns true on success. On failure, `why` explains why.
    bool parseFormat(std::vector<uint8_t> const& data, std::string const& filename,
        MacroData& out, std::string& why);

    // ---- individual parsers (called by parseFormat) ----
    bool parseGdr2(std::vector<uint8_t> const& data, MacroData& out, std::string& why);
    bool parseMhrJson(std::vector<uint8_t> const& data, MacroData& out, std::string& why);
    bool parseMhrBin(std::vector<uint8_t> const& data, MacroData& out, std::string& why);
    bool parseXdBot(std::vector<uint8_t> const& data, MacroData& out, std::string& why);

    // a minimal JSON value + parser (for MHR JSON). Not a full JSON implementation: it
    // handles objects, arrays, numbers, booleans, strings and null, which is what MHR needs.
    struct JsonValue {
        enum Type { Null, Bool, Number, String, Array, Object };
        Type type = Null;
        bool b = false;
        double num = 0.0;
        std::string str;
        std::vector<JsonValue> arr;
        std::vector<std::pair<std::string, JsonValue>> obj;

        JsonValue const* find(std::string const& key) const {
            if (type != Object) {
                return nullptr;
            }
            for (auto const& [k, v] : obj) {
                if (k == key) {
                    return &v;
                }
            }
            return nullptr;
        }
    };

    // parse a JSON document. Returns true on success.
    bool jsonParse(std::string const& text, JsonValue& out);
}
