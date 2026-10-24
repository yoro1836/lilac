#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The guest agent's input lines (helper/GuestProtocol.kt), turned into
// Wayland seat events for vine_wl:
//
//   touch ACTION INDEX COUNT (ID X Y)*   X, Y: fractions of the screen
//   key KEYCODE 1|0                      Android keycode, down or up
//   text STRING                          escaped like HelperProtocol
namespace vine::wl {

struct InputEvent {
    enum Kind { TouchDown, TouchUp, TouchMotion, TouchFrame, TouchCancel, Key };
    Kind kind;
    int32_t id = 0;               // touch point
    double x = 0, y = 0;          // touch position, fractions
    uint32_t key = 0;             // evdev code
    bool down = false;
};

// Appends the line's events to `out`. false (and `error`) for a line it
// can't use; nothing is appended then.
bool parse_input_line(const std::string& line, std::vector<InputEvent>* out, std::string* error);

// evdev code for an Android keycode, 0 if there is none below the
// hwcomposer's limit (it keeps key state for codes < 239).
uint32_t evdev_for_keycode(int keycode);

} // namespace vine::wl
