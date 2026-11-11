// Gamepad support for the 3.3.5 client.
//
// The engine already polls DirectInput8 gamepads (Engine/Source/Os/Win64/Joystick.cpp)
// and raises joystick events, but nothing in the UI layer ever subscribed to them, so
// buttons and sticks were silently dropped:
//
//   EVENT_ID_18 -> stick  (normalised -1..1 in m_x / m_y)
//   EVENT_ID_19 -> single axis
//   EVENT_ID_20 -> button pressed  (m_id = zero based button index)
//   EVENT_ID_21 -> button released
//   EVENT_ID_22 -> POV hat         (m_state = 0 centred, 1..8 clockwise from up)
//
// This file wires them up:
//   * buttons    -> "JOYBUTTON<n>"   through CGUIBindings::ExecKey
//   * POV hat    -> "JOYHATUP" / "JOYHATRIGHT" / "JOYHATDOWN" / "JOYHATLEFT"
//   * left stick -> MOVEFORWARD / MOVEBACKWARD / STRAFELEFT / STRAFERIGHT
//   * right stick -> CGInputControl::m_joystickLookX / m_joystickLookY (camera free look)
//
// The binding names above are part of the stock 3.3.5 binding vocabulary
// (see s_numberedKeys / s_keyNames in WoW/Source/Ui/UIBindings.cpp), so players can
// bind gamepad buttons like any other key:
//
//   /run SetBinding("JOYBUTTON1", "ACTIONBUTTON1")

#include "WoW/Source/UIUtil/JoystickInput.h"

#include <cmath>
#include <cstdio>

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Os/Joystick.h"
#include "WoW/Source/Ui/Types.h"
#include "WoW/Source/Ui/UIBindings.h"
#include "WoW/Source/UIUtil/InputControl.h"

namespace {

constexpr int32_t MOVE_STICK = 0;
constexpr int32_t LOOK_STICK = 1;

constexpr float MOVE_PRESS = 0.35f;
constexpr float MOVE_RELEASE = 0.22f;
constexpr float LOOK_DEADZONE = 0.18f;

bool s_initialized = false;

struct LATCH {
    bool m_held;
    const char* m_command;
};

LATCH s_moveForward = { false, "MOVEFORWARD" };
LATCH s_moveBackward = { false, "MOVEBACKWARD" };
LATCH s_strafeLeft = { false, "STRAFELEFT" };
LATCH s_strafeRight = { false, "STRAFERIGHT" };

struct HATLATCH {
    bool m_held;
    const char* m_key;
};

HATLATCH s_hatUp = { false, "JOYHATUP" };
HATLATCH s_hatRight = { false, "JOYHATRIGHT" };
HATLATCH s_hatDown = { false, "JOYHATDOWN" };
HATLATCH s_hatLeft = { false, "JOYHATLEFT" };

float ApplyDeadzone(float value, float deadzone) {
    float magnitude = std::fabs(value);

    if (magnitude <= deadzone) {
        return 0.0f;
    }

    float scaled = (magnitude - deadzone) / (1.0f - deadzone);

    if (scaled > 1.0f) {
        scaled = 1.0f;
    }

    return value < 0.0f ? -scaled : scaled;
}

void SetMoveState(LATCH& latch, bool down) {
    if (latch.m_held == down) {
        return;
    }

    CGUIBindings* bindings = CGUIBindings::s_bindings;

    if (!bindings) {
        return;
    }

    latch.m_held = down;
    bindings->ExecCommand(latch.m_command, down ? 1 : 0, down ? 1.0f : 0.0f, 1, 0, 0, -1.0f, 0.0f, nullptr);
}

// Hysteresis so a stick resting near the threshold does not chatter.
void SetMoveAxis(LATCH& positive, LATCH& negative, float value) {
    SetMoveState(positive, value > (positive.m_held ? MOVE_RELEASE : MOVE_PRESS));
    SetMoveState(negative, value < -(negative.m_held ? MOVE_RELEASE : MOVE_PRESS));
}

void SetHatState(HATLATCH& latch, bool down) {
    if (latch.m_held == down) {
        return;
    }

    CGUIBindings* bindings = CGUIBindings::s_bindings;

    if (!bindings) {
        return;
    }

    latch.m_held = down;
    bindings->ExecKey(0, latch.m_key, down ? 1 : 0, 1, BINDING_MODE_ACTIVE);
}

void SetButtonState(int32_t index, bool down) {
    if (index < 0 || index > 127) {
        return;
    }

    CGUIBindings* bindings = CGUIBindings::s_bindings;

    if (!bindings) {
        return;
    }

    char key[32];
    std::snprintf(key, sizeof(key), "JOYBUTTON%d", index + 1);
    bindings->ExecKey(0, key, down ? 1 : 0, 1, BINDING_MODE_ACTIVE);
}

void ReleaseAll() {
    SetMoveState(s_moveForward, false);
    SetMoveState(s_moveBackward, false);
    SetMoveState(s_strafeLeft, false);
    SetMoveState(s_strafeRight, false);
    SetHatState(s_hatUp, false);
    SetHatState(s_hatRight, false);
    SetHatState(s_hatDown, false);
    SetHatState(s_hatLeft, false);

    CGInputControl* input = CGInputControl::s_inputControl;

    if (input) {
        input->m_joystickLookX = 0.0f;
        input->m_joystickLookY = 0.0f;
    }
}

int32_t OnJoystickStick(const void* data, void*) {
    const EVENT_DATA_JOYSTICK* evt = static_cast<const EVENT_DATA_JOYSTICK*>(data);

    if (!evt) {
        return 0;
    }

    if (evt->m_id == LOOK_STICK) {
        CGInputControl* input = CGInputControl::s_inputControl;

        if (input) {
            // m_y is negative when the stick is pushed up, which is also the sign the
            // camera free look expects for a look up movement.
            input->m_joystickLookX = ApplyDeadzone(evt->m_x, LOOK_DEADZONE);
            input->m_joystickLookY = ApplyDeadzone(evt->m_y, LOOK_DEADZONE);
        }
    } else if (evt->m_id == MOVE_STICK) {
        SetMoveAxis(s_moveForward, s_moveBackward, -evt->m_y);
        SetMoveAxis(s_strafeRight, s_strafeLeft, evt->m_x);
    }

    return 0;
}

int32_t OnJoystickButtonDown(const void* data, void*) {
    const EVENT_DATA_JOYSTICK* evt = static_cast<const EVENT_DATA_JOYSTICK*>(data);

    if (!evt) {
        return 0;
    }

    SetButtonState(evt->m_id, true);
    return 0;
}

int32_t OnJoystickButtonUp(const void* data, void*) {
    const EVENT_DATA_JOYSTICK* evt = static_cast<const EVENT_DATA_JOYSTICK*>(data);

    if (!evt) {
        return 0;
    }

    SetButtonState(evt->m_id, false);
    return 0;
}

int32_t OnJoystickHat(const void* data, void*) {
    const EVENT_DATA_JOYSTICK* evt = static_cast<const EVENT_DATA_JOYSTICK*>(data);

    if (!evt) {
        return 0;
    }

    int32_t direction = evt->m_state;

    SetHatState(s_hatUp, direction == 1 || direction == 2 || direction == 8);
    SetHatState(s_hatRight, direction == 2 || direction == 3 || direction == 4);
    SetHatState(s_hatDown, direction == 4 || direction == 5 || direction == 6);
    SetHatState(s_hatLeft, direction == 6 || direction == 7 || direction == 8);

    return 0;
}

// Losing focus stops the polling loop, so every held axis has to be released here or
// the character would keep walking after an alt tab.
int32_t OnFocusChanged(const void* data, void*) {
    const EVENT_DATA_FOCUS* evt = static_cast<const EVENT_DATA_FOCUS*>(data);

    if (evt && !evt->m_focus) {
        ReleaseAll();
    }

    return 0;
}

// Only installed when the key is still unbound, so a player who rebinds with
// /run SetBinding("JOYBUTTON1", "ACTIONBUTTON7") keeps their layout.
struct DEFAULTBIND {
    const char* m_key;
    const char* m_command;
};

const DEFAULTBIND s_defaultBindings[] = {
    { "JOYBUTTON1", "JUMP" },
    { "JOYBUTTON2", "TOGGLEAUTORUN" },
    { "JOYBUTTON3", "ACTIONBUTTON1" },
    { "JOYBUTTON4", "ACTIONBUTTON2" },
    { "JOYBUTTON5", "ACTIONBUTTON3" },
    { "JOYBUTTON6", "ACTIONBUTTON4" },
    { "JOYBUTTON7", "TOGGLECHARACTER0" },
    { "JOYBUTTON8", "TOGGLEGAMEMENU" },
    { "JOYBUTTON9", "TOGGLESHEATH" },
    { "JOYBUTTON10", "INTERACTTARGET" },
    { "JOYHATUP", "ACTIONBUTTON5" },
    { "JOYHATDOWN", "ACTIONBUTTON6" },
    { "JOYHATLEFT", "ACTIONBUTTON7" },
    { "JOYHATRIGHT", "ACTIONBUTTON8" },
};

void InstallDefaultBindings() {
    CGUIBindings* bindings = CGUIBindings::s_bindings;

    if (!bindings) {
        return;
    }

    for (uint32_t i = 0; i < sizeof(s_defaultBindings) / sizeof(s_defaultBindings[0]); i++) {
        if (bindings->GetKeyBinding(BINDING_MODE_ACTIVE, s_defaultBindings[i].m_key)) {
            continue;
        }

        bindings->Bind(BINDING_SET_CURRENT, BINDING_MODE_0, s_defaultBindings[i].m_key, s_defaultBindings[i].m_command);
    }
}

} // namespace

void JoystickInput_Initialize() {
    if (s_initialized) {
        return;
    }

    s_initialized = true;

    EventRegisterEx(EVENT_ID_18, OnJoystickStick, nullptr, 1.0f);
    EventRegisterEx(EVENT_ID_20, OnJoystickButtonDown, nullptr, 1.0f);
    EventRegisterEx(EVENT_ID_21, OnJoystickButtonUp, nullptr, 1.0f);
    EventRegisterEx(EVENT_ID_22, OnJoystickHat, nullptr, 1.0f);
    EventRegisterEx(EVENT_ID_FOCUS, OnFocusChanged, nullptr, 1.0f);

    CVar* joystickCVar = CVar::LookupRegistered("Joystick");

    if (joystickCVar && joystickCVar->m_intValue && GetJoystick() == 0xFFFFFFFF) {
        JoystickCallback(joystickCVar, nullptr, "1", nullptr);
    }

    // Bindings are loaded by now, so this only fills the gaps.
    InstallDefaultBindings();

    uint32_t device = GetJoystick();

    if (device != 0xFFFFFFFF) {
        // Right stick is axis 2/3 on every common DirectInput gamepad; the built in
        // default mapping only registers axis 0/1 as stick 0.
        OsJoystickMapStick(device, LOOK_STICK, 2, 3);

        // The left stick drives the character, so the stock "stick as mouse cursor"
        // behaviour has to stay off or the cursor would walk along with the player.
        s_joystickMouseEnabled = 0;
        OsJoystickSetMouseMode(device, 0);
    }
}
