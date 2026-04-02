#include "sdl_renderer.h"
#include "data_win.h"
#include "text_utils.h"
#include "matrix_math.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ===[ Coordinate Transform ]===
// Game-space (x,y) → backbuffer pixel position.
// Backbuffer is fixed 320x240, so we scale from game resolution to backbuffer.
static void viewToBackbuffer(const SDLRenderer* sdl,
                              float gx, float gy,
                              float* bx, float* by)
{
    // First, apply view→port transformation in game space
    float px, py;
    if (sdl->viewW > 0 && sdl->viewH > 0) {
        px = (gx - sdl->viewX) * (float)sdl->portW / (float)sdl->viewW;
        py = (gy - sdl->viewY) * (float)sdl->portH / (float)sdl->viewH;
    } else {
        px = gx;
        py = gy;
    }

    // Scale from game space to backbuffer space using pre-scaled port values
    // This ensures consistency with the clip rect set in beginView
    float scaleX = (float)sdl->windowW / (float)sdl->gameW;
    float scaleY = (float)sdl->windowH / (float)sdl->gameH;
    
    *bx = sdl->scaledPortX + px * scaleX;
    *by = sdl->scaledPortY + py * scaleY;
}

// Scale a single dimension from game space to backbuffer space.
static float scaleW(const SDLRenderer* sdl, float w) {
    float scaleX = (float)sdl->windowW / (float)sdl->gameW;
    if (sdl->viewW > 0) {
        return w * (float)sdl->portW / (float)sdl->viewW * scaleX;
    }
    return w * scaleX;
}

static float scaleH(const SDLRenderer* sdl, float h) {
    float scaleY = (float)sdl->windowH / (float)sdl->gameH;
    if (sdl->viewH > 0) {
        return h * (float)sdl->portH / (float)sdl->viewH * scaleY;
    }
    return h * scaleY;
}

// ===[ Nearest-Neighbour Blit With Per-Pixel Alpha ]===
// Renders a (possibly scaled) region of `src` onto `dst` at (dstX, dstY).
// Respects dst->clip_rect. `globalAlpha` in [0..255] multiplies source alpha.
static void blitScaledAlpha(SDL_Surface* src, const SDL_Rect* srcRect,
                             SDL_Surface* dst, int dstX, int dstY,
                             int dstW, int dstH,
                             uint8_t globalAlpha)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    // Clip rectangle on destination
    int cx1 = dst->clip_rect.x;
    int cy1 = dst->clip_rect.y;
    int cx2 = cx1 + (int)dst->clip_rect.w;
    int cy2 = cy1 + (int)dst->clip_rect.h;
    if (cx2 > dst->w) cx2 = dst->w;
    if (cy2 > dst->h) cy2 = dst->h;

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    int srcBpp = src->format->BytesPerPixel;
    int dstBpp = dst->format->BytesPerPixel;

    for (int dy = 0; dy < dstH; dy++) {
        int dpy = dstY + dy;
        if (dpy < cy1 || dpy >= cy2) continue;

        int spy = sy0 + (int)((float)dy * sh / dstH);
        if (spy < 0 || spy >= src->h) continue;

        for (int dx = 0; dx < dstW; dx++) {
            int dpx = dstX + dx;
            if (dpx < cx1 || dpx >= cx2) continue;

            int spx = sx0 + (int)((float)dx * sw / dstW);
            if (spx < 0 || spx >= src->w) continue;

            // Read source pixel
            const uint8_t* sp = (const uint8_t*)src->pixels
                                 + spy * src->pitch + spx * srcBpp;
            uint32_t sc;
            switch (srcBpp) {
                case 4: sc = *(const uint32_t*)sp; break;
                case 3: sc = sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16); break;
                default: continue;
            }

            uint8_t sr, sg, sb, sa;
            SDL_GetRGBA(sc, src->format, &sr, &sg, &sb, &sa);

            // Multiply by global alpha
            sa = (uint8_t)((uint32_t)sa * globalAlpha / 255u);
            if (sa == 0) continue;

            // Read destination pixel
            uint8_t* dp = (uint8_t*)dst->pixels
                          + dpy * dst->pitch + dpx * dstBpp;
            uint32_t dc;
            switch (dstBpp) {
                case 4: dc = *(uint32_t*)dp; break;
                case 3: dc = dp[0] | ((uint32_t)dp[1] << 8) | ((uint32_t)dp[2] << 16); break;
                default: continue;
            }

            uint8_t dr, dg, db, da;
            SDL_GetRGBA(dc, dst->format, &dr, &dg, &db, &da);

            // Porter-Duff "src over dst"
            uint8_t nr = (uint8_t)((sr * sa + dr * (255u - sa)) / 255u);
            uint8_t ng = (uint8_t)((sg * sa + dg * (255u - sa)) / 255u);
            uint8_t nb = (uint8_t)((sb * sa + db * (255u - sa)) / 255u);

            uint32_t nc = SDL_MapRGBA(dst->format, nr, ng, nb, 255);
            switch (dstBpp) {
                case 4: *(uint32_t*)dp = nc; break;
                case 3:
                    dp[0] = (uint8_t)(nc >>  0);
                    dp[1] = (uint8_t)(nc >>  8);
                    dp[2] = (uint8_t)(nc >> 16);
                    break;
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Tinted Blit: applies color tint to source pixels ]===
// Renders text with a color tint by multiplying source RGB by tint color
static void blitScaledAlphaTint(SDL_Surface* src, const SDL_Rect* srcRect,
                             SDL_Surface* dst, int dstX, int dstY,
                             int dstW, int dstH,
                             uint8_t globalAlpha, uint32_t tintColor)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    // Extract tint color (BGR format)
    uint8_t tintR = BGR_R(tintColor);
    uint8_t tintG = BGR_G(tintColor);
    uint8_t tintB = BGR_B(tintColor);

    // Clip rectangle on destination
    int cx1 = dst->clip_rect.x;
    int cy1 = dst->clip_rect.y;
    int cx2 = cx1 + (int)dst->clip_rect.w;
    int cy2 = cy1 + (int)dst->clip_rect.h;
    if (cx2 > dst->w) cx2 = dst->w;
    if (cy2 > dst->h) cy2 = dst->h;

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    int srcBpp = src->format->BytesPerPixel;
    int dstBpp = dst->format->BytesPerPixel;

    for (int dy = 0; dy < dstH; dy++) {
        int dpy = dstY + dy;
        if (dpy < cy1 || dpy >= cy2) continue;

        int spy = sy0 + (int)((float)dy * sh / dstH);
        if (spy < 0 || spy >= src->h) continue;

        for (int dx = 0; dx < dstW; dx++) {
            int dpx = dstX + dx;
            if (dpx < cx1 || dpx >= cx2) continue;

            int spx = sx0 + (int)((float)dx * sw / dstW);
            if (spx < 0 || spx >= src->w) continue;

            // Read source pixel
            const uint8_t* sp = (const uint8_t*)src->pixels
                                 + spy * src->pitch + spx * srcBpp;
            uint32_t sc;
            switch (srcBpp) {
                case 4: sc = *(const uint32_t*)sp; break;
                case 3: sc = sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16); break;
                default: continue;
            }

            uint8_t sr, sg, sb, sa;
            SDL_GetRGBA(sc, src->format, &sr, &sg, &sb, &sa);

            // Multiply by global alpha
            sa = (uint8_t)((uint32_t)sa * globalAlpha / 255u);
            if (sa == 0) continue;

            // Apply tint color to RGB channels
            sr = (uint8_t)((uint16_t)sr * tintR / 255u);
            sg = (uint8_t)((uint16_t)sg * tintG / 255u);
            sb = (uint8_t)((uint16_t)sb * tintB / 255u);

            // Read destination pixel
            uint8_t* dp = (uint8_t*)dst->pixels
                          + dpy * dst->pitch + dpx * dstBpp;
            uint32_t dc;
            switch (dstBpp) {
                case 4: dc = *(uint32_t*)dp; break;
                case 3: dc = dp[0] | ((uint32_t)dp[1] << 8) | ((uint32_t)dp[2] << 16); break;
                default: continue;
            }

            uint8_t dr, dg, db, da;
            SDL_GetRGBA(dc, dst->format, &dr, &dg, &db, &da);

            // Porter-Duff "src over dst"
            uint8_t nr = (uint8_t)((sr * sa + dr * (255u - sa)) / 255u);
            uint8_t ng = (uint8_t)((sg * sa + dg * (255u - sa)) / 255u);
            uint8_t nb = (uint8_t)((sb * sa + db * (255u - sa)) / 255u);

            uint32_t nc = SDL_MapRGBA(dst->format, nr, ng, nb, 255);
            switch (dstBpp) {
                case 4: *(uint32_t*)dp = nc; break;
                case 3:
                    dp[0] = (uint8_t)(nc >>  0);
                    dp[1] = (uint8_t)(nc >>  8);
                    dp[2] = (uint8_t)(nc >> 16);
                    break;
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Backbuffer helpers ]===

// Create the backbuffer at fixed 320x240 resolution.
static void ensureBackbuffer(SDLRenderer* sdl)
{
    if (sdl->backbuffer && sdl->backbuffer->w == 320 && sdl->backbuffer->h == 240)
        return;

    if (sdl->backbuffer) {
        SDL_FreeSurface(sdl->backbuffer);
        sdl->backbuffer = nullptr;
    }

    SDL_Surface* screen = sdl->screen;
    if (screen) {
        sdl->backbuffer = SDL_CreateRGBSurface(
            SDL_SWSURFACE, 320, 240,
            screen->format->BitsPerPixel,
            screen->format->Rmask, screen->format->Gmask,
            screen->format->Bmask, screen->format->Amask);
    } else {
        sdl->backbuffer = SDL_CreateRGBSurface(
            SDL_SWSURFACE, 320, 240, 32,
            0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    }

    if (!sdl->backbuffer)
        fprintf(stderr, "SDL: Failed to create backbuffer 320x240\n");
}

// ===[ Vtable: init ]===
static void sdlInit(Renderer* renderer, DataWin* dataWin)
{
    SDLRenderer* sdl = (SDLRenderer*)renderer;
    renderer->dataWin = dataWin;

    // Grab the already-created screen surface (set by SDL_SetVideoMode in main).
    sdl->screen = SDL_GetVideoSurface();

    // Load all TXTR pages as RGBA SDL surfaces.
    sdl->textureCount = dataWin->txtr.count;
    sdl->textures = safeCalloc(sdl->textureCount, sizeof(SDL_Surface*));

    for (uint32_t i = 0; i < sdl->textureCount; i++) {
        Texture* txtr = &dataWin->txtr.textures[i];
        if (!txtr->blobData || txtr->blobSize == 0) continue;

        int w, h, ch;
        uint8_t* px = stbi_load_from_memory(txtr->blobData, (int)txtr->blobSize,
                                             &w, &h, &ch, 4);
        if (!px) {
            fprintf(stderr, "SDL: stbi failed for TXTR %u: %s\n", i, stbi_failure_reason());
            continue;
        }

        // stbi gives R=byte0 G=byte1 B=byte2 A=byte3
        SDL_Surface* tmp = SDL_CreateRGBSurfaceFrom(
            px, w, h, 32, w * 4,
            0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
        if (!tmp) { stbi_image_free(px); continue; }

        // Convert to display-alpha format so blitting respects per-pixel alpha.
        sdl->textures[i] = SDL_DisplayFormatAlpha(tmp);
        SDL_FreeSurface(tmp);
        stbi_image_free(px);

        if (!sdl->textures[i])
            fprintf(stderr, "SDL: DisplayFormatAlpha failed for TXTR %u\n", i);
        else
            fprintf(stderr, "SDL: Loaded TXTR %u (%dx%d)\n", i, w, h);
    }

    // Pre-create a default backbuffer.
    ensureBackbuffer(sdl);

    sdl->gameW = 320; sdl->gameH = 240;
    sdl->windowW = 320; sdl->windowH = 240;

    fprintf(stderr, "SDL: Renderer initialized (%u texture pages)\n", sdl->textureCount);
}

// ===[ Vtable: destroy ]===
static void sdlDestroy(Renderer* renderer)
{
    SDLRenderer* sdl = (SDLRenderer*)renderer;

    if (sdl->backbuffer) { SDL_FreeSurface(sdl->backbuffer); sdl->backbuffer = nullptr; }

    for (uint32_t i = 0; i < sdl->textureCount; i++) {
        if (sdl->textures[i]) SDL_FreeSurface(sdl->textures[i]);
    }
    free(sdl->textures);
    free(sdl);
}

// ===[ Vtable: beginFrame ]===
static void sdlBeginFrame(Renderer* renderer,
                          int32_t gameW, int32_t gameH,
                          int32_t windowW, int32_t windowH)
{
    SDLRenderer* sdl = (SDLRenderer*)renderer;

    sdl->gameW   = gameW;   sdl->gameH   = gameH;
    sdl->windowW = windowW; sdl->windowH = windowH;
    
    // Initialize scaled port values to full screen (will be overridden by beginView)
    sdl->scaledPortX = 0;
    sdl->scaledPortY = 0;
    sdl->scaledPortW = windowW;
    sdl->scaledPortH = windowH;

    // Refresh screen pointer in case it changed.
    sdl->screen = SDL_GetVideoSurface();

    ensureBackbuffer(sdl);

    // Clear backbuffer to black.
    SDL_SetClipRect(sdl->backbuffer, nullptr);
    SDL_FillRect(sdl->backbuffer, nullptr,
                 SDL_MapRGB(sdl->backbuffer->format, 0, 0, 0));
}

// ===[ Vtable: endFrame ]===
static void sdlEndFrame(Renderer* renderer)
{
    SDLRenderer* sdl = (SDLRenderer*)renderer;

    SDL_Surface* screen = sdl->screen;
    if (!screen) return;

    SDL_SetClipRect(sdl->backbuffer, nullptr);

    // Backbuffer and screen are both 320x240, direct blit
    SDL_BlitSurface(sdl->backbuffer, nullptr, screen, nullptr);

    SDL_Flip(screen);
}

// ===[ Vtable: beginView ]===
static void sdlBeginView(Renderer* renderer,
                         int32_t viewX, int32_t viewY,
                         int32_t viewW, int32_t viewH,
                         int32_t portX, int32_t portY,
                         int32_t portW, int32_t portH,
                         float viewAngle)
{
    (void)viewAngle; // Rotation not supported in software renderer
    SDLRenderer* sdl = (SDLRenderer*)renderer;

    sdl->viewX = viewX; sdl->viewY = viewY;
    sdl->viewW = viewW; sdl->viewH = viewH;
    sdl->portX = portX; sdl->portY = portY;
    sdl->portW = portW; sdl->portH = portH;

    // Scale port coordinates to fixed 320x240 backbuffer
    float scaleX = (float)sdl->windowW / (float)sdl->gameW;
    float scaleY = (float)sdl->windowH / (float)sdl->gameH;

    // Store scaled port values for consistent coordinate transformation
    sdl->scaledPortX = (int32_t)(portX * scaleX);
    sdl->scaledPortY = (int32_t)(portY * scaleY);
    sdl->scaledPortW = (int32_t)(portW * scaleX);
    sdl->scaledPortH = (int32_t)(portH * scaleY);

    // Restrict drawing to the port rectangle.
    SDL_Rect clip = { (Sint16)sdl->scaledPortX, (Sint16)sdl->scaledPortY,
                      (Uint16)sdl->scaledPortW, (Uint16)sdl->scaledPortH };
    SDL_SetClipRect(sdl->backbuffer, &clip);
}

// ===[ Vtable: endView ]===
static void sdlEndView(Renderer* renderer)
{
    SDLRenderer* sdl = (SDLRenderer*)renderer;
    SDL_SetClipRect(sdl->backbuffer, nullptr);
}

// ===[ Vtable: drawSprite ]===
static void sdlDrawSprite(Renderer* renderer,
                          int32_t tpagIndex,
                          float x, float y,
                          float originX, float originY,
                          float xscale, float yscale,
                          float angleDeg,
                          uint32_t color, float alpha)
{
    (void)angleDeg; (void)color; // Rotation/tinting not implemented
    SDLRenderer* sdl = (SDLRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= sdl->textureCount) return;
    SDL_Surface* src = sdl->textures[pageId];
    if (!src) return;

    SDL_Rect srcRect = {
        (Sint16)tpag->sourceX, (Sint16)tpag->sourceY,
        (Uint16)tpag->sourceWidth, (Uint16)tpag->sourceHeight
    };

    // The sprite is drawn at (x,y) in game space, then origin is subtracted.
    float bx, by;
    viewToBackbuffer(sdl, x - originX, y - originY, &bx, &by);

    int dstW = (int)(scaleW(sdl, (float)tpag->sourceWidth)  * xscale);
    int dstH = (int)(scaleH(sdl, (float)tpag->sourceHeight) * yscale);

    blitScaledAlpha(src, &srcRect, sdl->backbuffer,
                    (int)bx, (int)by, dstW, dstH,
                    (uint8_t)(alpha * 255.0f));
}

// ===[ Vtable: drawSpritePart ]===
static void sdlDrawSpritePart(Renderer* renderer,
                               int32_t tpagIndex,
                               int32_t srcOffX, int32_t srcOffY,
                               int32_t srcW,    int32_t srcH,
                               float x, float y,
                               float xscale, float yscale,
                               uint32_t color, float alpha)
{
    (void)color;
    SDLRenderer* sdl = (SDLRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= sdl->textureCount) return;
    SDL_Surface* src = sdl->textures[pageId];
    if (!src) return;

    SDL_Rect srcRect = {
        (Sint16)(tpag->sourceX + srcOffX),
        (Sint16)(tpag->sourceY + srcOffY),
        (Uint16)srcW, (Uint16)srcH
    };

    float bx, by;
    viewToBackbuffer(sdl, x, y, &bx, &by);

    int dstW = (int)(scaleW(sdl, (float)srcW) * xscale);
    int dstH = (int)(scaleH(sdl, (float)srcH) * yscale);

    blitScaledAlpha(src, &srcRect, sdl->backbuffer,
                    (int)bx, (int)by, dstW, dstH,
                    (uint8_t)(alpha * 255.0f));
}

// Forward declaration
static void drawLineBackbuffer(SDL_Surface* dst, int x0, int y0, int x1, int y1,
                                uint8_t r, uint8_t g, uint8_t b);

// ===[ Vtable: drawRectangle ]===
static void sdlDrawRectangle(Renderer* renderer,
                              float x1, float y1, float x2, float y2,
                              uint32_t color, float alpha, bool outline)
{
    SDLRenderer* sdl = (SDLRenderer*)renderer;

    float bx1, by1, bx2, by2;
    viewToBackbuffer(sdl, x1, y1, &bx1, &by1);
    viewToBackbuffer(sdl, x2, y2, &bx2, &by2);

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    if (!outline) {
        // Filled rect: GML adds +1 to width/height
        bx2 += 1.0f;
        by2 += 1.0f;
        
        int ix1 = (int)floorf(fminf(bx1, bx2));
        int iy1 = (int)floorf(fminf(by1, by2));
        int ix2 = (int)ceilf(fmaxf(bx1, bx2));
        int iy2 = (int)ceilf(fmaxf(by1, by2));

        // Only draw opaque rectangles (skip semi-transparent overlay effects)
        if (a >= 255) {
            uint32_t mapped = SDL_MapRGB(sdl->backbuffer->format, r, g, b);
            SDL_Rect rect = { (Sint16)ix1, (Sint16)iy1,
                              (Uint16)(ix2 - ix1), (Uint16)(iy2 - iy1) };
            SDL_FillRect(sdl->backbuffer, &rect, mapped);
        }
        // Skip semi-transparent rectangles (darkening effects)
    } else {
        // Outline: draw 4 one-pixel-wide edges as rectangles
        // This matches the OpenGL implementation and works correctly with scaling
        uint32_t mapped = SDL_MapRGB(sdl->backbuffer->format, r, g, b);
        
        // Ensure coordinates are ordered
        if (bx1 > bx2) { float t = bx1; bx1 = bx2; bx2 = t; }
        if (by1 > by2) { float t = by1; by1 = by2; by2 = t; }
        
        // Compute integer coordinates for each edge
        // Top edge: from (bx1, by1) to (bx2, by1+1)
        int topX1 = (int)floorf(bx1);
        int topY1 = (int)floorf(by1);
        int topX2 = (int)ceilf(bx2);
        int topY2 = topY1 + 1;
        
        // Bottom edge: from (bx1, by2) to (bx2, by2+1)
        int botX1 = (int)floorf(bx1);
        int botY1 = (int)floorf(by2);
        int botX2 = (int)ceilf(bx2);
        int botY2 = botY1 + 1;
        
        // Left edge: from (bx1, by1+1) to (bx1+1, by2)
        int leftX1 = (int)floorf(bx1);
        int leftY1 = (int)floorf(by1) + 1;
        int leftX2 = leftX1 + 1;
        int leftY2 = (int)ceilf(by2);
        
        // Right edge: from (bx2, by1+1) to (bx2+1, by2)
        int rightX1 = (int)floorf(bx2);
        int rightY1 = (int)floorf(by1) + 1;
        int rightX2 = rightX1 + 1;
        int rightY2 = (int)ceilf(by2);
        
        // Draw each edge, ensuring at least 1 pixel height/width
        if (topX2 > topX1 && topY2 > topY1) {
            SDL_Rect top = { (Sint16)topX1, (Sint16)topY1, (Uint16)(topX2 - topX1), (Uint16)(topY2 - topY1) };
            SDL_FillRect(sdl->backbuffer, &top, mapped);
        }
        if (botX2 > botX1 && botY2 > botY1) {
            SDL_Rect bot = { (Sint16)botX1, (Sint16)botY1, (Uint16)(botX2 - botX1), (Uint16)(botY2 - botY1) };
            SDL_FillRect(sdl->backbuffer, &bot, mapped);
        }
        if (leftX2 > leftX1 && leftY2 > leftY1) {
            SDL_Rect left = { (Sint16)leftX1, (Sint16)leftY1, (Uint16)(leftX2 - leftX1), (Uint16)(leftY2 - leftY1) };
            SDL_FillRect(sdl->backbuffer, &left, mapped);
        }
        if (rightX2 > rightX1 && rightY2 > rightY1) {
            SDL_Rect right = { (Sint16)rightX1, (Sint16)rightY1, (Uint16)(rightX2 - rightX1), (Uint16)(rightY2 - rightY1) };
            SDL_FillRect(sdl->backbuffer, &right, mapped);
        }
    }
}

// ===[ Bresenham line helper (operates in backbuffer space) ]===
static void drawLineBackbuffer(SDL_Surface* dst, int x0, int y0, int x1, int y1,
                                uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t color = SDL_MapRGB(dst->format, r, g, b);

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    SDL_LockSurface(dst);
    int bpp = dst->format->BytesPerPixel;

    while (1) {
        // Respect clip rect
        if (x0 >= dst->clip_rect.x && x0 < dst->clip_rect.x + dst->clip_rect.w &&
            y0 >= dst->clip_rect.y && y0 < dst->clip_rect.y + dst->clip_rect.h) {
            uint8_t* p = (uint8_t*)dst->pixels + y0 * dst->pitch + x0 * bpp;
            switch (bpp) {
                case 4: *(uint32_t*)p = color; break;
                case 3:
                    p[0] = (uint8_t)(color >>  0);
                    p[1] = (uint8_t)(color >>  8);
                    p[2] = (uint8_t)(color >> 16);
                    break;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    SDL_UnlockSurface(dst);
}

// ===[ Vtable: drawLine ]===
static void sdlDrawLine(Renderer* renderer,
                         float x1, float y1, float x2, float y2,
                         float width, uint32_t color, float alpha)
{
    (void)width; (void)alpha;
    SDLRenderer* sdl = (SDLRenderer*)renderer;

    float bx1, by1, bx2, by2;
    viewToBackbuffer(sdl, x1, y1, &bx1, &by1);
    viewToBackbuffer(sdl, x2, y2, &bx2, &by2);

    drawLineBackbuffer(sdl->backbuffer,
                       (int)bx1, (int)by1, (int)bx2, (int)by2,
                       BGR_R(color), BGR_G(color), BGR_B(color));
}

// ===[ Vtable: drawLineColor ]===
static void sdlDrawLineColor(Renderer* renderer,
                              float x1, float y1, float x2, float y2,
                              float width, uint32_t color1, uint32_t color2,
                              float alpha)
{
    // Approximate with first colour only (gradient not trivial in software)
    sdlDrawLine(renderer, x1, y1, x2, y2, width, color1, alpha);
}

// ===[ Vtable: drawTriangle ]===
static void sdlDrawTriangle(Renderer* renderer,
                             float x1, float y1,
                             float x2, float y2,
                             float x3, float y3,
                             bool outline)
{
    SDLRenderer* sdl = (SDLRenderer*)renderer;
    uint32_t color = renderer->drawColor;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);

    float bx1, by1, bx2, by2, bx3, by3;
    viewToBackbuffer(sdl, x1, y1, &bx1, &by1);
    viewToBackbuffer(sdl, x2, y2, &bx2, &by2);
    viewToBackbuffer(sdl, x3, y3, &bx3, &by3);

    if (outline) {
        drawLineBackbuffer(sdl->backbuffer, (int)bx1,(int)by1,(int)bx2,(int)by2, r,g,b);
        drawLineBackbuffer(sdl->backbuffer, (int)bx2,(int)by2,(int)bx3,(int)by3, r,g,b);
        drawLineBackbuffer(sdl->backbuffer, (int)bx3,(int)by3,(int)bx1,(int)by1, r,g,b);
    } else {
        // Simple scanline fill
        // Sort vertices by Y
        float ax=bx1,ay=by1, cx=bx2,cy=by2, ex=bx3,ey=by3;
        if (ay > cy) { float t; t=ax;ax=cx;cx=t; t=ay;ay=cy;cy=t; }
        if (ay > ey) { float t; t=ax;ax=ex;ex=t; t=ay;ay=ey;ey=t; }
        if (cy > ey) { float t; t=cx;cx=ex;ex=t; t=cy;cy=ey;ey=t; }

        uint32_t mapped = SDL_MapRGB(sdl->backbuffer->format, r, g, b);
        int yStart = (int)ay, yEnd = (int)ey;
        float totalH = ey - ay;
        if (totalH < 1.0f) return;

        SDL_LockSurface(sdl->backbuffer);
        int bpp = sdl->backbuffer->format->BytesPerPixel;
        SDL_Rect* cr = &sdl->backbuffer->clip_rect;

        for (int y = yStart; y <= yEnd; y++) {
            if (y < cr->y || y >= cr->y + cr->h) continue;
            float t = (y - ay) / totalH;
            float xa = ax + (ex - ax) * t;
            bool secondHalf = (y - ay > cy - ay);
            float segH = secondHalf ? (ey - cy) : (cy - ay);
            float t2   = segH > 0.01f
                         ? (y - (secondHalf ? cy : ay)) / segH
                         : 0.0f;
            float xb = secondHalf ? (cx + (ex - cx) * t2)
                                   : (ax + (cx - ax) * t2);
            int xLeft  = (int)(xa < xb ? xa : xb);
            int xRight = (int)(xa < xb ? xb : xa);
            xLeft  = xLeft  < cr->x              ? cr->x              : xLeft;
            xRight = xRight > cr->x + cr->w - 1  ? cr->x + cr->w - 1 : xRight;
            for (int x = xLeft; x <= xRight; x++) {
                uint8_t* p = (uint8_t*)sdl->backbuffer->pixels
                             + y * sdl->backbuffer->pitch + x * bpp;
                switch (bpp) {
                    case 4: *(uint32_t*)p = mapped; break;
                    case 3:
                        p[0]=(uint8_t)(mapped>>0);
                        p[1]=(uint8_t)(mapped>>8);
                        p[2]=(uint8_t)(mapped>>16);
                        break;
                }
            }
        }
        SDL_UnlockSurface(sdl->backbuffer);
    }
}

// ===[ Vtable: drawText ]===
// Renders text using the game's own bitmap font glyphs.
// Falls back silently if the font is missing or not set.
static void sdlDrawText(Renderer* renderer,
                         const char* text,
                         float x, float y,
                         float xscale, float yscale,
                         float angleDeg)
{
    (void)angleDeg;
    SDLRenderer* sdl = (SDLRenderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (!text || !text[0]) return;

    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || (uint32_t)fontIndex >= dw->font.count) return;

    Font* font = &dw->font.fonts[fontIndex];
    if (font->glyphCount == 0) return;

    int32_t tpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* baseTpag = &dw->tpag.items[tpagIndex];

    int16_t pageId = baseTpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= sdl->textureCount) return;
    SDL_Surface* src = sdl->textures[pageId];
    if (!src) return;

    int32_t textLen = (int32_t)strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float totalHeight = (float)lineCount * (float)font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset;
    int32_t lineStart = 0;
    uint8_t ga = (uint8_t)(renderer->drawAlpha * 255.0f);
    uint32_t tintColor = renderer->drawColor; // BGR format

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;

        while (pos < lineLen) {
            uint16_t ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            // Округляем координаты до целых для устранения дрожания
            float gx = x + (cursorX + (float)glyph->offset) * xscale * font->scaleX;
            float gy = y + cursorY * yscale * font->scaleY;

            // Округляем до целых пикселей в game space перед масштабированием
            gx = roundf(gx);
            gy = roundf(gy);

            float bx, by;
            viewToBackbuffer(sdl, gx, gy, &bx, &by);

            SDL_Rect srcRect = {
                (Sint16)(baseTpag->sourceX + glyph->sourceX),
                (Sint16)(baseTpag->sourceY + glyph->sourceY),
                (Uint16)glyph->sourceWidth,
                (Uint16)glyph->sourceHeight
            };
            int dstW = (int)(scaleW(sdl, (float)glyph->sourceWidth)  * xscale * font->scaleX);
            int dstH = (int)(scaleH(sdl, (float)glyph->sourceHeight) * yscale * font->scaleY);

            // Use tinted blit to apply drawColor to text
            blitScaledAlphaTint(src, &srcRect, sdl->backbuffer,
                            (int)bx, (int)by, dstW, dstH, ga, tintColor);

            cursorX += glyph->shift;
            if (pos < lineLen) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float)font->emSize;
        if (lineEnd < textLen)
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        else
            lineStart = lineEnd;
    }
}

// ===[ Vtable: drawTextColor ]===
// Multi-colour variant – falls back to drawText with top-left colour.
static void sdlDrawTextColor(Renderer* renderer,
                              const char* text,
                              float x, float y,
                              float xscale, float yscale,
                              float angleDeg,
                              int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                              float alpha)
{
    (void)c2; (void)c3; (void)c4;
    uint32_t savedColor = renderer->drawColor;
    float    savedAlpha = renderer->drawAlpha;
    renderer->drawColor = (uint32_t)c1;
    renderer->drawAlpha = alpha;
    sdlDrawText(renderer, text, x, y, xscale, yscale, angleDeg);
    renderer->drawColor = savedColor;
    renderer->drawAlpha = savedAlpha;
}

// ===[ Vtable: flush ]===
static void sdlFlush(Renderer* renderer) { (void)renderer; /* no-op */ }

// ===[ Vtable: createSpriteFromSurface / deleteSprite ]===
static int32_t sdlCreateSpriteFromSurface(Renderer* renderer,
                                           int32_t x, int32_t y,
                                           int32_t w, int32_t h,
                                           bool removeback, bool smooth,
                                           int32_t xorig, int32_t yorig)
{
    (void)renderer; (void)x; (void)y; (void)w; (void)h;
    (void)removeback; (void)smooth; (void)xorig; (void)yorig;
    fprintf(stderr, "SDL: createSpriteFromSurface not implemented\n");
    return -1;
}

static void sdlDeleteSprite(Renderer* renderer, int32_t spriteIndex)
{
    (void)renderer; (void)spriteIndex;
}

// ===[ Vtable ]===
#ifdef __cplusplus
static RendererVtable sdlVtable = {
    sdlInit,               // init
    sdlDestroy,            // destroy
    sdlBeginFrame,         // beginFrame
    sdlEndFrame,           // endFrame
    sdlBeginView,          // beginView
    sdlEndView,            // endView
    sdlDrawSprite,         // drawSprite
    sdlDrawSpritePart,     // drawSpritePart
    sdlDrawRectangle,      // drawRectangle
    sdlDrawLine,           // drawLine
    sdlDrawLineColor,      // drawLineColor
    sdlDrawTriangle,       // drawTriangle
    sdlDrawText,           // drawText
    sdlDrawTextColor,      // drawTextColor
    sdlFlush,              // flush
    sdlCreateSpriteFromSurface, // createSpriteFromSurface
    sdlDeleteSprite,       // deleteSprite
    NULL,                  // drawTile
};
#else
static RendererVtable sdlVtable = {
    .init                  = sdlInit,
    .destroy               = sdlDestroy,
    .beginFrame            = sdlBeginFrame,
    .endFrame              = sdlEndFrame,
    .beginView             = sdlBeginView,
    .endView               = sdlEndView,
    .drawSprite            = sdlDrawSprite,
    .drawSpritePart        = sdlDrawSpritePart,
    .drawRectangle         = sdlDrawRectangle,
    .drawLine              = sdlDrawLine,
    .drawLineColor         = sdlDrawLineColor,
    .drawTriangle          = sdlDrawTriangle,
    .drawText              = sdlDrawText,
    .drawTextColor         = sdlDrawTextColor,
    .flush                 = sdlFlush,
    .createSpriteFromSurface = sdlCreateSpriteFromSurface,
    .deleteSprite          = sdlDeleteSprite,
    .drawTile              = NULL,
};
#endif

// ===[ Public constructor ]===
Renderer* SDLRenderer_create(void)
{
    SDLRenderer* sdl = safeCalloc(1, sizeof(SDLRenderer));
    sdl->base.vtable     = &sdlVtable;
    sdl->base.drawColor  = 0xFFFFFF; // white (BGR)
    sdl->base.drawAlpha  = 1.0f;
    sdl->base.drawFont   = -1;
    sdl->base.drawHalign = 0;
    sdl->base.drawValign = 0;
    return (Renderer*)sdl;
}
