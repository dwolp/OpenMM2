#include "platform/Input.h"

#include "core/Log.h"
#include "core/StringUtil.h"
#include "platform/Window.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace mm2::platform {

static_assert(static_cast<int>(Key::A) == SDL_SCANCODE_A);
static_assert(static_cast<int>(Key::Num1) == SDL_SCANCODE_1);
static_assert(static_cast<int>(Key::Return) == SDL_SCANCODE_RETURN);
static_assert(static_cast<int>(Key::Semicolon) == SDL_SCANCODE_SEMICOLON);
static_assert(static_cast<int>(Key::F1) == SDL_SCANCODE_F1);
static_assert(static_cast<int>(Key::PrintScreen) == SDL_SCANCODE_PRINTSCREEN);
static_assert(static_cast<int>(Key::Right) == SDL_SCANCODE_RIGHT);
static_assert(static_cast<int>(Key::NumLock) == SDL_SCANCODE_NUMLOCKCLEAR);
static_assert(static_cast<int>(Key::Kp1) == SDL_SCANCODE_KP_1);
static_assert(static_cast<int>(Key::KpPeriod) == SDL_SCANCODE_KP_PERIOD);
static_assert(static_cast<int>(Key::LCtrl) == SDL_SCANCODE_LCTRL);
static_assert(static_cast<int>(Key::RGui) == SDL_SCANCODE_RGUI);
static_assert(kKeyCount == SDL_SCANCODE_COUNT);
static_assert(static_cast<int>(GamepadButton::Count) == SDL_GAMEPAD_BUTTON_COUNT);
static_assert(static_cast<int>(GamepadAxis::Count) == SDL_GAMEPAD_AXIS_COUNT);

std::string keyName(Key key) {
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(key));
    return n ? n : "";
}

Key keyFromName(std::string_view name) {
    const std::string s(name);
    const SDL_Scancode sc = SDL_GetScancodeFromName(s.c_str());
    return static_cast<Key>(sc);
}

Input::Input() = default;

Input::~Input() {
    m_ff.reset(); // before its joystick closes
    for (void* g : m_gamepadHandles)
        SDL_CloseGamepad(static_cast<SDL_Gamepad*>(g));
    for (void* j : m_joystickHandles)
        SDL_CloseJoystick(static_cast<SDL_Joystick*>(j));
}

void Input::beginFrame() {
    m_keysPressed.reset();
    m_keysReleased.reset();
    m_pressOrder.clear();
    m_mouseDelta = {};
    m_wheel = {};
    m_mousePressed = 0;
    m_text.clear();
    m_devicesChanged = false;
    for (auto& g : m_gamepads)
        g.pressed.reset();
}

void Input::handleEvent(const SDL_Event& ev) {
    auto density = [](SDL_WindowID id) {
        SDL_Window* w = SDL_GetWindowFromID(id);
        const float d = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
        return d > 0.0f ? d : 1.0f;
    };

    switch (ev.type) {
    case SDL_EVENT_KEY_DOWN: {
        const int sc = ev.key.scancode & (kKeyCount - 1);
        if (!ev.key.repeat && !m_keys.test(sc)) {
            m_keysPressed.set(sc);
            m_pressOrder.push_back(static_cast<Key>(sc));
        }
        m_keys.set(sc);
        break;
    }
    case SDL_EVENT_KEY_UP: {
        const int sc = ev.key.scancode & (kKeyCount - 1);
        m_keys.reset(sc);
        m_keysReleased.set(sc);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (ev.text.text)
            m_text += ev.text.text;
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        const float d = density(ev.motion.windowID);
        m_mousePos = {ev.motion.x * d, ev.motion.y * d};
        m_mouseDelta += Vec2{ev.motion.xrel * d, ev.motion.yrel * d};
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        m_mouseButtons |= 1u << ev.button.button;
        m_mousePressed |= 1u << ev.button.button;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP: m_mouseButtons &= ~(1u << ev.button.button); break;
    case SDL_EVENT_MOUSE_WHEEL: {
        const float sign = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        m_wheel += Vec2{ev.wheel.x * sign, ev.wheel.y * sign};
        break;
    }
    case SDL_EVENT_JOYSTICK_ADDED: openDevice(ev.jdevice.which); break;
    case SDL_EVENT_JOYSTICK_REMOVED: closeDevice(ev.jdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMAPPED:
        // A mapping arrived for a device we opened as a plain joystick.
        closeDevice(ev.gdevice.which);
        openDevice(ev.gdevice.which);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Releasing keys avoids "stuck" inputs after alt-tab.
        m_keys.reset();
        m_mouseButtons = 0;
        break;
    default: break;
    }
}

void Input::openDevice(std::uint32_t id) {
    const auto hasId = [id](const auto& s) { return s.id == id; };
    if (std::ranges::any_of(m_gamepads, hasId) || std::ranges::any_of(m_joysticks, hasId))
        return;
    if (SDL_IsGamepad(id)) {
        SDL_Gamepad* g = SDL_OpenGamepad(id);
        if (!g) {
            log::warn("input: cannot open gamepad {}: {}", id, SDL_GetError());
            return;
        }
        GamepadState s;
        s.id = id;
        s.name = SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "Gamepad";
        s.hasRumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        log::info("input: gamepad connected: {}", s.name);
        m_gamepads.push_back(std::move(s));
        m_gamepadHandles.push_back(g);
    } else {
        SDL_Joystick* j = SDL_OpenJoystick(id);
        if (!j) {
            log::warn("input: cannot open joystick {}: {}", id, SDL_GetError());
            return;
        }
        JoystickState s;
        s.id = id;
        s.name = SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "Joystick";
        s.axes.resize(static_cast<std::size_t>(std::max(0, SDL_GetNumJoystickAxes(j))));
        s.buttons.resize(static_cast<std::size_t>(std::max(0, SDL_GetNumJoystickButtons(j))));
        s.hats.resize(static_cast<std::size_t>(std::max(0, SDL_GetNumJoystickHats(j))));
        s.hasRumble =
            SDL_GetBooleanProperty(SDL_GetJoystickProperties(j), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
        log::info("input: joystick connected: {} ({} axes, {} buttons)", s.name, s.axes.size(), s.buttons.size());
        m_joysticks.push_back(std::move(s));
        m_joystickHandles.push_back(j);
    }
    m_devicesChanged = true;
}

void Input::closeDevice(std::uint32_t id) {
    if (m_ff && m_ffId == id)
        m_ff.reset();
    for (std::size_t i = 0; i < m_gamepads.size(); ++i) {
        if (m_gamepads[i].id == id) {
            log::info("input: gamepad disconnected: {}", m_gamepads[i].name);
            SDL_CloseGamepad(static_cast<SDL_Gamepad*>(m_gamepadHandles[i]));
            m_gamepads.erase(m_gamepads.begin() + static_cast<std::ptrdiff_t>(i));
            m_gamepadHandles.erase(m_gamepadHandles.begin() + static_cast<std::ptrdiff_t>(i));
            m_devicesChanged = true;
            return;
        }
    }
    for (std::size_t i = 0; i < m_joysticks.size(); ++i) {
        if (m_joysticks[i].id == id) {
            log::info("input: joystick disconnected: {}", m_joysticks[i].name);
            SDL_CloseJoystick(static_cast<SDL_Joystick*>(m_joystickHandles[i]));
            m_joysticks.erase(m_joysticks.begin() + static_cast<std::ptrdiff_t>(i));
            m_joystickHandles.erase(m_joystickHandles.begin() + static_cast<std::ptrdiff_t>(i));
            m_devicesChanged = true;
            return;
        }
    }
}

void Input::updateDevices() {
    for (std::size_t i = 0; i < m_gamepads.size(); ++i) {
        auto* g = static_cast<SDL_Gamepad*>(m_gamepadHandles[i]);
        auto& s = m_gamepads[i];
        for (int a = 0; a < static_cast<int>(GamepadAxis::Count); ++a) {
            const float v = static_cast<float>(SDL_GetGamepadAxis(g, static_cast<SDL_GamepadAxis>(a))) / 32767.0f;
            s.axes[static_cast<std::size_t>(a)] = std::clamp(v, -1.0f, 1.0f);
        }
        for (int b = 0; b < static_cast<int>(GamepadButton::Count); ++b) {
            const bool down = SDL_GetGamepadButton(g, static_cast<SDL_GamepadButton>(b));
            if (down && !s.buttons.test(static_cast<std::size_t>(b)))
                s.pressed.set(static_cast<std::size_t>(b));
            s.buttons.set(static_cast<std::size_t>(b), down);
        }
    }
    for (std::size_t i = 0; i < m_joysticks.size(); ++i) {
        auto* j = static_cast<SDL_Joystick*>(m_joystickHandles[i]);
        auto& s = m_joysticks[i];
        for (std::size_t a = 0; a < s.axes.size(); ++a)
            s.axes[a] = std::clamp(static_cast<float>(SDL_GetJoystickAxis(j, static_cast<int>(a))) / 32767.0f, -1.0f, 1.0f);
        for (std::size_t b = 0; b < s.buttons.size(); ++b)
            s.buttons[b] = SDL_GetJoystickButton(j, static_cast<int>(b));
        for (std::size_t h = 0; h < s.hats.size(); ++h)
            s.hats[h] = SDL_GetJoystickHat(j, static_cast<int>(h));
    }
}

void Input::startTextInput(Window& window) { SDL_StartTextInput(window.sdl()); }
void Input::stopTextInput(Window& window) { SDL_StopTextInput(window.sdl()); }

void Input::setRelativeMouse(Window& window, bool enabled) { SDL_SetWindowRelativeMouseMode(window.sdl(), enabled); }

void Input::moveMouse(Window& window, Vec2 delta) {
    SDL_Window* w = window.sdl();
    if (!w || (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) == 0)
        return;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return;
    m_mousePos.x = std::clamp(m_mousePos.x + delta.x, 0.0f, static_cast<float>(pw - 1));
    m_mousePos.y = std::clamp(m_mousePos.y + delta.y, 0.0f, static_cast<float>(ph - 1));
    float d = SDL_GetWindowPixelDensity(w);
    if (d <= 0.0f)
        d = 1.0f;
    SDL_WarpMouseInWindow(w, m_mousePos.x / d, m_mousePos.y / d);
}

namespace {
Uint16 motor(float v) { return static_cast<Uint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); }
} // namespace

bool Input::rumbleGamepad(std::size_t index, float low, float high, std::uint32_t ms) {
    if (index >= m_gamepadHandles.size())
        return false;
    return SDL_RumbleGamepad(static_cast<SDL_Gamepad*>(m_gamepadHandles[index]), motor(low), motor(high), ms);
}

FFDevice* Input::forceFeedback(bool preferGamepad) {
    // The same device the race reads as its joystick.
    const bool usePad = preferGamepad ? !m_gamepads.empty() : m_joysticks.empty() && !m_gamepads.empty();
    std::uint32_t id = 0;
    SDL_Joystick* joystick = nullptr;
    if (usePad) {
        id = m_gamepads.front().id;
        joystick = SDL_GetGamepadJoystick(static_cast<SDL_Gamepad*>(m_gamepadHandles.front()));
    } else if (!m_joysticks.empty()) {
        id = m_joysticks.front().id;
        joystick = static_cast<SDL_Joystick*>(m_joystickHandles.front());
    }
    if (!joystick) {
        m_ff.reset();
        m_ffId = 0;
        return nullptr;
    }
    if (m_ffId != id) {
        m_ff.reset();
        m_ff = openForceFeedback(joystick);
        m_ffId = id;
    }
    return m_ff.get();
}

bool Input::rumbleJoystick(std::size_t index, float low, float high, std::uint32_t ms) {
    if (index >= m_joystickHandles.size())
        return false;
    return SDL_RumbleJoystick(static_cast<SDL_Joystick*>(m_joystickHandles[index]), motor(low), motor(high), ms);
}

} // namespace mm2::platform
