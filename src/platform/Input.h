#pragma once

#include "core/Math.h"
#include "platform/ForceFeedback.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

union SDL_Event;

namespace mm2::platform {

class Window;

// Physical key positions (USB HID usage IDs, identical to SDL scancodes), so
// bindings follow key position regardless of keyboard layout.
enum class Key : std::uint16_t {
    Unknown = 0,
    A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Return = 40, Escape, Backspace, Tab, Space, Minus, Equals, LeftBracket, RightBracket, Backslash,
    Semicolon = 51, Apostrophe, Grave, Comma, Period, Slash, CapsLock,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    PrintScreen = 70, ScrollLock, Pause, Insert, Home, PageUp, Delete, End, PageDown,
    Right = 79, Left, Down, Up,
    NumLock = 83, KpDivide, KpMultiply, KpMinus, KpPlus, KpEnter,
    Kp1 = 89, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9, Kp0, KpPeriod,
    LCtrl = 224, LShift, LAlt, LGui, RCtrl, RShift, RAlt, RGui,
};
inline constexpr int kKeyCount = 512;

// Human-readable key name for binding menus ("Left Shift").
std::string keyName(Key key);
Key keyFromName(std::string_view name);

enum class MouseButton : std::uint8_t { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

// Layout matches SDL_GamepadButton / SDL_GamepadAxis.
enum class GamepadButton : std::uint8_t {
    South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, Misc1, RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2,
    Touchpad, Misc2, Misc3, Misc4, Misc5, Misc6,
    Count
};
enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

struct GamepadState {
    std::uint32_t id = 0; // instance id, stable while connected
    std::string name;
    std::array<float, static_cast<int>(GamepadAxis::Count)> axes{}; // sticks -1..1, triggers 0..1
    std::bitset<static_cast<int>(GamepadButton::Count)> buttons;
    std::bitset<static_cast<int>(GamepadButton::Count)> pressed; // went down this frame
    bool hasRumble = false;
};

// Joysticks that are not recognised as gamepads: steering wheels, pedals,
// flight sticks. Axes are normalised to -1..1.
struct JoystickState {
    std::uint32_t id = 0;
    std::string name;
    std::vector<float> axes;
    std::vector<bool> buttons;
    std::vector<std::uint8_t> hats; // SDL_HAT_* bitmask
    bool hasRumble = false;
};

// Polled input state, updated by platform::pollEvents().
class Input {
public:
    Input();
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Clears per-frame edges (pressed/released, wheel, deltas, text). Call
    // once per frame before pollEvents().
    void beginFrame();
    void handleEvent(const SDL_Event& event);
    // Opens/closes devices for connection events and samples device state.
    void updateDevices();

    bool keyDown(Key k) const { return m_keys.test(index(k)); }
    bool keyPressed(Key k) const { return m_keysPressed.test(index(k)); }
    bool keyReleased(Key k) const { return m_keysReleased.test(index(k)); }
    bool anyKeyPressed() const { return m_keysPressed.any(); }
    // Keys pressed this frame, in order (for "press a key" binding menus).
    const std::vector<Key>& keysPressedThisFrame() const { return m_pressOrder; }

    Vec2 mousePosition() const { return m_mousePos; } // window pixels
    Vec2 mouseDelta() const { return m_mouseDelta; }
    Vec2 mouseWheel() const { return m_wheel; }
    bool mouseDown(MouseButton b) const { return m_mouseButtons & bit(b); }
    bool mousePressed(MouseButton b) const { return m_mousePressed & bit(b); }

    // UTF-8 text typed this frame (only while text input is active).
    const std::string& text() const { return m_text; }
    void startTextInput(Window& window);
    void stopTextInput(Window& window);
    static void setRelativeMouse(Window& window, bool enabled);
    // Moves the pointer by `delta` window pixels, kept inside the window, and
    // the system cursor with it (the menus' gamepad pointer). Only while the
    // window has the input focus.
    void moveMouse(Window& window, Vec2 delta);

    const std::vector<GamepadState>& gamepads() const { return m_gamepads; }
    const std::vector<JoystickState>& joysticks() const { return m_joysticks; }
    // Rumble motors 0..1 for `ms` milliseconds. Returns false if unsupported.
    bool rumbleGamepad(std::size_t index, float low, float high, std::uint32_t ms);
    bool rumbleJoystick(std::size_t index, float low, float high, std::uint32_t ms);
    // Force feedback on the first joystick (the first game pad when
    // `preferGamepad`, else the first raw joystick, else the other kind):
    // opened on first use and closed with the device. Null without one.
    FFDevice* forceFeedback(bool preferGamepad);

    // Set by pollEvents(); true once when a device was connected/disconnected.
    bool devicesChanged() const { return m_devicesChanged; }

private:
    static int index(Key k) { return static_cast<int>(k) & (kKeyCount - 1); }
    static std::uint32_t bit(MouseButton b) { return 1u << static_cast<int>(b); }
    void openDevice(std::uint32_t id);
    void closeDevice(std::uint32_t id);

    std::bitset<kKeyCount> m_keys, m_keysPressed, m_keysReleased;
    std::vector<Key> m_pressOrder;
    Vec2 m_mousePos, m_mouseDelta, m_wheel;
    std::uint32_t m_mouseButtons = 0, m_mousePressed = 0;
    std::string m_text;
    std::vector<GamepadState> m_gamepads;
    std::vector<JoystickState> m_joysticks;
    std::vector<void*> m_gamepadHandles;  // SDL_Gamepad*, parallel to m_gamepads
    std::vector<void*> m_joystickHandles; // SDL_Joystick*, parallel to m_joysticks
    bool m_devicesChanged = false;
    std::unique_ptr<FFDevice> m_ff;
    std::uint32_t m_ffId = 0;
};

} // namespace mm2::platform
