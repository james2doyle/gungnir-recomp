#pragma once

// SDL3 keyboard/mouse fallback for desktop development (T11a).
//
// The source-neutral core stays SDL-free; this adapter translates key and
// mouse-button events into InputState slots. Touch and gamepad producers land
// in T11b/T11d.

#if defined(GUNGNIR_HAVE_SDL3)

#include "input/keyboard_map.hpp"

#include <SDL3/SDL.h>

namespace gungnir::input {

class SdlInput {
public:
    SdlInput();
    explicit SdlInput(KeyboardMap map);

    // Feeds one event. Returns true when it was a key or mouse-button event.
    bool handle(const SDL_Event &event);
    // Releases the adapter's own held keys and mouse buttons.
    void release_held();

    [[nodiscard]] const KeyboardMap &map() const noexcept { return map_; }

private:
    KeyboardMap map_;
    ButtonMask keys_{};
    ButtonMask mouse_{};
};

} // namespace gungnir::input

#endif
