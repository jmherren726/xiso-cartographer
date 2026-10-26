/* The controllers through nxdk's SDL (input_sdl.h). */
#include "input_sdl.h"

#include <SDL.h>
#include <stdbool.h>

#define MAX_PADS 4
#define TRIGGER_DOWN 16384 /* of 32767: past half way */
#define TRIGGER_UP 8192    /* let go below a quarter, so a trigger held at half doesn't flicker */

static SDL_GameController *pads[MAX_PADS];
static bool trigger_down[MAX_PADS][2]; /* LT, RT of each pad */
static ui_repeat repeat;

static int pad_slot(SDL_JoystickID id)
{
    SDL_GameController *c = SDL_GameControllerFromInstanceID(id);
    for (int i = 0; i < MAX_PADS; i++) {
        if (c != NULL && pads[i] == c) {
            return i;
        }
    }
    return -1;
}

static ui_button from_sdl(Uint8 b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return UI_BTN_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return UI_BTN_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return UI_BTN_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return UI_BTN_RIGHT;
    case SDL_CONTROLLER_BUTTON_A:
        return UI_BTN_A;
    case SDL_CONTROLLER_BUTTON_B:
        return UI_BTN_B;
    case SDL_CONTROLLER_BUTTON_X:
        return UI_BTN_X;
    case SDL_CONTROLLER_BUTTON_Y:
        return UI_BTN_Y;
    case SDL_CONTROLLER_BUTTON_START:
        return UI_BTN_START;
    case SDL_CONTROLLER_BUTTON_BACK:
        return UI_BTN_BACK;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: /* nxdk maps White here */
        return UI_BTN_WHITE;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: /* and Black here */
        return UI_BTN_BLACK;
    default:
        return UI_BTN_NONE;
    }
}

/* A trigger moved: a press when it passes TRIGGER_DOWN, ready again below TRIGGER_UP. */
static ui_button trigger(const SDL_ControllerAxisEvent *e)
{
    int t = e->axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT    ? 0
            : e->axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? 1
                                                          : -1;
    int slot = pad_slot(e->which);
    if (t < 0 || slot < 0) {
        return UI_BTN_NONE;
    }
    if (!trigger_down[slot][t] && e->value > TRIGGER_DOWN) {
        trigger_down[slot][t] = true;
        return t == 0 ? UI_BTN_LT : UI_BTN_RT;
    }
    if (trigger_down[slot][t] && e->value < TRIGGER_UP) {
        trigger_down[slot][t] = false;
    }
    return UI_BTN_NONE;
}

ui_button ui_sdl_button(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ui_button b = UI_BTN_NONE;
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            for (int i = 0; i < MAX_PADS; i++) {
                if (pads[i] == NULL) {
                    pads[i] = SDL_GameControllerOpen(e.cdevice.which);
                    break;
                }
            }
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            int slot = pad_slot(e.cdevice.which);
            if (slot >= 0) {
                SDL_GameControllerClose(pads[slot]);
                pads[slot] = NULL;
                trigger_down[slot][0] = false;
                trigger_down[slot][1] = false;
                repeat.held = UI_BTN_NONE;
            }
        } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            b = from_sdl(e.cbutton.button);
            ui_repeat_down(&repeat, b, SDL_GetTicks());
        } else if (e.type == SDL_CONTROLLERBUTTONUP) {
            ui_repeat_up(&repeat, from_sdl(e.cbutton.button));
        } else if (e.type == SDL_CONTROLLERAXISMOTION) {
            b = trigger(&e.caxis);
        }
        if (b != UI_BTN_NONE) {
            return b; /* the events after it wait in SDL's queue for the next call */
        }
    }
    return ui_repeat_due(&repeat, SDL_GetTicks());
}

bool ui_sdl_exit_combo(void)
{
    for (int i = 0; i < MAX_PADS; i++) {
        SDL_GameController *c = pads[i];
        if (c != NULL &&
            SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TRIGGER_DOWN &&
            SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_DOWN &&
            SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) &&
            SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) { /* Black */
            return true;
        }
    }
    return false;
}
