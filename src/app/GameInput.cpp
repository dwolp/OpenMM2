// mmInput's per-frame input processing from Midtown Madness 2 (build 3393,
// MM2Recomp): mmInput::Update and the functions it runs, the readers
// GetThrottleVal / GetBrakesVal / GetHandBrake / GetSteering / GetCamPan,
// mmJoystick's view of the first joystick, and mmReplayManager's recording
// of the driving inputs.

#include "app/GameInput.h"

#include "game/CamCar.h"
#include "platform/Input.h"

#include <algorithm>
#include <cmath>

namespace mm2::app::controls {
namespace {

constexpr std::uint64_t bit(Action a) { return std::uint64_t{1} << static_cast<int>(a); }
constexpr std::size_t idx(Action a) { return static_cast<std::size_t>(a); }

// mmInput::PutEventInQueue keeps 30 actions a frame.
constexpr std::size_t kMaxEvents = 30;

} // namespace

// --- The joystick -------------------------------------------------------------------------------

float JoystickFrame::axis(int c) const {
    // mmJoystick::GetAxis: Z is read negated; the halves give the part of
    // the axis on their side as a positive value.
    switch (c) {
    case component::kJoyX: return x;
    case component::kJoyY: return y;
    case component::kJoyZ: return -z;
    case component::kJoyR: return r;
    case component::kJoyPov: return pov;
    case component::kJoyXLeft: return x < 0.0f ? -x : 0.0f;
    case component::kJoyXRight: return x <= 0.0f ? 0.0f : x;
    case component::kJoyYUp: return y < 0.0f ? -y : 0.0f;
    case component::kJoyYDown: return y <= 0.0f ? 0.0f : y;
    default: return 0.0f; // U and V are never polled; buttons have no axis
    }
}

bool JoystickFrame::button(int c) const {
    if (c < component::kJoyButton1 || component::kJoyButton12 < c)
        return false;
    return (buttons >> (c - component::kJoyButton1)) & 1u;
}

float normalizeAxis(int value) {
    // mmJaxis::SetRange(2000, -2000) and Normalize: ((v - min) * 2) / range - 1.
    constexpr float kMin = -2000.0f, kRange = 4000.0f;
    const float v = static_cast<float>(value);
    return ((v - kMin) + (v - kMin)) / kRange - 1.0f;
}

float directInputAxis(float value, float deadZone) {
    // mmJoystick::inputPrepareDevice asks DirectInput for -2000 .. 2000;
    // SetDeadZone (X and Y) makes it report the centre inside the dead zone
    // and rescale the rest (inferred: DirectInput is not in midtown2.exe).
    const float v = applyDeadZone(std::clamp(value, -1.0f, 1.0f), deadZone);
    return normalizeAxis(static_cast<int>(std::lround(v * 2000.0f)));
}

float povFromHat(std::uint8_t hat) {
    // DirectInput's POV in hundredths of a degree clockwise from north,
    // centred as -1 (mmJaxis::NormalizePOV gives -1 for it); the hat's
    // eight directions as SDL reports them (SDL_HAT_UP 1, RIGHT 2, DOWN 4,
    // LEFT 8).
    int angle;
    switch (hat & 0x0f) {
    case 0x1: angle = 0; break;
    case 0x3: angle = 4500; break;
    case 0x2: angle = 9000; break;
    case 0x6: angle = 13500; break;
    case 0x4: angle = 18000; break;
    case 0xc: angle = 22500; break;
    case 0x8: angle = 27000; break;
    case 0x9: angle = 31500; break;
    default: return -1.0f;
    }
    // mmJaxis::SetRange(36000, 0): (range - value) / range.
    constexpr float kRange = 36000.0f;
    return (kRange - static_cast<float>(angle)) / kRange;
}

JoystickFrame readJoystick(const platform::Input& in, Controller c, float deadZone) {
    using platform::GamepadAxis;
    using platform::GamepadButton;
    JoystickFrame j;
    const auto& pads = in.gamepads();
    const auto& sticks = in.joysticks();
    const bool usePad = c == Controller::GamePad ? !pads.empty() : sticks.empty() && !pads.empty();
    if (usePad) {
        const auto& p = pads.front();
        const auto axis = [&p](GamepadAxis a) { return p.axes[static_cast<std::size_t>(a)]; };
        const auto down = [&p](GamepadButton b) { return p.buttons.test(static_cast<std::size_t>(b)); };
        j.present = true;
        j.x = directInputAxis(axis(GamepadAxis::LeftX), deadZone);
        j.y = directInputAxis(axis(GamepadAxis::LeftY), deadZone);
        // DirectInput's XInput pad: both triggers on Z (the left one
        // towards the positive end); no Rz (inferred).
        j.z = directInputAxis(axis(GamepadAxis::LeftTrigger) - axis(GamepadAxis::RightTrigger), 0.0f);
        std::uint8_t hat = 0;
        hat |= down(GamepadButton::DpadUp) ? 0x1 : 0;
        hat |= down(GamepadButton::DpadRight) ? 0x2 : 0;
        hat |= down(GamepadButton::DpadDown) ? 0x4 : 0;
        hat |= down(GamepadButton::DpadLeft) ? 0x8 : 0;
        j.pov = povFromHat(hat);
        j.hasPov = true; // the D-pad
        constexpr GamepadButton kOrder[] = {GamepadButton::South,        GamepadButton::East,
                                            GamepadButton::West,         GamepadButton::North,
                                            GamepadButton::LeftShoulder, GamepadButton::RightShoulder,
                                            GamepadButton::Back,         GamepadButton::Start,
                                            GamepadButton::LeftStick,    GamepadButton::RightStick};
        for (std::size_t i = 0; i < std::size(kOrder); ++i)
            if (down(kOrder[i]))
                j.buttons |= 1u << i;
        j.numButtons = static_cast<int>(std::size(kOrder));
        return j;
    }
    if (sticks.empty())
        return j;
    const auto& s = sticks.front();
    const auto axis = [&s](std::size_t i) { return i < s.axes.size() ? s.axes[i] : 0.0f; };
    j.present = true;
    j.x = directInputAxis(axis(0), deadZone);
    j.y = directInputAxis(axis(1), deadZone);
    j.z = directInputAxis(axis(2), 0.0f);
    j.r = directInputAxis(axis(3), 0.0f);
    j.pov = s.hats.empty() ? -1.0f : povFromHat(s.hats.front());
    j.hasPov = !s.hats.empty();
    for (std::size_t i = 0; i < s.buttons.size() && i < 32; ++i)
        if (s.buttons[i])
            j.buttons |= 1u << i;
    j.numButtons = static_cast<int>(s.buttons.size());
    return j;
}

InputFrame readFrame(const platform::Input& in, Controller c, float deadZone, float width, float height) {
    InputFrame f;
    for (int k = 0; k < platform::kKeyCount; ++k)
        if (in.keyDown(static_cast<platform::Key>(k)))
            f.keysDown.set(static_cast<std::size_t>(k));
    f.keysPressed = in.keysPressedThisFrame();
    // ioMouse's buttons: left, right, middle.
    f.mouseButtons = (in.mouseDown(platform::MouseButton::Left) ? 1u : 0u) |
                     (in.mouseDown(platform::MouseButton::Right) ? 2u : 0u) |
                     (in.mouseDown(platform::MouseButton::Middle) ? 4u : 0u);
    const Vec2 m = in.mousePosition();
    f.mouseX = width > 0.0f ? m.x * (1.0f / width) : 0.5f;
    f.mouseY = height > 0.0f ? m.y * (1.0f / height) : 0.5f;
    f.joy = readJoystick(in, c, deadZone);
    if (c == Controller::Keyboard && !in.gamepads().empty()) {
        using platform::GamepadAxis;
        const auto& p = in.gamepads().front();
        const auto axis = [&p](GamepadAxis a) { return p.axes[static_cast<std::size_t>(a)]; };
        f.pad.present = true;
        f.pad.throttle = axis(GamepadAxis::RightTrigger);
        f.pad.brake = axis(GamepadAxis::LeftTrigger);
        f.pad.steer = applyDeadZone(axis(GamepadAxis::LeftX), deadZone);
        f.pad.handbrake = p.buttons.test(static_cast<std::size_t>(platform::GamepadButton::South));
    }
    return f;
}

void CaptureReader::begin(const InputFrame& f) {
    // mmJoyMan::SetCapture(1): Poll, then mmJaxis::ResetCapture on X, Y, Z
    // and Rz.
    m_restX = f.joy.x;
    m_restY = f.joy.y;
    m_restZ = f.joy.z;
    m_restR = f.joy.r;
    m_waitRelease = f.mouseButtons != 0;
    m_mouseLatch = false;
}

Captured CaptureReader::poll(const InputFrame& f, Controller c) {
    // UICWArray::Update: nothing is captured until the mouse button that
    // started the capture is up.
    if (m_waitRelease) {
        m_waitRelease = (f.mouseButtons & 3u) != 0;
        return {};
    }
    // mmInput::PollSuperQ: exactly one key went down.
    if (f.keysPressed.size() == 1)
        return {Captured::Kind::Key, static_cast<int>(f.keysPressed.front())};
    if (f.keysPressed.size() > 1)
        return {};
    // The mouse's buttons (eqEventHandler: left 1, right 2, middle 4), not
    // both at once, reported every other frame while held (the latch).
    const std::uint32_t mouse = f.mouseButtons;
    if (mouse != 0 && mouse != 3 && !m_mouseLatch) {
        m_mouseLatch = true;
        return {Captured::Kind::Mouse, static_cast<int>(mouse)};
    }
    m_mouseLatch = false;
    const bool joystickType = c == Controller::Joystick || c == Controller::GamePad || c == Controller::Wheel;
    if (!joystickType || !f.joy.present)
        return {};
    // mmJoyMan::PollJoyButtons / GetOneButton.
    for (int b = 0; b < 16; ++b)
        if ((f.joy.buttons >> b) & 1u)
            return {Captured::Kind::JoyButton, b + 1};
    // mmJoystick::Update while capturing, then mmJoyMan::PollJoyAxes.
    if (const int m = axisCaptured(m_restX, f.joy.x); m != 0)
        return {Captured::Kind::JoyAxis, m == 1 ? component::kJoyXRight : component::kJoyXLeft};
    if (const int m = axisCaptured(m_restY, f.joy.y); m != 0)
        return {Captured::Kind::JoyAxis, m == 1 ? component::kJoyYDown : component::kJoyYUp};
    if (axisCaptured(m_restZ, f.joy.z) != 0)
        return {Captured::Kind::JoyAxis, component::kJoyZ};
    if (axisCaptured(m_restR, f.joy.r) != 0)
        return {Captured::Kind::JoyAxis, component::kJoyR};
    return {};
}

phys::PedalInput replayQuantize(const phys::PedalInput& in) {
    // mmReplayManager::Update stores ftol(steering x 127) in a signed byte
    // and ftol(pedal x 255) in bytes; GetSteering / GetThrottle / GetBrakes
    // / GetHandBrakes read them back x 1/127 and x 1/255.
    const auto pedal = [](float v) {
        const auto b = static_cast<std::uint8_t>(static_cast<int>(v * 255.0f));
        return static_cast<float>(b) * 0.003921569f;
    };
    phys::PedalInput out = in;
    const auto s = static_cast<std::int8_t>(static_cast<int>(in.steering * 127.0f));
    out.steering = static_cast<float>(static_cast<int>(s)) * 0.007874016f;
    out.accelerator = pedal(in.accelerator);
    out.brake = pedal(in.brake);
    out.handbrake = pedal(in.handbrake);
    return out;
}

// --- GameInput ----------------------------------------------------------------------------------

void GameInput::configure(Controller c, const BindingSet& set, const Options& o, bool joystickPresent) {
    // mmPlayerConfig::SetControls / mmInput::Init: a joystick type without a
    // joystick is the keyboard (with the keyboard's set).
    m_chosen = c;
    m_options = o;
    m_controller = c;
    m_set = set;
    if ((c == Controller::Joystick || c == Controller::GamePad || c == Controller::Wheel) && !joystickPresent) {
        m_controller = Controller::Keyboard;
        m_set = defaultBindings(Controller::Keyboard, -1);
    }
    for (std::size_t i = 0; i < kActionCount; ++i)
        m_io[i] = assignedIoType(actions()[i].kind, m_set[i]);
    m_joyDown.fill(false);
    m_held = m_fired = m_hit = 0;
    m_events.clear();
    reset();
}

void GameInput::load(const IniFile& ini, const JoystickFrame& joy) {
    const Options o = Options::load(ini);
    const int buttons = joy.present ? joy.numButtons : -1;
    configure(o.controller, loadBindings(ini, o.controller, buttons), o, joy.present);
    // The keyboard stood in for a missing joystick: its own stored set.
    if (m_controller != o.controller) {
        m_set = loadBindings(ini, Controller::Keyboard, buttons);
        for (std::size_t i = 0; i < kActionCount; ++i)
            m_io[i] = assignedIoType(actions()[i].kind, m_set[i]);
    }
}

void GameInput::setPlayerTune(const data::DatNode& n) {
    // mmPlayer::FileIO's fields; a field the file lacks keeps the
    // constructor's value. The discrete rates go to mmInput for both the
    // keyboard and the game pad filter (mmPlayer::Update).
    for (phys::SteeringFilter* f : {&m_keyFilter, &m_padFilter}) {
        phys::SteeringFilter::Params& p = f->params;
        n.read("SpeedSensitive", p.speedSensitive);
        n.read("SpeedBaseLow", p.speedBaseLow);
        n.read("SpeedBaseHi", p.speedBaseHi);
        n.read("DiscreteSteeringDeltaOutLo", p.deltaOutLo);
        n.read("DiscreteSteeringDeltaInLo", p.deltaInLo);
        n.read("DiscreteSteeringFilterLo", p.filterLo);
        n.read("DiscreteSteeringDeltaOutHi", p.deltaOutHi);
        n.read("DiscreteSteeringDeltaInHi", p.deltaInHi);
        n.read("DiscreteSteeringFilterHi", p.filterHi);
    }
    AnalogSteering& a = m_analog;
    n.read("SpeedSensitive", a.speedSensitive);
    n.read("SpeedBaseLow", a.speedBaseLow);
    n.read("SpeedBaseHi", a.speedBaseHi);
    n.read("MouseSensitivityLow", a.mouseSensitivityLow);
    n.read("MouseSteerFilterLow", a.mouseFilterLow);
    n.read("MouseSensitivityHi", a.mouseSensitivityHi);
    n.read("MouseSteerFilterHi", a.mouseFilterHi);
    // JoyApp / WheelApp are shorts: the approach is on when not 0.
    const auto device = [&n](std::string_view prefix, AnalogSteering::DeviceParams& d) {
        const std::string k(prefix);
        n.read(k + "SensitivityLow", d.sensitivityLow);
        n.read(k + "SteerFilterLow", d.filterLow);
        n.read(k + "SensitivityHi", d.sensitivityHi);
        n.read(k + "SteerFilterHi", d.filterHi);
        int app = d.approach ? 1 : 0;
        if (n.read(k + "App", app))
            d.approach = app != 0;
        n.read(k + "SteerApproachOutLo", d.approachOutLow);
        n.read(k + "SteerApproachInLo", d.approachInLow);
        n.read(k + "SteerApproachOutHi", d.approachOutHi);
        n.read(k + "SteerApproachInHi", d.approachInHi);
        n.read(k + "SteerAppApp", d.approachGrowth);
    };
    device("Joy", a.joystick);
    device("Wheel", a.wheel);
    // ScoreWeight is read too, and nothing uses it.
}

void GameInput::reset() {
    // mmInput::Reset.
    m_throttle = m_brakes = m_handbrake = 0.0f;
    m_steerAxis = 0.0f;
    m_keyFilter.reset();
    m_padFilter.reset();
}

void GameInput::flush() {
    // mmInput::Flush: ProcessJoyEvents marks the joystick buttons held now
    // (their actions then wait for the next press), and everything queued
    // and the states are dropped.
    for (std::size_t i = 0; i < kActionCount; ++i)
        if (m_io[i] == IoType::Event && m_set[i].device == Device::Joystick)
            m_joyDown[i] = m_joy.button(m_set[i].component);
    m_events.clear();
    m_fired = 0;
    m_held = 0;
    m_hit = 0;
}

void GameInput::queue(Action a) {
    // mmInput::PutEventInQueue.
    if (m_events.size() >= kMaxEvents)
        return;
    m_events.push_back(a);
    m_fired |= bit(a);
}

bool GameInput::scanForEvent(Device device, int c) {
    // mmInput::ScanForEvent: the first press slot not yet hit this frame
    // whose control matches.
    for (std::size_t i = 0; i < kActionCount; ++i) {
        const Action a = static_cast<Action>(i);
        if (m_io[i] != IoType::Event || (m_hit & bit(a)) || m_set[i].device != device || m_set[i].component != c)
            continue;
        m_hit |= bit(a);
        queue(a);
        return true;
    }
    return false;
}

void GameInput::processMouse(const InputFrame& f) {
    // mmInput::ProcessMouseEvents: each mouse event (a move or a button
    // change) with a button held is matched against the buttons now held
    // (1 the left alone, 2 the right alone). OpenMM2 sees one event a frame.
    const bool event = f.mouseButtons != m_lastMouseButtons || f.mouseX != m_lastMouseX || f.mouseY != m_lastMouseY;
    m_lastMouseButtons = f.mouseButtons;
    m_lastMouseX = f.mouseX;
    m_lastMouseY = f.mouseY;
    if (event && f.mouseButtons != 0)
        scanForEvent(Device::Mouse, static_cast<int>(f.mouseButtons & 0xff));
}

void GameInput::processKeyboard(const InputFrame& f) {
    // mmInput::ProcessKeyboardEvents: DirectInput's buffered key presses
    // (no repeats), read from the last.
    for (auto it = f.keysPressed.rbegin(); it != f.keysPressed.rend(); ++it)
        scanForEvent(Device::Keyboard, static_cast<int>(*it));
}

void GameInput::processJoystick() {
    // mmInput::ProcessJoyEvents, for every press slot bound to the
    // joystick, in slot order.
    for (std::size_t i = 0; i < kActionCount; ++i) {
        const Action a = static_cast<Action>(i);
        if (m_io[i] != IoType::Event || m_set[i].device != Device::Joystick)
            continue;
        // ProcessJoyEvents also fires press slots bound to half of the X or Y
        // axis (past 0.75, every 0.8 s, compared crossed over), but no press
        // slot can hold one: mmIODev::Assign and SanityCheckioType refuse
        // axes for them, so that branch never runs and is not ported.
        const int c = m_set[i].component;
        if (component::kJoyButton1 <= c && c <= component::kJoyButton12) {
            // A button fires when it goes down (mmIODev +0xa0).
            if (m_joy.button(c)) {
                if (!m_joyDown[i]) {
                    m_joyDown[i] = true;
                    queue(a);
                }
            } else {
                m_joyDown[i] = false;
            }
        }
    }
}

void GameInput::pollContinuous(Action a, const InputFrame& f) {
    // mmInput::PollContinuous: the axis's value, stored for the slots that
    // have one. The mouse's X and Y are the cursor across the window over
    // the mouse sensitivity.
    const Binding b = m_set[idx(a)];
    float v = 0.0f;
    if (b.device == Device::Mouse)
        v = m_analog.mouseAxis(b.component == component::kMouseX ? f.mouseX : f.mouseY, 1.0f);
    else if (b.device == Device::Joystick)
        v = m_joy.axis(b.component);
    switch (a) {
    case Action::Throttle:
        // Only the joystick and the wheel keep an analog throttle, and only
        // its lower end is clamped.
        if (m_controller == Controller::Joystick || m_controller == Controller::Wheel)
            m_throttle = v < 0.0f ? 0.0f : v;
        break;
    case Action::Steering:
        if (m_controller != Controller::Keyboard)
            m_steerAxis = v;
        break;
    case Action::Brakes: m_brakes = v < 0.0f ? 0.0f : (1.0f < v ? 1.0f : v); break;
    case Action::Handbrake: m_handbrake = v < 0.0f ? 0.0f : (1.0f < v ? 1.0f : v); break;
    // The Camera Pan slot's value would go to mmInput +0x1a4, but MM2
    // compares the slot's 64-bit mask with (int)(1 << 31), a negative
    // number, so it is never stored: the camera pans from the POV hat in
    // GetCamPan instead.
    default: break;
    }
}

void GameInput::processStates(const InputFrame& f) {
    // mmInput::ProcessStates: the slots the controller reads; held buttons
    // set their bits (ScanState), axes are polled (PollContinuous).
    m_held = 0;
    for (std::size_t i = 0; i < kActionCount; ++i) {
        const Action a = static_cast<Action>(i);
        if (!slotEnabled(m_controller, a))
            continue;
        const Binding b = m_set[i];
        if (m_io[i] == IoType::Held) {
            bool down = false;
            switch (b.device) {
            case Device::Mouse: down = (static_cast<std::uint32_t>(b.component) & f.mouseButtons) != 0; break;
            case Device::Keyboard:
                down = b.component >= 0 && b.component < platform::kKeyCount &&
                       f.keysDown.test(static_cast<std::size_t>(b.component));
                break;
            case Device::Joystick: down = m_joy.button(b.component); break;
            default: break;
            }
            if (down)
                m_held |= bit(a);
        } else if (m_io[i] == IoType::Axis) {
            pollContinuous(a, f);
        }
    }
}

void GameInput::update(const InputFrame& f, float) {
    // mmInput::Update. mmJoyMan::Update polls the joystick only for the
    // joystick types.
    m_events.clear();
    m_fired = 0;
    const bool joystickType = m_controller == Controller::Joystick || m_controller == Controller::GamePad ||
                              m_controller == Controller::Wheel;
    if (joystickType)
        m_joy = f.joy;
    m_pad = m_controller == Controller::Keyboard ? f.pad : InputFrame::Pad{};
    // ProcessEvents: the mouse, the keyboard, the joystick, then the hit
    // flags are cleared.
    processMouse(f);
    processKeyboard(f);
    processJoystick();
    m_hit = 0;
    processStates(f);
}

float GameInput::throttle() const {
    // mmInput::GetThrottleVal: a held button gives 1, an axis its value.
    float v = 0.0f;
    if (m_io[idx(Action::Throttle)] == IoType::Held)
        v = held(Action::Throttle) ? 1.0f : 0.0f;
    else if (m_io[idx(Action::Throttle)] == IoType::Axis)
        v = m_throttle;
    return m_pad.present ? std::max(v, m_pad.throttle) : v;
}

float GameInput::brakes() const {
    float v = 0.0f;
    if (m_io[idx(Action::Brakes)] == IoType::Held)
        v = held(Action::Brakes) ? 1.0f : 0.0f;
    else if (m_io[idx(Action::Brakes)] == IoType::Axis)
        v = m_brakes;
    return m_pad.present ? std::max(v, m_pad.brake) : v;
}

float GameInput::handBrake() const {
    float v = 0.0f;
    if (m_io[idx(Action::Handbrake)] == IoType::Held)
        v = held(Action::Handbrake) ? 1.0f : 0.0f;
    else if (m_io[idx(Action::Handbrake)] == IoType::Axis)
        v = m_handbrake;
    return m_pad.present && m_pad.handbrake ? 1.0f : v;
}

float GameInput::steering(float dt) {
    switch (m_controller) {
    case Controller::Keyboard: {
        // FilterDiscreteSteering towards full lock (mmInput +0x1a8, 1);
        // Steer Left wins. OpenMM2 extra: a game pad's stick through the
        // same filter.
        float target = held(Action::SteerLeft) ? -1.0f : (held(Action::SteerRight) ? 1.0f : 0.0f);
        if (m_pad.present && m_pad.steer != 0.0f)
            target = m_pad.steer;
        return m_keyFilter.filter(target, dt);
    }
    case Controller::GamePad: return m_padFilter.filter(m_steerAxis, dt);
    default: return m_analog.filter(m_controller, m_steerAxis, dt); // playerFilterSteering
    }
}

void GameInput::setSpeed(float speed) {
    m_keyFilter.setSpeed(speed);
    m_padFilter.setSpeed(speed);
    m_analog.setSpeed(speed, m_options.sensitivity);
}

float GameInput::camPan(bool left, bool right, bool back, bool forward) const {
    // mmInput::GetCamPan. With the joystick controller a pushed POV hat
    // gives its value (north 1, east 0.75, south 0.5, west 0.25); otherwise
    // the look buttons (camCarCS's quarter turns).
    if (m_chosen == Controller::Joystick && m_controller == Controller::Joystick) {
        const float pov = m_joy.axis(component::kJoyPov);
        if (pov != -1.0f && pov != 0.0f)
            return pov;
    }
    return game::cameraPanFor(held(Action::LookLeft) || left, held(Action::LookRight) || right,
                              held(Action::LookBack) || back, held(Action::LookForward) || forward);
}

} // namespace mm2::app::controls
