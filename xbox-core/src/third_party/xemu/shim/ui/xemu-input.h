/* xbox-core: what xemu's XID gamepad uses of xemu's input layer. The controller state comes from
 * xbox-core's pad (src/usb.cpp: SetPad) and the rumble goes back to it (GetRumble). */
#ifndef XB_XEMU_INPUT_H
#define XB_XEMU_INPUT_H
#include <stdbool.h>
#include <stdint.h>
#define DRIVER_DUKE "usb-xbox-gamepad"
#define DRIVER_S "usb-xbox-gamepad-s"
enum controller_state_buttons_mask {
    CONTROLLER_BUTTON_A = (1 << 0),
    CONTROLLER_BUTTON_B = (1 << 1),
    CONTROLLER_BUTTON_X = (1 << 2),
    CONTROLLER_BUTTON_Y = (1 << 3),
    CONTROLLER_BUTTON_DPAD_LEFT = (1 << 4),
    CONTROLLER_BUTTON_DPAD_UP = (1 << 5),
    CONTROLLER_BUTTON_DPAD_RIGHT = (1 << 6),
    CONTROLLER_BUTTON_DPAD_DOWN = (1 << 7),
    CONTROLLER_BUTTON_BACK = (1 << 8),
    CONTROLLER_BUTTON_START = (1 << 9),
    CONTROLLER_BUTTON_WHITE = (1 << 10),
    CONTROLLER_BUTTON_BLACK = (1 << 11),
    CONTROLLER_BUTTON_LSTICK = (1 << 12),
    CONTROLLER_BUTTON_RSTICK = (1 << 13),
    CONTROLLER_BUTTON_GUIDE = (1 << 14),
};
enum controller_state_axis_index {
    CONTROLLER_AXIS_LTRIG,
    CONTROLLER_AXIS_RTRIG,
    CONTROLLER_AXIS_LSTICK_X,
    CONTROLLER_AXIS_LSTICK_Y,
    CONTROLLER_AXIS_RSTICK_X,
    CONTROLLER_AXIS_RSTICK_Y,
    CONTROLLER_AXIS__COUNT,
};
typedef struct ControllerState {
    uint16_t buttons;
    int16_t axis[CONTROLLER_AXIS__COUNT];
    uint16_t rumble_l, rumble_r;
    int bound;
} ControllerState;
ControllerState *xemu_input_get_bound(int index);
void xemu_input_update_controller(ControllerState *state);
void xemu_input_update_rumble(ControllerState *state);
static inline bool xemu_input_get_test_mode(void) { return false; }
#endif
