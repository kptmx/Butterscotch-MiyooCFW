#pragma once

#include "runner.h"
#include "runner_keyboard.h"
#include "renderer.h"
#include <SDL/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Debug menu state — singleton
typedef struct {
    bool visible;
    int selectedItem;
    int itemCount;
} DebugMenu;

extern DebugMenu g_debugMenu;

// Call once per frame when menu is visible. Returns true if menu handled input.
bool DebugMenu_process(Renderer* renderer, Runner* runner);

// Toggle menu visibility
void DebugMenu_toggle(Renderer* renderer, Runner* runner);

// Draw menu on top of current frame
void DebugMenu_draw(Renderer* renderer, SDL_Surface* screen);

#ifdef __cplusplus
}
#endif
