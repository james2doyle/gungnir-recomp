#include "input/sdl_input.hpp"

#if defined(GUNGNIR_HAVE_SDL3)

#include "input/input_state.hpp"

#include <utility>

namespace gungnir::input {
namespace {

static_assert(SDL_SCANCODE_A == scancode::A);
static_assert(SDL_SCANCODE_D == scancode::D);
static_assert(SDL_SCANCODE_E == scancode::E);
static_assert(SDL_SCANCODE_I == scancode::I);
static_assert(SDL_SCANCODE_J == scancode::J);
static_assert(SDL_SCANCODE_K == scancode::K);
static_assert(SDL_SCANCODE_L == scancode::L);
static_assert(SDL_SCANCODE_Q == scancode::Q);
static_assert(SDL_SCANCODE_S == scancode::S);
static_assert(SDL_SCANCODE_W == scancode::W);
static_assert(SDL_SCANCODE_RETURN == scancode::Return);
static_assert(SDL_SCANCODE_BACKSPACE == scancode::Backspace);
static_assert(SDL_SCANCODE_RIGHT == scancode::Right);
static_assert(SDL_SCANCODE_LEFT == scancode::Left);
static_assert(SDL_SCANCODE_DOWN == scancode::Down);
static_assert(SDL_SCANCODE_UP == scancode::Up);

} // namespace

SdlInput::SdlInput() : map_(KeyboardMap::defaults()) {}

SdlInput::SdlInput(KeyboardMap map) : map_(std::move(map)) {}

bool SdlInput::handle(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (event.key.repeat) return true;
        const ButtonMask mask = map_.key_mask(static_cast<int>(event.key.scancode));
        if (mask != 0u) {
            if (event.type == SDL_EVENT_KEY_DOWN)
                keys_ |= mask;
            else
                keys_ &= ~mask;
            state().set_buttons(Source::Keyboard, 0, 0, keys_);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const int button = static_cast<int>(event.button.button);
        const ButtonMask mask = map_.mouse_mask(button);
        if (mask != 0u) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                mouse_ |= mask;
            else
                mouse_ &= ~mask;
            state().set_buttons(Source::Mouse, 0, 0, mouse_);
        }
        return true;
    }
    default:
        return false;
    }
}

void SdlInput::release_held() {
    keys_ = 0u;
    mouse_ = 0u;
    state().release(Source::Keyboard, 0, 0);
    state().release(Source::Mouse, 0, 0);
}

} // namespace gungnir::input

#endif
