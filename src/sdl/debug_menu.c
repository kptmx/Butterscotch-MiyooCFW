#include "debug_menu.h"
#include "runner.h"
#include "sdl_renderer.h"

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <stdio.h>
#include <string.h>

DebugMenu g_debugMenu = {0};

static TTF_Font* getDebugFont(void) {
    return SDLRendererOpt_getLoadingFont();
}

typedef enum {
    MI_SPRITE_BBOX,
    MI_DEBUG_OVERLAY,
    MI_ROOM_PREV,
    MI_ROOM_NEXT,
    MI_COUNT
} MenuItem;

static const char* menuLabels[MI_COUNT] = {
    "Sprite bboxes",
    "Debug overlay",
    "Room: prev",
    "Room: next",
};

static void handleSelect(MenuItem item, Runner* runner) {
    if (item == MI_SPRITE_BBOX) {
        SDLRendererOpt_toggleDebugBBoxes(runner->renderer);
    } else if (item == MI_DEBUG_OVERLAY) {
        SDLRendererOpt_toggleDebugOverlay(runner->renderer);
    } else if (item == MI_ROOM_PREV) {
        DataWin* dw = runner->dataWin;
        if (dw && runner->currentRoomOrderPosition > 0) {
            int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
            runner->pendingRoom = prevIdx;
            runner->audioSystem->vtable->stopAll(runner->audioSystem);
        }
    } else if (item == MI_ROOM_NEXT) {
        DataWin* dw = runner->dataWin;
        if (dw && (int32_t)dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
            int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
            runner->pendingRoom = nextIdx;
            runner->audioSystem->vtable->stopAll(runner->audioSystem);
        }
    }
}

// Draw a single line of text using SDL_ttf at the given position
static void drawText(SDL_Surface* screen, const char* text, int x, int y, SDL_Color color) {
    TTF_Font* font = getDebugFont();
    if (!font || !text) return;
    SDL_Surface* surf = TTF_RenderText_Blended(font, text, color);
    if (!surf) return;
    SDL_Rect dst = { x, y, 0, 0 };
    SDL_BlitSurface(surf, NULL, screen, &dst);
    SDL_FreeSurface(surf);
}

void DebugMenu_toggle(Renderer* renderer, Runner* runner) {
    (void)renderer;
    (void)runner;
    g_debugMenu.visible = !g_debugMenu.visible;
    g_debugMenu.selectedItem = 0;
    g_debugMenu.itemCount = MI_COUNT;
}

bool DebugMenu_process(Renderer* renderer, Runner* runner) {
    if (!g_debugMenu.visible) return false;

    // Use runner keyboard state
    if (RunnerKeyboard_checkPressed(runner->keyboard, VK_UP)) {
        g_debugMenu.selectedItem--;
        if (g_debugMenu.selectedItem < 0) g_debugMenu.selectedItem = g_debugMenu.itemCount - 1;
    }
    if (RunnerKeyboard_checkPressed(runner->keyboard, VK_DOWN)) {
        g_debugMenu.selectedItem++;
        if (g_debugMenu.selectedItem >= g_debugMenu.itemCount) g_debugMenu.selectedItem = 0;
    }
    if (RunnerKeyboard_checkPressed(runner->keyboard, VK_ENTER)) {  // A on PC / START on MiyooCFW or Z (keycode 90)
        handleSelect((MenuItem)g_debugMenu.selectedItem, runner);
    }
    if (RunnerKeyboard_checkPressed(runner->keyboard, 90)) {  // A on PC / START on MiyooCFW or Z (keycode 90)
        handleSelect((MenuItem)g_debugMenu.selectedItem, runner);
    }
    if (RunnerKeyboard_checkPressed(runner->keyboard, VK_ESCAPE)) { // B on PC / SELECT on MiyooCFW
        g_debugMenu.visible = false;
        return true;
    }

    return true; // consumed input
}

void DebugMenu_draw(Renderer* renderer, SDL_Surface* screen) {
    (void)renderer;

    int menuX = 4;
    int menuY = 4;
    int lineH = 14;
    int totalH = g_debugMenu.itemCount * lineH + 4;

    // Background
    SDL_Rect bg = { menuX - 2, menuY - 2, 200, totalH };
    SDL_FillRect(screen, &bg, SDL_MapRGB(screen->format, 0, 0, 0));

    // Items
    for (int i = 0; i < g_debugMenu.itemCount; i++) {
        int y = menuY + i * lineH;
        SDL_Color col;
        if (i == g_debugMenu.selectedItem) {
            col.r = 0; col.g = 255; col.b = 255; // cyan selected
        } else {
            col.r = 200; col.g = 200; col.b = 200;
        }

        char buf[64];
        if (i == MI_SPRITE_BBOX) {
            snprintf(buf, sizeof(buf), "%s [%s]", menuLabels[i],
                     SDLRendererOpt_isDebugBBoxesEnabled(renderer) ? "ON" : "OFF");
            drawText(screen, buf, menuX, y, col);
        } else if (i == MI_DEBUG_OVERLAY) {
            snprintf(buf, sizeof(buf), "%s [%s]", menuLabels[i],
                     SDLRendererOpt_isDebugOverlayEnabled(renderer) ? "ON" : "OFF");
            drawText(screen, buf, menuX, y, col);
        } else {
            drawText(screen, menuLabels[i], menuX, y, col);
        }
    }
}
