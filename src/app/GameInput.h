#pragma once

// mmInput's work each frame (mmInput::Update): the chosen controller's
// binding set read against the devices, giving the game its held buttons
// (ProcessStates / ScanState), its analog values (PollContinuous), its
// one-shot actions (ProcessEvents: the mouse, the keyboard's presses and
// the joystick's buttons and axis halves), and the driving inputs
// (GetThrottleVal, GetBrakesVal, GetHandBrake, GetSteering with its
// filters, GetCamPan). DirectInput is replaced by the platform layer: the
// first joystick is read the way DirectInput showed it to mmJoystick.

#include "app/Controls.h"
#include "data/DatFile.h"
#include "phys/vehicle/Controls.h"

#include <bitset>
#include <cstdint>
#include <vector>

namespace mm2::platform {
class Input;
}

namespace mm2::app::controls {

// The first joystick as mmJoystick::Poll sees it: DirectInput's range of
// -2000 .. 2000 on X, Y, Z and Rz normalised to -1 .. 1 (mmJaxis::Normalize),
// with the dead zone on X and Y (mmJoystick::SetDeadZone), the POV hat
// (mmJaxis::NormalizePOV: (36000 - angle) / 36000, -1 when centred) and the
// buttons.
struct JoystickFrame {
    bool present = false;
    float x = 0.0f, y = 0.0f, z = 0.0f, r = 0.0f;
    float pov = -1.0f;
    std::uint32_t buttons = 0; // bit n: button n + 1
    int numButtons = 0;
    bool hasPov = false; // DIDEVCAPS dwPOVs (mmJoyMan::HasCoolie)

    // mmJoystick::GetAxis for a component (0 for buttons and U / V, which
    // mmJoystick::Poll never reads).
    float axis(int component) const;
    // mmJoyMan::GetJoyButton (ButtonToBit: buttons 1-12).
    bool button(int component) const;
};

// mmJaxis::Normalize of an axis DirectInput reported in -2000 .. 2000.
float normalizeAxis(int value);
// The platform layer's -1 .. 1 axis through DirectInput: the dead zone,
// then the integer range, then mmJaxis::Normalize.
float directInputAxis(float value, float deadZone);
// mmJaxis::NormalizePOV for an SDL hat (SDL_HAT_* bits).
float povFromHat(std::uint8_t hat);

// The devices for one frame.
struct InputFrame {
    std::bitset<platform::kKeyCount> keysDown;
    std::vector<platform::Key> keysPressed; // in the order they went down
    std::uint32_t mouseButtons = 0;         // bit 0 left, bit 1 right, bit 2 middle
    float mouseX = 0.5f, mouseY = 0.5f;     // the cursor across the window, 0 .. 1
    JoystickFrame joy;
    // OpenMM2 extra: a game pad next to the keyboard controller drives too
    // (the triggers, the left stick through the dead zone, South for the
    // handbrake).
    struct Pad {
        bool present = false;
        float throttle = 0.0f, brake = 0.0f, steer = 0.0f;
        bool handbrake = false;
    } pad;
};

// The first joystick for a controller: the game pad controller prefers a
// device SDL knows as a game pad, the joystick and the wheel a raw joystick
// (SDL splits what DirectInput listed together; inferred). A raw joystick's
// axes 0-3 stand for X, Y, Z and Rz and its first hat for the POV; a game
// pad shows DirectInput's view of an XInput pad (the left stick on X / Y,
// the triggers sharing Z, the D-pad as the POV, buttons A, B, X, Y, LB, RB,
// Back, Start and the stick clicks).
JoystickFrame readJoystick(const platform::Input& in, Controller c, float deadZone);
// The whole frame; `width` / `height` are the window's size in the units of
// platform::Input::mousePosition().
InputFrame readFrame(const platform::Input& in, Controller c, float deadZone, float width, float height);

// mmInput::CaptureState / PollStates for the customize page: what the
// player pressed to bind a slot.
class CaptureReader {
public:
    // CaptureState(1): the joystick's axes rest where they are now
    // (mmJaxis::ResetCapture); a mouse button still held from the click
    // that started the capture is waited out (UICWArray::Update).
    void begin(const InputFrame& f);
    // PollStates: one newly pressed key (none when several went down
    // together; Escape cancels and F1-F10 are refused by the page), else a
    // mouse button alone (left 1, right 2; not both), else for the joystick
    // types the lowest of the first sixteen joystick buttons, else an axis
    // that moved 0.125 from rest (X and Y by halves, Z, Rz).
    Captured poll(const InputFrame& f, Controller c);

private:
    float m_restX = 0.0f, m_restY = 0.0f, m_restZ = 0.0f, m_restR = 0.0f;
    bool m_waitRelease = false;
    bool m_mouseLatch = false; // the latch PollStates keeps for the mouse
};

// mmReplayManager::Update records the inputs every frame and the game
// drives the car with the recorded values: the steering as a signed byte
// (x 127, truncated, read back x 1/127), the pedals as bytes (x 255).
phys::PedalInput replayQuantize(const phys::PedalInput& in);

class GameInput {
public:
    // mmInput::Init for a controller: its binding set, the options, and
    // whether a joystick is connected (mmPlayerConfig::SetControls falls
    // back to the keyboard when a joystick type has none).
    void configure(Controller c, const BindingSet& set, const Options& o, bool joystickPresent);
    // configure() from the [Controls] section.
    void load(const IniFile& ini, const JoystickFrame& joy);

    // The controller in use and the one chosen.
    Controller controller() const { return m_controller; }
    const Options& options() const { return m_options; }
    Binding binding(Action a) const { return m_set[static_cast<std::size_t>(a)]; }
    IoType ioType(Action a) const { return m_io[static_cast<std::size_t>(a)]; }

    // mmInput::Update (ProcessEvents, then ProcessStates).
    void update(const InputFrame& f, float dt);
    // mmInput::Flush: the frame's actions and states are dropped; the
    // joystick buttons held now will not fire.
    void flush();
    // mmInput::Reset: the pedal values, the steering and the axis-half
    // timer back to rest.
    void reset();

    // The slot's button is held this frame (mmInput +0x158).
    bool held(Action a) const { return (m_held >> static_cast<int>(a)) & 1u; }
    // The action fired this frame (mmInput::PopEvent).
    bool fired(Action a) const { return (m_fired >> static_cast<int>(a)) & 1u; }
    // The actions in mmInput::PopEvent's order (the last queued first).
    const std::vector<Action>& events() const { return m_events; }

    // mmInput::GetThrottleVal, GetBrakesVal, GetHandBrake.
    float throttle() const;
    float brakes() const;
    float handBrake() const;
    // The steering control's value (mmInput +0x19c): the mouse's or the
    // joystick's axis.
    float steeringAxis() const { return m_steerAxis; }
    // mmInput::GetSteering(playerFilterSteering): one frame of the
    // controller's filter (FilterDiscreteSteering for the keyboard,
    // FilterGamepadSteering for the game pad, mmPlayer::FilterSteering for
    // the mouse, the joystick and the wheel).
    float steering(float dt);
    // The game pad filter on an axis value (OPENMM2_DEBUG_INPUT).
    float filterAxis(float axis, float dt) { return m_padFilter.filter(axis, dt); }
    // mmPlayer::Update: the filters' parameters for the car's forward
    // speed (m/s), used from the next frame.
    void setSpeed(float speed);
    // mmPlayer::Init loads the player node named after the car, which reads
    // tune/<car>.asnode (mmPlayer::FileIO: the speed bases, the keyboard and
    // game pad rates, and the mouse, joystick and wheel curves) over
    // mmPlayer's constructor values. Every retail car but vpcentury and vpdune
    // ships one.
    void setPlayerTune(const data::DatNode& node);
    // The keyboard and game pad rates, and the analog devices' curves.
    const phys::SteeringFilter::Params& discreteSteering() const { return m_keyFilter.params; }
    const phys::SteeringFilter::Params& padSteering() const { return m_padFilter.params; }
    const AnalogSteering& analogSteering() const { return m_analog; }
    // mmInput::GetCamPan: the joystick's POV hat (joystick controller), else
    // the look buttons (OpenMM2 may add its own look inputs).
    float camPan(bool left = false, bool right = false, bool back = false, bool forward = false) const;

private:
    void processMouse(const InputFrame& f);
    void processKeyboard(const InputFrame& f);
    void processJoystick();
    void processStates(const InputFrame& f);
    void pollContinuous(Action a, const InputFrame& f);
    bool scanForEvent(Device device, int component);
    void queue(Action a);

    Controller m_chosen = Controller::Keyboard;
    Controller m_controller = Controller::Keyboard;
    Options m_options;
    BindingSet m_set{};
    std::array<IoType, kActionCount> m_io{};
    std::array<bool, kActionCount> m_joyDown{}; // mmIODev +0xa0: a press slot's button was down
    std::uint64_t m_held = 0;                  // +0x158
    std::uint64_t m_fired = 0;
    std::uint64_t m_hit = 0;                   // mmIO +0x10, cleared after ProcessEvents
    std::vector<Action> m_events;              // +0x50, up to 30
    float m_throttle = 0.0f, m_brakes = 0.0f, m_handbrake = 0.0f; // +0x198, +0x190, +0x194
    float m_steerAxis = 0.0f;                                     // +0x19c
    std::uint32_t m_lastMouseButtons = 0;
    float m_lastMouseX = -1.0f, m_lastMouseY = -1.0f;
    JoystickFrame m_joy;
    InputFrame::Pad m_pad;
    phys::SteeringFilter m_keyFilter; // FilterDiscreteSteering (+0x19c on the keyboard)
    phys::SteeringFilter m_padFilter; // FilterGamepadSteering (+0x1a0)
    AnalogSteering m_analog;          // mmPlayer::FilterSteering
};

} // namespace mm2::app::controls
