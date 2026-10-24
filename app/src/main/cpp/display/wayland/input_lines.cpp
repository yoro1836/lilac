#include "input_lines.h"

#include <cstdlib>
#include <sstream>

namespace vine::wl {

namespace {

// GuestProtocol actions (MotionEvent's)
enum { kDown = 0, kUp = 1, kMove = 2, kCancel = 3, kPointerDown = 5, kPointerUp = 6 };

constexpr uint32_t kKeyLeftShift = 42;

struct KeyMap {
    int keycode;
    uint32_t evdev;
};

// Android keycode -> evdev, as Generic.kl maps them back
const KeyMap kKeys[] = {
    {3, 172},   // HOME
    {4, 158},   // BACK
    {19, 103},  // DPAD_UP
    {20, 108},  // DPAD_DOWN
    {21, 105},  // DPAD_LEFT
    {22, 106},  // DPAD_RIGHT
    {24, 115},  // VOLUME_UP
    {25, 114},  // VOLUME_DOWN
    {55, 51},   // COMMA
    {56, 52},   // PERIOD
    {59, 42},   // SHIFT_LEFT
    {61, 15},   // TAB
    {62, 57},   // SPACE
    {66, 28},   // ENTER
    {67, 14},   // DEL
    {68, 41},   // GRAVE
    {69, 12},   // MINUS
    {70, 13},   // EQUALS
    {71, 26},   // LEFT_BRACKET
    {72, 27},   // RIGHT_BRACKET
    {73, 43},   // BACKSLASH
    {74, 39},   // SEMICOLON
    {75, 40},   // APOSTROPHE
    {76, 53},   // SLASH
    {82, 139},  // MENU
    {84, 217},  // SEARCH
    {111, 1},   // ESCAPE
    {112, 111}, // FORWARD_DEL
    {164, 113}, // VOLUME_MUTE
};

// Rows of the evdev letter keys
const char* const kLetterRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
const uint32_t kLetterRowStart[] = {16, 30, 44};

uint32_t letter_evdev(char c) {
    for (int r = 0; r < 3; ++r)
        for (const char* p = kLetterRows[r]; *p; ++p)
            if (*p == c) return kLetterRowStart[r] + (uint32_t)(p - kLetterRows[r]);
    return 0;
}

// evdev code and shift for a printable ASCII character, {0} if none
struct Stroke {
    uint32_t key;
    bool shift;
};

Stroke stroke_for(char c) {
    if (c >= 'a' && c <= 'z') return {letter_evdev(c), false};
    if (c >= 'A' && c <= 'Z') return {letter_evdev((char)(c - 'A' + 'a')), true};
    if (c >= '1' && c <= '9') return {(uint32_t)(2 + (c - '1')), false};
    if (c == '0') return {11, false};
    static const char kShiftedDigits[] = "!@#$%^&*()";
    for (int i = 0; i < 10; ++i)
        if (c == kShiftedDigits[i]) return {(uint32_t)(2 + i), true};
    switch (c) {
    case ' ': return {57, false};
    case '\n': return {28, false};
    case '\t': return {15, false};
    case '-': return {12, false};
    case '_': return {12, true};
    case '=': return {13, false};
    case '+': return {13, true};
    case '[': return {26, false};
    case '{': return {26, true};
    case ']': return {27, false};
    case '}': return {27, true};
    case ';': return {39, false};
    case ':': return {39, true};
    case '\'': return {40, false};
    case '"': return {40, true};
    case '`': return {41, false};
    case '~': return {41, true};
    case '\\': return {43, false};
    case '|': return {43, true};
    case ',': return {51, false};
    case '<': return {51, true};
    case '.': return {52, false};
    case '>': return {52, true};
    case '/': return {53, false};
    case '?': return {53, true};
    default: return {0, false};
    }
}

std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const char n = s[++i];
        out += n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n;
    }
    return out;
}

InputEvent key_event(uint32_t key, bool down) {
    InputEvent e{InputEvent::Key};
    e.key = key;
    e.down = down;
    return e;
}

bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

} // namespace

uint32_t evdev_for_keycode(int keycode) {
    if (keycode >= 29 && keycode <= 54) return letter_evdev((char)('a' + keycode - 29)); // A..Z
    if (keycode == 7) return 11;                                                       // 0
    if (keycode >= 8 && keycode <= 16) return (uint32_t)(2 + keycode - 8);             // 1..9
    for (const KeyMap& k : kKeys)
        if (k.keycode == keycode) return k.evdev;
    return 0;
}

bool parse_input_line(const std::string& line, std::vector<InputEvent>* out, std::string* error) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "touch") {
        int action = -1, index = -1, count = -1;
        in >> action >> index >> count;
        if (!in || count < 1 || count > 10 || index < 0 || index >= count) return fail(error, "bad touch line");
        struct Point {
            int32_t id;
            double x, y;
        };
        std::vector<Point> pts((size_t)count);
        for (Point& p : pts) {
            in >> p.id >> p.x >> p.y;
            if (!in) return fail(error, "bad touch point");
        }
        std::vector<InputEvent> evs;
        auto touch = [&](InputEvent::Kind kind, const Point& p) {
            InputEvent e{kind};
            e.id = p.id;
            e.x = p.x;
            e.y = p.y;
            evs.push_back(e);
        };
        switch (action) {
        case kDown:
        case kPointerDown: touch(InputEvent::TouchDown, pts[(size_t)index]); break;
        case kUp:
        case kPointerUp: touch(InputEvent::TouchUp, pts[(size_t)index]); break;
        case kMove:
            for (const Point& p : pts) touch(InputEvent::TouchMotion, p);
            break;
        case kCancel: evs.push_back(InputEvent{InputEvent::TouchCancel}); break;
        default: return fail(error, "unknown touch action " + std::to_string(action));
        }
        if (action != kCancel) evs.push_back(InputEvent{InputEvent::TouchFrame});
        out->insert(out->end(), evs.begin(), evs.end());
        return true;
    }
    if (verb == "key") {
        int keycode = -1, down = -1;
        in >> keycode >> down;
        if (!in || (down != 0 && down != 1)) return fail(error, "bad key line");
        const uint32_t key = evdev_for_keycode(keycode);
        if (!key) return fail(error, "no evdev key for keycode " + std::to_string(keycode));
        out->push_back(key_event(key, down == 1));
        return true;
    }
    if (verb == "text") {
        const size_t at = line.find("text ");
        const std::string text = unescape(at == std::string::npos ? std::string() : line.substr(at + 5));
        std::vector<InputEvent> evs;
        for (char c : text) {
            const Stroke s = stroke_for(c);
            if (!s.key) return fail(error, "cannot type character " + std::to_string((unsigned char)c));
            if (s.shift) evs.push_back(key_event(kKeyLeftShift, true));
            evs.push_back(key_event(s.key, true));
            evs.push_back(key_event(s.key, false));
            if (s.shift) evs.push_back(key_event(kKeyLeftShift, false));
        }
        out->insert(out->end(), evs.begin(), evs.end());
        return true;
    }
    return fail(error, "unknown input line: " + verb);
}

} // namespace vine::wl
