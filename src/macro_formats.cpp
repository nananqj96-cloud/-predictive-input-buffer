// FrameWave v3.4: macro format parsers (pure C++, no Geode).
// See macro_formats.hpp for the format specs.
#include "macro_formats.hpp"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>

namespace macrofmt {

    // ======================================================================
    // a byte reader with bounds checking
    // ======================================================================
    struct Reader {
        std::vector<uint8_t> const& data;
        size_t pos = 0;

        explicit Reader(std::vector<uint8_t> const& d) : data(d) {}

        bool eof() const { return pos >= data.size(); }
        size_t remaining() const { return data.size() - pos; }

        uint8_t u8() {
            if (pos >= data.size()) {
                return 0;
            }
            return data[pos++];
        }

        uint16_t u16be() {
            uint16_t v = 0;
            if (pos + 2 <= data.size()) {
                v = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
                pos += 2;
            }
            return v;
        }

        uint32_t u32be() {
            uint32_t v = 0;
            if (pos + 4 <= data.size()) {
                v = (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
                    (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
                pos += 4;
            }
            return v;
        }

        uint32_t u32le() {
            uint32_t v = 0;
            if (pos + 4 <= data.size()) {
                v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                    (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
                pos += 4;
            }
            return v;
        }

        float f32be() {
            uint32_t v = u32be();
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }

        double f64be() {
            uint64_t v = 0;
            if (pos + 8 <= data.size()) {
                for (int i = 0; i < 8; ++i) {
                    v = (v << 8) | data[pos + i];
                }
                pos += 8;
            }
            double f;
            std::memcpy(&f, &v, 8);
            return f;
        }

        // LEB128 varint (max 9 bytes). Returns false on overflow or EOF.
        bool varint(uint64_t& out) {
            out = 0;
            for (int i = 0; i < 9; ++i) {
                if (pos >= data.size()) {
                    return false;
                }
                uint8_t byte = data[pos++];
                out |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
                if ((byte & 0x80) == 0) {
                    return true;
                }
            }
            return false;
        }

        // read a varint-length-prefixed UTF-8 string
        bool str(std::string& out) {
            uint64_t len;
            if (!varint(len)) {
                return false;
            }
            if (len > 0xFFFF || pos + len > data.size()) {
                return false;
            }
            out.assign(reinterpret_cast<char const*>(data.data() + pos), static_cast<size_t>(len));
            pos += static_cast<size_t>(len);
            return true;
        }

        bool skip(size_t n) {
            if (pos + n > data.size()) {
                return false;
            }
            pos += n;
            return true;
        }
    };

    // ======================================================================
    // format detection
    // ======================================================================
    Format detectFormat(std::vector<uint8_t> const& data, std::string const& filename) {
        // by extension first
        auto lower = filename;
        for (auto& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".gdr2") == 0) {
            return Format::Gdr2;
        }
        if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".mhr.json") == 0) {
            return Format::MhrJson;
        }
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".json") == 0) {
            // could be MHR JSON or GDR v1 JSON; check contents
            if (!data.empty() && data[0] == '{') {
                // try to detect MHR JSON by looking for "events" key
                // (GDR v1 JSON has "inputs", MHR has "events")
                std::string text(reinterpret_cast<char const*>(data.data()), data.size());
                if (text.find("\"events\"") != std::string::npos) {
                    return Format::MhrJson;
                }
            }
        }
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".mhr") == 0) {
            return Format::MhrBin;
        }
        if (lower.size() >= 3 && lower.compare(lower.size() - 3, 3, ".xd") == 0) {
            return Format::XdBot;
        }

        // by magic bytes
        if (data.size() >= 3 && data[0] == 'G' && data[1] == 'D' && data[2] == 'R') {
            // GDR2 if the next byte encodes version 2, or extension follows
            // GDR v1 binary uses MsgPack (first byte 0x81-0x83 for maps, or 0x82)
            // GDR2 starts with "GDR" then a varint version
            // We can't distinguish GDR v1 msgpack from GDR2 by magic alone here,
            // but GDR2 has magic "GDR" as the first 3 bytes. GDR v1 msgpack starts
            // with a msgpack map marker (0x81-0x8f).
            // Actually the existing parseMacro already handles GDR v1 (JSON/MsgPack).
            // So if we see "GDR" magic, it's GDR2.
            return Format::Gdr2;
        }
        if (data.size() >= 4) {
            uint32_t magic = (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
                             (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
            if (magic == 0x4841434B) {  // "HACK"
                return Format::MhrBin;
            }
        }
        if (!data.empty() && (data[0] == '{' || data[0] == '[')) {
            // JSON — try MHR
            std::string text(reinterpret_cast<char const*>(data.data()), data.size());
            if (text.find("\"events\"") != std::string::npos) {
                return Format::MhrJson;
            }
        }
        // xdBot is text: first line is a number (fps)
        if (!data.empty() && data[0] >= '0' && data[0] <= '9') {
            return Format::XdBot;
        }
        return Format::Unsupported;
    }

    // ======================================================================
    // dispatch
    // ======================================================================
    bool parseFormat(std::vector<uint8_t> const& data, std::string const& filename,
        MacroData& out, std::string& why) {
        if (data.empty()) {
            why = "empty file";
            return false;
        }
        auto fmt = detectFormat(data, filename);
        switch (fmt) {
            case Format::Gdr2:
                return parseGdr2(data, out, why);
            case Format::MhrJson:
                return parseMhrJson(data, out, why);
            case Format::MhrBin:
                return parseMhrBin(data, out, why);
            case Format::XdBot:
                return parseXdBot(data, out, why);
            default:
                why = "unrecognized macro format";
                return false;
        }
    }

    // ======================================================================
    // GDR2 binary parser
    // ======================================================================
    bool parseGdr2(std::vector<uint8_t> const& data, MacroData& out, std::string& why) {
        Reader r(data);
        // magic
        if (r.remaining() < 3 || r.data[r.pos] != 'G' || r.data[r.pos + 1] != 'D' || r.data[r.pos + 2] != 'R') {
            why = "not a GDR2 file (bad magic)";
            return false;
        }
        r.pos += 3;
        // version
        uint64_t version;
        if (!r.varint(version) || version != 2) {
            why = "unsupported GDR version " + std::to_string(version);
            return false;
        }
        // extension tag
        std::string extTag;
        if (!r.str(extTag)) {
            why = "GDR2: truncated extension tag";
            return false;
        }
        bool hasExt = !extTag.empty();
        // metadata
        if (!r.str(out.author)) { why = "GDR2: truncated author"; return false; }
        std::string desc;
        if (!r.str(desc)) { why = "GDR2: truncated description"; return false; }
        out.fps = r.f32be();
        uint64_t gv;
        if (!r.varint(gv)) { why = "GDR2: truncated game version"; return false; }
        (void)gv;
        out.fps = r.f64be();
        uint64_t seed, coins;
        if (!r.varint(seed)) { why = "GDR2: truncated seed"; return false; }
        if (!r.varint(coins)) { why = "GDR2: truncated coins"; return false; }
        (void)seed; (void)coins;
        out.ldm = r.u8() != 0;
        out.platformer = r.u8() != 0;
        if (!r.str(out.bot)) { why = "GDR2: truncated bot name"; return false; }
        uint64_t botVer;
        if (!r.varint(botVer)) { why = "GDR2: truncated bot version"; return false; }
        (void)botVer;
        uint64_t levelId;
        if (!r.varint(levelId)) { why = "GDR2: truncated level id"; return false; }
        out.levelId = static_cast<int>(levelId);
        if (!r.str(out.levelName)) { why = "GDR2: truncated level name"; return false; }
        // skip extension section
        uint64_t extSize;
        if (!r.varint(extSize) || !r.skip(static_cast<size_t>(extSize))) {
            why = "GDR2: truncated extension section";
            return false;
        }
        // deaths
        uint64_t deathCount;
        if (!r.varint(deathCount)) { why = "GDR2: truncated death count"; return false; }
        uint64_t prevDeath = 0;
        for (uint64_t i = 0; i < deathCount; ++i) {
            uint64_t delta;
            if (!r.varint(delta)) { why = "GDR2: truncated death list"; return false; }
            prevDeath += delta;
        }
        (void)prevDeath;
        // inputs
        uint64_t totalInputs, p1Inputs;
        if (!r.varint(totalInputs) || !r.varint(p1Inputs)) {
            why = "GDR2: truncated input count";
            return false;
        }
        uint64_t p1Remaining = p1Inputs;
        uint64_t prevFrame = 0;
        size_t readCount = 0;
        while (r.remaining() > 0 && readCount < totalInputs) {
            uint64_t packed;
            if (!r.varint(packed)) {
                break;
            }
            uint64_t delta = out.platformer ? (packed >> 3) : (packed >> 1);
            uint64_t frame = prevFrame + delta;
            bool down = (packed & 1) != 0;
            int button = out.platformer ? static_cast<int>((packed >> 1) & 3) : 1;
            bool isP2 = (p1Remaining == 0);
            Input in;
            in.frame = static_cast<int>(frame);
            in.down = down;
            in.p2 = isP2;
            in.button = button;
            out.inputs.push_back(in);
            out.lastFrame = std::max(out.lastFrame, in.frame);
            if (isP2) {
                out.twoPlayer = true;
            }
            // extension data
            if (hasExt) {
                uint64_t extLen;
                if (r.varint(extLen)) {
                    r.skip(static_cast<size_t>(extLen));
                }
            }
            prevFrame = frame;
            ++readCount;
            if (p1Remaining > 0) {
                --p1Remaining;
                if (p1Remaining == 0) {
                    prevFrame = 0;
                }
            }
        }
        if (out.inputs.empty()) {
            why = "GDR2: no inputs";
            return false;
        }
        if (!(out.fps >= 30.0 && out.fps <= 100000.0)) {
            out.fps = 240.0;
        }
        std::stable_sort(out.inputs.begin(), out.inputs.end(),
            [](Input const& a, Input const& b) { return a.frame < b.frame; });
        out.file = "";
        return true;
    }

    // ======================================================================
    // MHR JSON parser
    // ======================================================================
    bool parseMhrJson(std::vector<uint8_t> const& data, MacroData& out, std::string& why) {
        std::string text(reinterpret_cast<char const*>(data.data()), data.size());
        JsonValue root;
        if (!jsonParse(text, root)) {
            why = "MHR: not valid JSON";
            return false;
        }
        auto* meta = root.find("meta");
        if (!meta) {
            why = "MHR: no meta field";
            return false;
        }
        auto* fpsVal = meta->find("fps");
        if (fpsVal) {
            out.fps = fpsVal->num;
        }
        auto* events = root.find("events");
        if (!events || events->type != JsonValue::Array) {
            why = "MHR: no events array";
            return false;
        }
        for (auto const& ev : events->arr) {
            auto* frameVal = ev.find("frame");
            auto* downVal = ev.find("down");
            if (!frameVal || !downVal) {
                continue;
            }
            Input in;
            in.frame = static_cast<int>(frameVal->num);
            in.down = downVal->b;
            auto* p2Val = ev.find("p2");
            in.p2 = p2Val ? p2Val->b : false;
            out.inputs.push_back(in);
            out.lastFrame = std::max(out.lastFrame, in.frame);
            if (in.p2) {
                out.twoPlayer = true;
            }
        }
        if (out.inputs.empty()) {
            why = "MHR: no inputs";
            return false;
        }
        if (!(out.fps >= 30.0 && out.fps <= 100000.0)) {
            out.fps = 240.0;
        }
        std::stable_sort(out.inputs.begin(), out.inputs.end(),
            [](Input const& a, Input const& b) { return a.frame < b.frame; });
        return true;
    }

    // ======================================================================
    // MHR binary parser
    // ======================================================================
    bool parseMhrBin(std::vector<uint8_t> const& data, MacroData& out, std::string& why) {
        Reader r(data);
        // magic: "HACK" big-endian
        if (r.remaining() < 4) {
            why = "MHR: file too short";
            return false;
        }
        uint32_t magic = r.u32be();
        if (magic != 0x4841434B) {
            why = "MHR: bad magic (not a Mega Hack replay)";
            return false;
        }
        // fps at offset 12 (little-endian u32)
        if (r.pos + 12 > data.size()) {
            why = "MHR: truncated header";
            return false;
        }
        r.pos = 12;
        out.fps = static_cast<double>(r.u32le());
        // num_actions at offset 28 (little-endian u32)
        if (r.pos + 28 > data.size()) {
            why = "MHR: truncated header";
            return false;
        }
        r.pos = 28;
        uint32_t numActions = r.u32le();
        // each action is 32 bytes: skip 2, down(1), player(1), frame(4 LE), skip 24
        constexpr size_t actionSize = 32;
        if (r.pos + static_cast<size_t>(numActions) * actionSize > data.size()) {
            why = "MHR: truncated actions";
            return false;
        }
        for (uint32_t i = 0; i < numActions; ++i) {
            r.pos += 2;  // skip
            bool down = r.u8() == 1;
            bool isP1 = r.u8() == 0;  // 0 = P1, 1 = P2
            int frame = static_cast<int>(r.u32le());
            r.pos += 24;  // skip physics
            Input in;
            in.frame = frame;
            in.down = down;
            in.p2 = !isP1;
            in.button = 1;
            out.inputs.push_back(in);
            out.lastFrame = std::max(out.lastFrame, frame);
            if (in.p2) {
                out.twoPlayer = true;
            }
        }
        if (out.inputs.empty()) {
            why = "MHR: no inputs";
            return false;
        }
        if (!(out.fps >= 30.0 && out.fps <= 100000.0)) {
            out.fps = 240.0;
        }
        std::stable_sort(out.inputs.begin(), out.inputs.end(),
            [](Input const& a, Input const& b) { return a.frame < b.frame; });
        return true;
    }

    // ======================================================================
    // xdBot text parser
    // ======================================================================
    bool parseXdBot(std::vector<uint8_t> const& data, MacroData& out, std::string& why) {
        std::string text(reinterpret_cast<char const*>(data.data()), data.size());
        // split into lines
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
                if (i > start) {
                    lines.push_back(text.substr(start, i - start));
                }
                start = i + 1;
            }
        }
        if (lines.empty()) {
            why = "xdBot: empty file";
            return false;
        }
        // first line: fps
        double fps = 240.0;
        {
            auto& first = lines[0];
            // trim
            while (!first.empty() && (first.back() == ' ' || first.back() == '\t')) {
                first.pop_back();
            }
            fps = std::atof(first.c_str());
            if (fps <= 0) {
                fps = 240.0;
            }
        }
        out.fps = fps;
        // action lines: frame|holding|button|player1|pos_only|...
        for (size_t i = 1; i < lines.size(); ++i) {
            auto& line = lines[i];
            if (line.empty()) {
                continue;
            }
            // split by '|'
            std::vector<std::string> fields;
            size_t s = 0;
            for (size_t j = 0; j <= line.size(); ++j) {
                if (j == line.size() || line[j] == '|') {
                    fields.push_back(line.substr(s, j - s));
                    s = j + 1;
                }
            }
            if (fields.size() < 4) {
                continue;
            }
            int frame = std::atoi(fields[0].c_str());
            bool holding = std::atoi(fields[1].c_str()) == 1;
            int button = std::atoi(fields[2].c_str());
            bool isP1 = std::atoi(fields[3].c_str()) == 1;
            Input in;
            in.frame = frame;
            in.down = holding;
            in.p2 = !isP1;
            in.button = (button == 2 || button == 3) ? button : 1;
            out.inputs.push_back(in);
            out.lastFrame = std::max(out.lastFrame, frame);
            if (in.p2) {
                out.twoPlayer = true;
            }
        }
        if (out.inputs.empty()) {
            why = "xdBot: no inputs";
            return false;
        }
        std::stable_sort(out.inputs.begin(), out.inputs.end(),
            [](Input const& a, Input const& b) { return a.frame < b.frame; });
        return true;
    }

    // ======================================================================
    // minimal JSON parser
    // ======================================================================
    namespace {
        struct JParser {
            std::string const& s;
            size_t pos = 0;
            bool ok = true;

            explicit JParser(std::string const& str) : s(str) {}

            void skipWs() {
                while (pos < s.size()) {
                    char c = s[pos];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                        ++pos;
                    } else {
                        break;
                    }
                }
            }

            bool parseValue(JsonValue& v) {
                skipWs();
                if (pos >= s.size()) {
                    ok = false;
                    return false;
                }
                char c = s[pos];
                if (c == '{') return parseObject(v);
                if (c == '[') return parseArray(v);
                if (c == '"') {
                    v.type = JsonValue::String;
                    return parseString(v.str);
                }
                if (c == 't' || c == 'f') return parseBool(v);
                if (c == 'n') return parseNull(v);
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(v);
                ok = false;
                return false;
            }

            bool parseObject(JsonValue& v) {
                v.type = JsonValue::Object;
                ++pos;  // {
                skipWs();
                if (pos < s.size() && s[pos] == '}') {
                    ++pos;
                    return true;
                }
                while (pos < s.size()) {
                    skipWs();
                    std::string key;
                    if (!parseString(key)) {
                        ok = false;
                        return false;
                    }
                    skipWs();
                    if (pos >= s.size() || s[pos] != ':') {
                        ok = false;
                        return false;
                    }
                    ++pos;
                    JsonValue val;
                    if (!parseValue(val)) {
                        ok = false;
                        return false;
                    }
                    v.obj.emplace_back(std::move(key), std::move(val));
                    skipWs();
                    if (pos < s.size() && s[pos] == ',') {
                        ++pos;
                        continue;
                    }
                    if (pos < s.size() && s[pos] == '}') {
                        ++pos;
                        return true;
                    }
                    ok = false;
                    return false;
                }
                ok = false;
                return false;
            }

            bool parseArray(JsonValue& v) {
                v.type = JsonValue::Array;
                ++pos;  // [
                skipWs();
                if (pos < s.size() && s[pos] == ']') {
                    ++pos;
                    return true;
                }
                while (pos < s.size()) {
                    JsonValue val;
                    if (!parseValue(val)) {
                        ok = false;
                        return false;
                    }
                    v.arr.push_back(std::move(val));
                    skipWs();
                    if (pos < s.size() && s[pos] == ',') {
                        ++pos;
                        continue;
                    }
                    if (pos < s.size() && s[pos] == ']') {
                        ++pos;
                        return true;
                    }
                    ok = false;
                    return false;
                }
                ok = false;
                return false;
            }

            bool parseString(std::string& out) {
                if (pos >= s.size() || s[pos] != '"') {
                    ok = false;
                    return false;
                }
                ++pos;
                while (pos < s.size()) {
                    char c = s[pos];
                    if (c == '"') {
                        ++pos;
                        return true;
                    }
                    if (c == '\\' && pos + 1 < s.size()) {
                        ++pos;
                        char e = s[pos];
                        switch (e) {
                            case '"': out += '"'; break;
                            case '\\': out += '\\'; break;
                            case '/': out += '/'; break;
                            case 'n': out += '\n'; break;
                            case 't': out += '\t'; break;
                            case 'r': out += '\r'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            case 'u': {
                                // skip 4 hex digits (we don't decode unicode)
                                pos += 4;
                                break;
                            }
                            default: out += e; break;
                        }
                        ++pos;
                    } else {
                        out += c;
                        ++pos;
                    }
                }
                ok = false;
                return false;
            }

            bool parseNumber(JsonValue& v) {
                size_t start = pos;
                if (pos < s.size() && s[pos] == '-') {
                    ++pos;
                }
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                    ++pos;
                }
                if (pos < s.size() && s[pos] == '.') {
                    ++pos;
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                        ++pos;
                    }
                }
                if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                    ++pos;
                    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
                        ++pos;
                    }
                    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                        ++pos;
                    }
                }
                v.type = JsonValue::Number;
                v.num = std::atof(s.substr(start, pos - start).c_str());
                return true;
            }

            bool parseBool(JsonValue& v) {
                if (s.substr(pos, 4) == "true") {
                    pos += 4;
                    v.type = JsonValue::Bool;
                    v.b = true;
                    return true;
                }
                if (s.substr(pos, 5) == "false") {
                    pos += 5;
                    v.type = JsonValue::Bool;
                    v.b = false;
                    return true;
                }
                ok = false;
                return false;
            }

            bool parseNull(JsonValue& v) {
                if (s.substr(pos, 4) == "null") {
                    pos += 4;
                    v.type = JsonValue::Null;
                    return true;
                }
                ok = false;
                return false;
            }
        };
    }

    bool jsonParse(std::string const& text, JsonValue& out) {
        JParser p(text);
        if (!p.parseValue(out)) {
            return false;
        }
        p.skipWs();
        return p.ok;
    }

}  // namespace macrofmt
