#include "sdl_renderer.h"
#include "data_win.h"
#include "text_utils.h"
#include "matrix_math.h"
#include "utils.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>

#include "stb_image.h"

// ===[ Константы кэша ]===
#define TEXTURE_CACHE_CAPACITY  64
#define TEXTURE_CACHE_THRESHOLD 32

// Forward declaration for loading screen
static void drawLoadingText(const char* text, SDL_Surface* screen);

// Global TTF font for loading screen
static TTF_Font* g_loadingFont = NULL;


// ===[ Быстрый доступ к пиксельному формату ]===
typedef struct {
    uint32_t rMask, gMask, bMask, aMask;
    uint8_t  rShift, gShift, bShift, aShift;
    int      bpp;
} FastFmt;

static inline void FastFmt_init(FastFmt* f, const SDL_PixelFormat* fmt) {
    f->rMask  = fmt->Rmask;  f->rShift = fmt->Rshift;
    f->gMask  = fmt->Gmask;  f->gShift = fmt->Gshift;
    f->bMask  = fmt->Bmask;  f->bShift = fmt->Bshift;
    f->aMask  = fmt->Amask;  f->aShift = fmt->Ashift;
    f->bpp    = fmt->BytesPerPixel;
}

static inline uint32_t FastFmt_read(const uint8_t* p, int bpp) {
    if (bpp == 4) return *(const uint32_t*)p;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline void FastFmt_write(uint8_t* p, uint32_t v, int bpp) {
    if (bpp == 4) { *(uint32_t*)p = v; return; }
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}

static inline uint8_t FastFmt_A(uint32_t px, const FastFmt* f) {
    return f->aMask ? (uint8_t)((px & f->aMask) >> f->aShift) : 255u;
}
static inline uint8_t FastFmt_R(uint32_t px, const FastFmt* f) {
    return (uint8_t)((px & f->rMask) >> f->rShift);
}
static inline uint8_t FastFmt_G(uint32_t px, const FastFmt* f) {
    return (uint8_t)((px & f->gMask) >> f->gShift);
}
static inline uint8_t FastFmt_B(uint32_t px, const FastFmt* f) {
    return (uint8_t)((px & f->bMask) >> f->bShift);
}

static inline uint32_t FastFmt_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                    const FastFmt* f) {
    return ((uint32_t)r << f->rShift)
         | ((uint32_t)g << f->gShift)
         | ((uint32_t)b << f->bShift)
         | (f->aMask ? ((uint32_t)a << f->aShift) : 0u);
}

// ===[ Fast div255 ]===
static inline uint32_t div255fast(uint32_t x) {
    return (x + (x >> 8u) + 1u) >> 8u;
}

// ===[ LUT координат источника ]===
#define SPX_LUT_MAX 2048
#define SPY_LUT_MAX 2048
static int s_spxLUT[SPX_LUT_MAX];
static int s_spyLUT[SPY_LUT_MAX];

static inline void buildSpxLUT(int count, int sx0, int startDx, int sw, int dstW) {
    for (int i = 0; i < count; i++) {
        s_spxLUT[i] = sx0 + (int)((float)(startDx + i) * (float)sw / (float)dstW);
    }
}

static inline void buildSpyLUT(int count, int sy0, int startDy, int sh, int dstH) {
    for (int j = 0; j < count; j++) {
        s_spyLUT[j] = sy0 + (int)((float)(startDy + j) * (float)sh / (float)dstH);
    }
}

// ===[ Texture Cache ]===
typedef struct {
    uint32_t txtrId;
    uint32_t lastUsed;
    uint16_t width;
    uint16_t height;
    uint8_t*     pixels;
    SDL_Surface* surface;
    FastFmt fmt;
    bool    fmtReady;
} TextureCacheEntry;

#define CACHE_HASH_SIZE 128u
#define CACHE_HASH_MASK (CACHE_HASH_SIZE - 1u)
#define CACHE_EMPTY_SLOT 0xFFFFFFFFu

typedef struct {
    TextureCacheEntry entries[TEXTURE_CACHE_CAPACITY];
    uint32_t hashSlots[CACHE_HASH_SIZE];
    size_t   count;
    uint32_t frameCounter;
} TextureCache;

static inline uint32_t cacheHash(uint32_t id) {
    id = (id ^ 61u) ^ (id >> 16u);
    id *= 9u;
    id ^= id >> 4u;
    id *= 0x27d4eb2du;
    id ^= id >> 15u;
    return id & CACHE_HASH_MASK;
}

static void TextureCache_init(TextureCache* cache) {
    memset(cache, 0, sizeof(TextureCache));
    memset(cache->hashSlots, 0xFF, sizeof(cache->hashSlots));
}

static TextureCacheEntry* TextureCache_find(TextureCache* cache, uint32_t txtrId) {
    uint32_t h = cacheHash(txtrId);
    for (uint32_t probe = 0; probe < CACHE_HASH_SIZE; probe++) {
        uint32_t idx = cache->hashSlots[(h + probe) & CACHE_HASH_MASK];
        if (idx == CACHE_EMPTY_SLOT) return NULL;
        if (cache->entries[idx].txtrId == txtrId) return &cache->entries[idx];
    }
    return NULL;
}

static void TextureCache_hashInsert(TextureCache* cache, uint32_t txtrId, uint32_t entryIdx) {
    uint32_t h = cacheHash(txtrId);
    for (uint32_t probe = 0; probe < CACHE_HASH_SIZE; probe++) {
        uint32_t slot = (h + probe) & CACHE_HASH_MASK;
        if (cache->hashSlots[slot] == CACHE_EMPTY_SLOT) {
            cache->hashSlots[slot] = entryIdx;
            return;
        }
    }
}

static void TextureCache_hashRemove(TextureCache* cache, uint32_t txtrId) {
    uint32_t h = cacheHash(txtrId);
    uint32_t removeSlot = CACHE_HASH_SIZE;
    for (uint32_t probe = 0; probe < CACHE_HASH_SIZE; probe++) {
        uint32_t slot = (h + probe) & CACHE_HASH_MASK;
        uint32_t idx = cache->hashSlots[slot];
        if (idx == CACHE_EMPTY_SLOT) break;
        if (cache->entries[idx].txtrId == txtrId) { removeSlot = slot; break; }
    }
    if (removeSlot == CACHE_HASH_SIZE) return;
    uint32_t cur = removeSlot;
    for (;;) {
        uint32_t next = (cur + 1u) & CACHE_HASH_MASK;
        if (cache->hashSlots[next] == CACHE_EMPTY_SLOT) break;
        uint32_t nextIdx = cache->hashSlots[next];
        uint32_t natural = cacheHash(cache->entries[nextIdx].txtrId);
        bool move = (natural <= cur)
                 || ((next < cur) && (natural > cur || natural <= next));
        if (!move) break;
        cache->hashSlots[cur]  = cache->hashSlots[next];
        cache->hashSlots[next] = CACHE_EMPTY_SLOT;
        cur = next;
    }
    cache->hashSlots[cur] = CACHE_EMPTY_SLOT;
}

static void TextureCache_evictLRU(TextureCache* cache) {
    size_t lruIdx = 0;
    uint32_t lruFrame = cache->entries[0].lastUsed;
    for (size_t i = 1; i < cache->count; i++) {
        if (cache->entries[i].lastUsed < lruFrame) {
            lruFrame = cache->entries[i].lastUsed;
            lruIdx = i;
        }
    }
    TextureCacheEntry* e = &cache->entries[lruIdx];
    TextureCache_hashRemove(cache, e->txtrId);
    if (e->pixels)  { free(e->pixels);            e->pixels  = NULL; }
    if (e->surface) { SDL_FreeSurface(e->surface); e->surface = NULL; }
    e->txtrId = CACHE_EMPTY_SLOT;
    e->lastUsed = 0;
    e->width = e->height = 0;
    e->fmtReady = false;
}

static TextureCacheEntry* TextureCache_getOrLoad(TextureCache* cache, SDLRenderer* sdl,
                                                  DataWin* dataWin, uint32_t txtrId) {
    cache->frameCounter++;
    TextureCacheEntry* entry = TextureCache_find(cache, txtrId);
    if (entry) { entry->lastUsed = cache->frameCounter; return entry; }

    if (cache->count >= TEXTURE_CACHE_THRESHOLD) TextureCache_evictLRU(cache);

    entry = NULL;
    for (size_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].txtrId == CACHE_EMPTY_SLOT) { entry = &cache->entries[i]; break; }
    }
    if (!entry) {
        if (cache->count >= TEXTURE_CACHE_CAPACITY) {
            TextureCache_evictLRU(cache);
            entry = &cache->entries[cache->count - 1];
        } else {
            entry = &cache->entries[cache->count++];
        }
    }

    memset(entry, 0, sizeof(TextureCacheEntry));
    entry->txtrId   = txtrId;
    entry->lastUsed = cache->frameCounter;

    uint32_t entryIdx = (uint32_t)(entry - cache->entries);
    TextureCache_hashInsert(cache, txtrId, entryIdx);

    if (txtrId >= dataWin->txtr.count) {
        fprintf(stderr, "SDL-OPT: Texture ID %u out of range (max %u)\n",
                txtrId, dataWin->txtr.count);
        return entry;
    }

    Texture* txtr = &dataWin->txtr.textures[txtrId];
    if (!txtr->blobData || txtr->blobSize == 0) {
        fprintf(stderr, "SDL-OPT: Texture %u has no data\n", txtrId);
        return entry;
    }

    if (sdl && sdl->screen) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Loading spritesheet %u", txtrId);
        drawLoadingText(msg, sdl->screen);
    }

    int w, h, ch;
    uint8_t* px = stbi_load_from_memory(txtr->blobData, (int)txtr->blobSize,
                                         &w, &h, &ch, 4);
    if (!px) {
        fprintf(stderr, "SDL-OPT: stbi failed for TXTR %u: %s\n",
                txtrId, stbi_failure_reason());
        return entry;
    }

    entry->width  = (uint16_t)w;
    entry->height = (uint16_t)h;
    entry->pixels = px;
    fprintf(stderr, "SDL-OPT: Loaded TXTR %u (%dx%d)\n", txtrId, w, h);
    (void)sdl;
    return entry;
}

static SDL_Surface* TextureCache_getSurface(TextureCacheEntry* entry, SDLRenderer* sdl) {
    (void)sdl;
    if (!entry->pixels && !entry->surface) return NULL;
    if (!entry->surface && entry->pixels) {
        entry->surface = SDL_CreateRGBSurfaceFrom(
            entry->pixels, entry->width, entry->height, 32, entry->width * 4,
            0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
        if (entry->surface) {
            SDL_Surface* converted = SDL_DisplayFormatAlpha(entry->surface);
            SDL_FreeSurface(entry->surface);
            entry->surface = converted;
            free(entry->pixels);
            entry->pixels  = NULL;
            if (converted) {
                FastFmt_init(&entry->fmt, converted->format);
                entry->fmtReady = true;
            }
        }
    }
    return entry->surface;
}

static void TextureCache_cleanup(TextureCache* cache) {
    for (size_t i = 0; i < cache->count; i++) {
        TextureCacheEntry* e = &cache->entries[i];
        if (e->pixels)  free(e->pixels);
        if (e->surface) SDL_FreeSurface(e->surface);
    }
}

// ===[ Coordinate Transform ]===
static void viewToScreen(const SDLRenderer* sdl,
                         float gx, float gy, float* bx, float* by)
{
    float px, py;
    if (sdl->viewW > 0 && sdl->viewH > 0) {
        px = (gx - sdl->viewX) * (float)sdl->portW / (float)sdl->viewW;
        py = (gy - sdl->viewY) * (float)sdl->portH / (float)sdl->viewH;
    } else {
        px = gx; py = gy;
    }
    float scaleX = (float)sdl->windowW / (float)sdl->gameW;
    float scaleY = (float)sdl->windowH / (float)sdl->gameH;
    *bx = (sdl->portX + px) * scaleX;
    *by = (sdl->portY + py) * scaleY;
}

static float scaleW(const SDLRenderer* sdl, float w) {
    float sx = (float)sdl->windowW / (float)sdl->gameW;
    return (sdl->viewW > 0) ? w * (float)sdl->portW / (float)sdl->viewW * sx : w * sx;
}
static float scaleH(const SDLRenderer* sdl, float h) {
    float sy = (float)sdl->windowH / (float)sdl->gameH;
    return (sdl->viewH > 0) ? h * (float)sdl->portH / (float)sdl->viewH * sy : h * sy;
}

// ===[ Clip helper ]===
static bool computeClip(SDL_Surface* dst,
                         int dstX, int dstY, int dstW, int dstH,
                         int* outStartX, int* outStartY,
                         int* outEndX,   int* outEndY,
                         int* outSpanW,  int* outSpanH)
{
    int cx1 = dst->clip_rect.x;
    int cy1 = dst->clip_rect.y;
    int cx2 = cx1 + (int)dst->clip_rect.w; if (cx2 > dst->w) cx2 = dst->w;
    int cy2 = cy1 + (int)dst->clip_rect.h; if (cy2 > dst->h) cy2 = dst->h;

    *outStartX = dstX > cx1 ? dstX : cx1;
    *outStartY = dstY > cy1 ? dstY : cy1;
    *outEndX   = (dstX + dstW) < cx2 ? (dstX + dstW) : cx2;
    *outEndY   = (dstY + dstH) < cy2 ? (dstY + dstH) : cy2;

    if (*outStartX >= *outEndX || *outStartY >= *outEndY) return false;

    *outSpanW = *outEndX - *outStartX;
    *outSpanH = *outEndY - *outStartY;
    if (*outSpanW > SPX_LUT_MAX) *outSpanW = SPX_LUT_MAX;
    if (*outSpanH > SPY_LUT_MAX) *outSpanH = SPY_LUT_MAX;
    return true;
}

// ===[ Blit со спрайтовым alpha-blend + globalAlpha ]===
//
// globalAlpha == 255 → оригинальный fast path без лишних операций
// globalAlpha == 0   → ранний выход
// иначе              → sa = div255fast(sa * globalAlpha), затем обычный blend
static void blitScaledAlphaOptimized(SDL_Surface* src, const SDL_Rect* srcRect,
                                      SDL_Surface* dst, int dstX, int dstY,
                                      int dstW, int dstH,
                                      uint8_t globalAlpha)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;
    if (globalAlpha == 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    int startX, startY, endX, endY, spanW, spanH;
    if (!computeClip(dst, dstX, dstY, dstW, dstH,
                     &startX, &startY, &endX, &endY, &spanW, &spanH)) return;
    (void)endX; (void)endY;

    buildSpxLUT(spanW, sx0, startX - dstX, sw, dstW);
    buildSpyLUT(spanH, sy0, startY - dstY, sh, dstH);

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    FastFmt sf, df;
    FastFmt_init(&sf, src->format);
    FastFmt_init(&df, dst->format);

    if (sf.bpp == 4 && df.bpp == 4) {

        if (globalAlpha == 255) {
            // ===[ Fast path: globalAlpha полный — ноль лишних операций ]===
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                const uint32_t* srcRow =
                    (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                uint32_t* dstRow =
                    (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    uint32_t sc = srcRow[spx];
                    uint8_t sa = FastFmt_A(sc, &sf);
                    if (sa == 0) continue;

                    uint8_t sr = FastFmt_R(sc, &sf);
                    uint8_t sg = FastFmt_G(sc, &sf);
                    uint8_t sb = FastFmt_B(sc, &sf);

                    uint32_t* dp = dstRow + (startX + i);

                    if (sa == 255) {
                        *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                    } else {
                        uint32_t dc   = *dp;
                        uint32_t invA = 255u - sa;
                        uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                        uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                        uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                        *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                    }
                }
            }
        } else {
            // ===[ Global alpha path: +1 div255fast на пиксель ]===
            for (int j = 0; j < spanH; j++) {
                int spy = s_spyLUT[j];
                if ((unsigned)spy >= (unsigned)src->h) continue;

                const uint32_t* srcRow =
                    (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                uint32_t* dstRow =
                    (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                for (int i = 0; i < spanW; i++) {
                    int spx = s_spxLUT[i];
                    if ((unsigned)spx >= (unsigned)src->w) continue;

                    uint32_t sc = srcRow[spx];
                    // Единственное отличие: масштабируем sa на globalAlpha
                    uint8_t sa = (uint8_t)div255fast(
                        (uint32_t)FastFmt_A(sc, &sf) * globalAlpha);
                    if (sa == 0) continue;

                    uint8_t sr = FastFmt_R(sc, &sf);
                    uint8_t sg = FastFmt_G(sc, &sf);
                    uint8_t sb = FastFmt_B(sc, &sf);

                    uint32_t* dp = dstRow + (startX + i);

                    if (sa == 255) {
                        *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                    } else {
                        uint32_t dc   = *dp;
                        uint32_t invA = 255u - sa;
                        uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                        uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                        uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                        *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                    }
                }
            }
        }

    } else {
        // ===[ Общий путь для нестандартных форматов ]===
        const int srcBpp = sf.bpp;
        const int dstBpp = df.bpp;

        for (int j = 0; j < spanH; j++) {
            int spy = s_spyLUT[j];
            if ((unsigned)spy >= (unsigned)src->h) continue;

            const uint8_t* srcLine =
                (const uint8_t*)src->pixels + spy * src->pitch;
            uint8_t* dstLine =
                (uint8_t*)dst->pixels + (startY + j) * dst->pitch;

            for (int i = 0; i < spanW; i++) {
                int spx = s_spxLUT[i];
                if ((unsigned)spx >= (unsigned)src->w) continue;

                uint32_t sc = FastFmt_read(srcLine + spx * srcBpp, srcBpp);
                uint8_t sa = FastFmt_A(sc, &sf);
                if (globalAlpha != 255)
                    sa = (uint8_t)div255fast((uint32_t)sa * globalAlpha);
                if (sa == 0) continue;

                uint8_t sr = FastFmt_R(sc, &sf);
                uint8_t sg = FastFmt_G(sc, &sf);
                uint8_t sb = FastFmt_B(sc, &sf);

                uint8_t* dp = dstLine + (startX + i) * dstBpp;

                if (sa == 255) {
                    FastFmt_write(dp, FastFmt_pack(sr, sg, sb, 255, &df), dstBpp);
                } else {
                    uint32_t dc   = FastFmt_read(dp, dstBpp);
                    uint32_t invA = 255u - sa;
                    uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                    uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                    uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                    FastFmt_write(dp, FastFmt_pack(nr, ng, nb, 255, &df), dstBpp);
                }
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Threshold blit для тайлов ]===
static void blitScaledThreshold(SDL_Surface* src, const SDL_Rect* srcRect,
                                  SDL_Surface* dst, int dstX, int dstY,
                                  int dstW, int dstH)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    int startX, startY, endX, endY, spanW, spanH;
    if (!computeClip(dst, dstX, dstY, dstW, dstH,
                     &startX, &startY, &endX, &endY, &spanW, &spanH)) return;
    (void)endX; (void)endY;

    buildSpxLUT(spanW, sx0, startX - dstX, sw, dstW);
    buildSpyLUT(spanH, sy0, startY - dstY, sh, dstH);

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    FastFmt sf, df;
    FastFmt_init(&sf, src->format);
    FastFmt_init(&df, dst->format);

    if (sf.bpp == 4 && df.bpp == 4) {
        for (int j = 0; j < spanH; j++) {
            int spy = s_spyLUT[j];
            if ((unsigned)spy >= (unsigned)src->h) continue;

            const uint32_t* srcRow =
                (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
            uint32_t* dstRow =
                (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

            for (int i = 0; i < spanW; i++) {
                int spx = s_spxLUT[i];
                if ((unsigned)spx >= (unsigned)src->w) continue;

                uint32_t sc = srcRow[spx];
                if (FastFmt_A(sc, &sf) < 128) continue;

                uint32_t* dp = dstRow + (startX + i);
                *dp = FastFmt_pack(FastFmt_R(sc, &sf),
                                   FastFmt_G(sc, &sf),
                                   FastFmt_B(sc, &sf),
                                   255, &df);
            }
        }
    } else {
        const int srcBpp = sf.bpp;
        const int dstBpp = df.bpp;

        for (int j = 0; j < spanH; j++) {
            int spy = s_spyLUT[j];
            if ((unsigned)spy >= (unsigned)src->h) continue;

            const uint8_t* srcLine =
                (const uint8_t*)src->pixels + spy * src->pitch;
            uint8_t* dstLine =
                (uint8_t*)dst->pixels + (startY + j) * dst->pitch;

            for (int i = 0; i < spanW; i++) {
                int spx = s_spxLUT[i];
                if ((unsigned)spx >= (unsigned)src->w) continue;

                uint32_t sc = FastFmt_read(srcLine + spx * srcBpp, srcBpp);
                if (FastFmt_A(sc, &sf) < 128) continue;

                FastFmt_write(dstLine + (startX + i) * dstBpp,
                              FastFmt_pack(FastFmt_R(sc, &sf),
                                           FastFmt_G(sc, &sf),
                                           FastFmt_B(sc, &sf),
                                           255, &df),
                              dstBpp);
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Tinted blit для текста и спрайтов с цветом ]===
//
// globalAlpha == 255 → ветка без умножения alpha
// globalAlpha == 0   → ранний выход
// иначе              → sa = div255fast(sa * globalAlpha), полный blend
static void blitScaledAlphaOptimizedTint(SDL_Surface* src, const SDL_Rect* srcRect,
                                          SDL_Surface* dst, int dstX, int dstY,
                                          int dstW, int dstH,
                                          uint32_t tintColor,
                                          uint8_t globalAlpha)
{
    if (!src || !dst || dstW <= 0 || dstH <= 0) return;
    if (globalAlpha == 0) return;

    int sx0 = srcRect ? srcRect->x : 0;
    int sy0 = srcRect ? srcRect->y : 0;
    int sw  = srcRect ? srcRect->w : src->w;
    int sh  = srcRect ? srcRect->h : src->h;
    if (sw <= 0 || sh <= 0) return;

    const uint8_t tintR = BGR_R(tintColor);
    const uint8_t tintG = BGR_G(tintColor);
    const uint8_t tintB = BGR_B(tintColor);
    const bool whiteTint = (tintR == 255 && tintG == 255 && tintB == 255);

    int startX, startY, endX, endY, spanW, spanH;
    if (!computeClip(dst, dstX, dstY, dstW, dstH,
                     &startX, &startY, &endX, &endY, &spanW, &spanH)) return;
    (void)endX; (void)endY;

    buildSpxLUT(spanW, sx0, startX - dstX, sw, dstW);
    buildSpyLUT(spanH, sy0, startY - dstY, sh, dstH);

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    FastFmt sf, df;
    FastFmt_init(&sf, src->format);
    FastFmt_init(&df, dst->format);

    if (sf.bpp == 4 && df.bpp == 4) {

        if (globalAlpha == 255) {
            // ===[ Full opacity path — оригинальное поведение ]===
            if (whiteTint) {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        if (FastFmt_A(sc, &sf) < 128) continue;

                        dstRow[startX + i] = FastFmt_pack(FastFmt_R(sc, &sf),
                                                           FastFmt_G(sc, &sf),
                                                           FastFmt_B(sc, &sf),
                                                           255, &df);
                    }
                }
            } else {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        if (FastFmt_A(sc, &sf) < 128) continue;

                        uint8_t tr = (uint8_t)div255fast((uint32_t)FastFmt_R(sc, &sf) * tintR);
                        uint8_t tg = (uint8_t)div255fast((uint32_t)FastFmt_G(sc, &sf) * tintG);
                        uint8_t tb = (uint8_t)div255fast((uint32_t)FastFmt_B(sc, &sf) * tintB);

                        dstRow[startX + i] = FastFmt_pack(tr, tg, tb, 255, &df);
                    }
                }
            }

        } else {
            // ===[ Global alpha path — полный blend с масштабированием sa ]===
            if (whiteTint) {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        uint8_t sa = (uint8_t)div255fast(
                            (uint32_t)FastFmt_A(sc, &sf) * globalAlpha);
                        if (sa == 0) continue;

                        uint8_t sr = FastFmt_R(sc, &sf);
                        uint8_t sg = FastFmt_G(sc, &sf);
                        uint8_t sb = FastFmt_B(sc, &sf);

                        uint32_t* dp = dstRow + (startX + i);

                        if (sa == 255) {
                            *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                        } else {
                            uint32_t dc   = *dp;
                            uint32_t invA = 255u - sa;
                            uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                            uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                            uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                            *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                        }
                    }
                }
            } else {
                for (int j = 0; j < spanH; j++) {
                    int spy = s_spyLUT[j];
                    if ((unsigned)spy >= (unsigned)src->h) continue;

                    const uint32_t* srcRow =
                        (const uint32_t*)((const uint8_t*)src->pixels + spy * src->pitch);
                    uint32_t* dstRow =
                        (uint32_t*)((uint8_t*)dst->pixels + (startY + j) * dst->pitch);

                    for (int i = 0; i < spanW; i++) {
                        int spx = s_spxLUT[i];
                        if ((unsigned)spx >= (unsigned)src->w) continue;

                        uint32_t sc = srcRow[spx];
                        uint8_t sa = (uint8_t)div255fast(
                            (uint32_t)FastFmt_A(sc, &sf) * globalAlpha);
                        if (sa == 0) continue;

                        uint8_t sr = (uint8_t)div255fast((uint32_t)FastFmt_R(sc, &sf) * tintR);
                        uint8_t sg = (uint8_t)div255fast((uint32_t)FastFmt_G(sc, &sf) * tintG);
                        uint8_t sb = (uint8_t)div255fast((uint32_t)FastFmt_B(sc, &sf) * tintB);

                        uint32_t* dp = dstRow + (startX + i);

                        if (sa == 255) {
                            *dp = FastFmt_pack(sr, sg, sb, 255, &df);
                        } else {
                            uint32_t dc   = *dp;
                            uint32_t invA = 255u - sa;
                            uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                            uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                            uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                            *dp = FastFmt_pack(nr, ng, nb, 255, &df);
                        }
                    }
                }
            }
        }

    } else {
        // ===[ Общий путь для нестандартных форматов ]===
        const int srcBpp = sf.bpp;
        const int dstBpp = df.bpp;

        for (int j = 0; j < spanH; j++) {
            int spy = s_spyLUT[j];
            if ((unsigned)spy >= (unsigned)src->h) continue;

            const uint8_t* srcLine =
                (const uint8_t*)src->pixels + spy * src->pitch;
            uint8_t* dstLine =
                (uint8_t*)dst->pixels + (startY + j) * dst->pitch;

            for (int i = 0; i < spanW; i++) {
                int spx = s_spxLUT[i];
                if ((unsigned)spx >= (unsigned)src->w) continue;

                uint32_t sc = FastFmt_read(srcLine + spx * srcBpp, srcBpp);
                uint8_t sa = FastFmt_A(sc, &sf);
                if (globalAlpha != 255)
                    sa = (uint8_t)div255fast((uint32_t)sa * globalAlpha);
                if (sa == 0) continue;

                uint8_t sr, sg, sb;
                if (whiteTint) {
                    sr = FastFmt_R(sc, &sf);
                    sg = FastFmt_G(sc, &sf);
                    sb = FastFmt_B(sc, &sf);
                } else {
                    sr = (uint8_t)div255fast((uint32_t)FastFmt_R(sc, &sf) * tintR);
                    sg = (uint8_t)div255fast((uint32_t)FastFmt_G(sc, &sf) * tintG);
                    sb = (uint8_t)div255fast((uint32_t)FastFmt_B(sc, &sf) * tintB);
                }

                uint8_t* dp = dstLine + (startX + i) * dstBpp;

                if (sa == 255) {
                    FastFmt_write(dp, FastFmt_pack(sr, sg, sb, 255, &df), dstBpp);
                } else {
                    uint32_t dc   = FastFmt_read(dp, dstBpp);
                    uint32_t invA = 255u - sa;
                    uint8_t nr = (uint8_t)div255fast((uint32_t)sr * sa + (uint32_t)FastFmt_R(dc, &df) * invA);
                    uint8_t ng = (uint8_t)div255fast((uint32_t)sg * sa + (uint32_t)FastFmt_G(dc, &df) * invA);
                    uint8_t nb = (uint8_t)div255fast((uint32_t)sb * sa + (uint32_t)FastFmt_B(dc, &df) * invA);
                    FastFmt_write(dp, FastFmt_pack(nr, ng, nb, 255, &df), dstBpp);
                }
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

// ===[ Renderer Struct ]===
typedef struct {
    SDLRenderer base;
    TextureCache textureCache;
    FastFmt screenFmt;
    bool    screenFmtReady;
} SDLRendererOpt;

// ===[ Loading screen ]===
static void drawLoadingText(const char* text, SDL_Surface* screen) {
    if (!screen || !text) return;

    char loadingStr[128];
    snprintf(loadingStr, sizeof(loadingStr), "%s, please wait...", text);

    if (g_loadingFont != NULL) {
        SDL_Color textColor = {255, 255, 255, 255};
        SDL_Surface* textSurface = TTF_RenderText_Blended(g_loadingFont, loadingStr, textColor);
        if (textSurface != NULL) {
            SDL_Rect textRect = {10, 10, 0, 0};
            SDL_BlitSurface(textSurface, NULL, screen, &textRect);
            SDL_FreeSurface(textSurface);
            fprintf(stderr, "Loading: %s\n", loadingStr);
            fflush(stderr);
            SDL_Flip(screen);
            return;
        } else {
            fprintf(stderr, "SDL_ttf: Failed to render text: %s\n", TTF_GetError());
        }
    }

    fprintf(stderr, "Loading: %s\n", loadingStr);
    fflush(stderr);
}

// ===[ Vtable: init ]===
static void sdlOptInit(Renderer* renderer, DataWin* dataWin) {
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;

    TextureCache_init(&opt->textureCache);
    renderer->dataWin = dataWin;
    opt->screenFmtReady = false;

    int ttfResult = TTF_Init();
    fprintf(stderr, "SDL-OPT: TTF_Init() returned %d\n", ttfResult);
    if (ttfResult == 0) {
        const char* fontPaths[] = {
            "/mnt/fonts/arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            NULL
        };

        for (int i = 0; fontPaths[i] != NULL; i++) {
            g_loadingFont = TTF_OpenFont(fontPaths[i], 10);
            if (g_loadingFont != NULL) {
                fprintf(stderr, "SDL-OPT: Loaded font: %s\n", fontPaths[i]);
                break;
            } else {
                fprintf(stderr, "SDL-OPT: Failed to load font: %s (%s)\n", fontPaths[i], TTF_GetError());
            }
        }

        if (g_loadingFont == NULL) {
            fprintf(stderr, "SDL-OPT: No fonts found, loading screen will not show text\n");
        }
    } else {
        fprintf(stderr, "SDL-OPT: Failed to initialize SDL_ttf: %s\n", TTF_GetError());
    }

    sdl->screen = SDL_GetVideoSurface();
    if (sdl->screen) {
        drawLoadingText("Caching textures", sdl->screen);
    }

    fprintf(stderr, "SDL-OPT: Renderer initialized (texture cache capacity: %d)\n",
            TEXTURE_CACHE_CAPACITY);
}

// ===[ Vtable: destroy ]===
static void sdlOptDestroy(Renderer* renderer) {
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    TextureCache_cleanup(&opt->textureCache);
    if (opt->base.backbuffer) {
        SDL_FreeSurface(opt->base.backbuffer);
        opt->base.backbuffer = NULL;
    }

    if (g_loadingFont != NULL) {
        TTF_CloseFont(g_loadingFont);
        g_loadingFont = NULL;
    }
    TTF_Quit();

    free(opt);
}

// ===[ Vtable: beginFrame ]===
static void sdlOptBeginFrame(Renderer* renderer,
                              int32_t gameW, int32_t gameH,
                              int32_t windowW, int32_t windowH)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    (void)windowW; (void)windowH;

    sdl->gameW   = gameW;
    sdl->gameH   = gameH;
    sdl->windowW = 320;
    sdl->windowH = 240;
    sdl->screen  = SDL_GetVideoSurface();

    if (sdl->screen && !opt->screenFmtReady) {
        FastFmt_init(&opt->screenFmt, sdl->screen->format);
        opt->screenFmtReady = true;
    }

    SDL_SetClipRect(sdl->screen, NULL);
    SDL_FillRect(sdl->screen, NULL,
                 SDL_MapRGB(sdl->screen->format, 0, 0, 0));
}

// ===[ Vtable: endFrame ]===
static void sdlOptEndFrame(Renderer* renderer) {
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    SDL_SetClipRect(sdl->screen, NULL);
    SDL_Flip(sdl->screen);
}

// ===[ Vtable: beginView ]===
static void sdlOptBeginView(Renderer* renderer,
                             int32_t viewX, int32_t viewY,
                             int32_t viewW, int32_t viewH,
                             int32_t portX, int32_t portY,
                             int32_t portW, int32_t portH,
                             float viewAngle)
{
    (void)viewAngle;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;

    sdl->viewX = viewX; sdl->viewY = viewY;
    sdl->viewW = viewW; sdl->viewH = viewH;
    sdl->portX = portX; sdl->portY = portY;
    sdl->portW = portW; sdl->portH = portH;

    SDL_Rect clip = { (Sint16)portX, (Sint16)portY,
                      (Uint16)portW, (Uint16)portH };
    SDL_SetClipRect(sdl->screen, &clip);
}

// ===[ Vtable: endView ]===
static void sdlOptEndView(Renderer* renderer) {
    SDLRenderer* sdl = &((SDLRendererOpt*)renderer)->base;
    SDL_SetClipRect(sdl->screen, NULL);
}

// ===[ Helper: получить surface + tpag ]===
static SDL_Surface* getTPAGSurface(SDLRendererOpt* opt, DataWin* dw,
                                    int32_t tpagIndex, TexturePageItem** outTpag)
{
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return NULL;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    *outTpag = tpag;
    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= dw->txtr.count) return NULL;
    TextureCacheEntry* entry = TextureCache_getOrLoad(
        &opt->textureCache, &opt->base, dw, (uint32_t)pageId);
    return TextureCache_getSurface(entry, &opt->base);
}

// ===[ Vtable: drawSpritePart ]===
static void sdlOptDrawSpritePart(Renderer* renderer,
                                  int32_t tpagIndex,
                                  int32_t srcOffX, int32_t srcOffY,
                                  int32_t srcW, int32_t srcH,
                                  float x, float y,
                                  float xscale, float yscale,
                                  uint32_t color, float alpha)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    DataWin* dw = renderer->dataWin;

    TexturePageItem* tpag;
    SDL_Surface* src = getTPAGSurface(opt, dw, tpagIndex, &tpag);
    if (!src) return;

    SDL_Rect srcRect = { tpag->sourceX + srcOffX, tpag->sourceY + srcOffY, srcW, srcH };

    // draw_sprite_part не применяет origin — позиция x,y прямая
    float bx, by;
    viewToScreen(sdl, x, y, &bx, &by);

    uint8_t ga = (uint8_t)(alpha * 255.0f);

    uint8_t tintR = BGR_R(color);
    uint8_t tintG = BGR_G(color);
    uint8_t tintB = BGR_B(color);
    bool needsTint = (tintR != 255 || tintG != 255 || tintB != 255);

    int dstW = (int)scaleW(sdl, (float)srcW * xscale);
    int dstH = (int)scaleH(sdl, (float)srcH * yscale);

    if (needsTint) {
        blitScaledAlphaOptimizedTint(src, &srcRect, sdl->screen,
                                      (int)bx, (int)by, dstW, dstH, color, ga);
    } else {
        blitScaledAlphaOptimized(src, &srcRect, sdl->screen,
                                  (int)bx, (int)by, dstW, dstH, ga);
    }
}

// ===[ Vtable: drawSprite ]===
static void sdlOptDrawSprite(Renderer* renderer,
                              int32_t tpagIndex,
                              float x, float y,
                              float originX, float originY,
                              float xscale, float yscale,
                              float angleDeg,
                              uint32_t color, float alpha)
{
    (void)angleDeg;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    DataWin* dw = renderer->dataWin;

    TexturePageItem* tpag;
    SDL_Surface* src = getTPAGSurface(opt, dw, tpagIndex, &tpag);
    if (!src) return;

    SDL_Rect srcRect = { tpag->sourceX, tpag->sourceY,
                         tpag->sourceWidth, tpag->sourceHeight };

    // Учитываем targetX/Y — смещение видимой части внутри bounding box спрайта
    // Эквивалентно GL: localX0 = targetX - originX, затем T(x,y) * S(xscale, yscale)
    float bx, by;
    viewToScreen(sdl,
                 x + ((float)tpag->targetX - originX) * xscale,
                 y + ((float)tpag->targetY - originY) * yscale,
                 &bx, &by);

    uint8_t ga = (uint8_t)(alpha * 255.0f);

    uint8_t tintR = BGR_R(color);
    uint8_t tintG = BGR_G(color);
    uint8_t tintB = BGR_B(color);
    bool needsTint = (tintR != 255 || tintG != 255 || tintB != 255);

    int dstW = (int)scaleW(sdl, (float)tpag->sourceWidth  * xscale);
    int dstH = (int)scaleH(sdl, (float)tpag->sourceHeight * yscale);

    if (needsTint) {
        blitScaledAlphaOptimizedTint(src, &srcRect, sdl->screen,
                                      (int)bx, (int)by, dstW, dstH, color, ga);
    } else {
        blitScaledAlphaOptimized(src, &srcRect, sdl->screen,
                                  (int)bx, (int)by, dstW, dstH, ga);
    }
}

// ===[ Vtable: drawRectangle ]===
static void sdlOptDrawRectangle(Renderer* renderer,
                                 float x1, float y1, float x2, float y2,
                                 uint32_t color, float alpha, bool outline)
{
    (void)alpha;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;

    float bx1, by1, bx2, by2;
    viewToScreen(sdl, x1, y1, &bx1, &by1);
    viewToScreen(sdl, x2, y2, &bx2, &by2);

    int X1 = (int)floorf(fminf(bx1, bx2));
    int Y1 = (int)floorf(fminf(by1, by2));
    int X2 = (int)ceilf(fmaxf(bx1, bx2));
    int Y2 = (int)ceilf(fmaxf(by1, by2));
    if (X2 <= X1) X2 = X1 + 1;
    if (Y2 <= Y1) Y2 = Y1 + 1;

    uint32_t c = SDL_MapRGB(sdl->screen->format,
                            BGR_B(color), BGR_G(color), BGR_R(color));
    if (outline) {
        int w = X2 - X1, h = Y2 - Y1;
        SDL_Rect edges[4] = {
            {X1,     Y1,     w, 1},
            {X1,     Y2 - 1, w, 1},
            {X1,     Y1,     1, h},
            {X2 - 1, Y1,     1, h}
        };
        for (int i = 0; i < 4; i++) SDL_FillRect(sdl->screen, &edges[i], c);
    } else {
        SDL_Rect rect = {X1, Y1, X2 - X1, Y2 - Y1};
        SDL_FillRect(sdl->screen, &rect, c);
    }
}

// ===[ Vtable: drawLine ]===
static void sdlOptDrawLine(Renderer* renderer,
                            float x1, float y1, float x2, float y2,
                            float width, uint32_t color, float alpha)
{
    (void)width;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    float bx1, by1, bx2, by2;
    viewToScreen(sdl, x1, y1, &bx1, &by1);
    viewToScreen(sdl, x2, y2, &bx2, &by2);
    sdlOptDrawRectangle(renderer, bx1, by1, bx2, by2, color, alpha, false);
}

// ===[ Vtable: drawLineColor ]===
static void sdlOptDrawLineColor(Renderer* renderer,
                                 float x1, float y1, float x2, float y2,
                                 float width, uint32_t color1, uint32_t color2, float alpha)
{
    (void)color2; (void)width;
    sdlOptDrawLine(renderer, x1, y1, x2, y2, width, color1, alpha);
}

// ===[ Vtable: drawTriangle ]===
static void sdlOptDrawTriangle(Renderer* renderer,
                                float x1, float y1, float x2, float y2,
                                float x3, float y3, bool outline)
{
    (void)outline;
    sdlOptDrawLine(renderer, x1, y1, x2, y2, 1, 0xFFFFFF, 1.0f);
    sdlOptDrawLine(renderer, x2, y2, x3, y3, 1, 0xFFFFFF, 1.0f);
    sdlOptDrawLine(renderer, x3, y3, x1, y1, 1, 0xFFFFFF, 1.0f);
}

// ===[ Vtable: drawText ]===
static void sdlOptDrawText(Renderer* renderer,
                            const char* text,
                            float x, float y,
                            float xscale, float yscale,
                            float angleDeg)
{
    (void)angleDeg;
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
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
    if (pageId < 0 || (uint32_t)pageId >= dw->txtr.count) return;

    TextureCacheEntry* entry = TextureCache_getOrLoad(
        &opt->textureCache, sdl, dw, (uint32_t)pageId);
    SDL_Surface* src = TextureCache_getSurface(entry, sdl);
    if (!src) return;

    int32_t textLen   = (int32_t)strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float totalHeight = (float)lineCount * (float)font->emSize;
    float valignOffset = 0;
    if      (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY   = valignOffset;
    int32_t lineStart = 0;
    uint32_t tintColor = renderer->drawColor;

    const float sx = xscale * font->scaleX;
    const float sy = yscale * font->scaleY;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if      (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
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

            float gx = roundf(x + (cursorX + (float)glyph->offset) * sx);
            float gy = roundf(y + cursorY * sy);

            float bx, by;
            viewToScreen(sdl, gx, gy, &bx, &by);

            SDL_Rect srcRect = {
                (Sint16)(baseTpag->sourceX + glyph->sourceX),
                (Sint16)(baseTpag->sourceY + glyph->sourceY),
                (Uint16)glyph->sourceWidth,
                (Uint16)glyph->sourceHeight
            };

            // Текст всегда рисуется с полной непрозрачностью (255)
            blitScaledAlphaOptimizedTint(src, &srcRect, sdl->screen,
                (int)bx, (int)by,
                (int)scaleW(sdl, (float)glyph->sourceWidth  * sx),
                (int)scaleH(sdl, (float)glyph->sourceHeight * sy),
                tintColor,
                255);

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
            break;
    }
}

// ===[ Vtable: drawTextColor ]===
static void sdlOptDrawTextColor(Renderer* renderer,
                                 const char* text, float x, float y,
                                 float xscale, float yscale, float angleDeg,
                                 int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha)
{
    (void)c2; (void)c3; (void)c4; (void)alpha;
    uint32_t saved = renderer->drawColor;
    renderer->drawColor = (uint32_t)c1;
    sdlOptDrawText(renderer, text, x, y, xscale, yscale, angleDeg);
    renderer->drawColor = saved;
}

// ===[ Vtable: flush ]===
static void sdlOptFlush(Renderer* renderer) { (void)renderer; }

// ===[ Vtable: createSpriteFromSurface ]===
static int32_t sdlOptCreateSpriteFromSurface(Renderer* r,
    int32_t x, int32_t y, int32_t w, int32_t h,
    bool removeback, bool smooth, int32_t xorig, int32_t yorig)
{
    (void)r;(void)x;(void)y;(void)w;(void)h;
    (void)removeback;(void)smooth;(void)xorig;(void)yorig;
    return -1;
}

// ===[ Vtable: deleteSprite ]===
static void sdlOptDeleteSprite(Renderer* r, int32_t idx) { (void)r; (void)idx; }

// ===[ Vtable: drawTile ]===
static void sdlOptDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY)
{
    SDLRendererOpt* opt = (SDLRendererOpt*)renderer;
    SDLRenderer* sdl = &opt->base;
    DataWin* dw = renderer->dataWin;

    int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(dw, tile);
    if (tpagIndex < 0) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= dw->txtr.count) return;

    int32_t srcX = tile->sourceX, srcY = tile->sourceY;
    int32_t srcW = (int32_t)tile->width, srcH = (int32_t)tile->height;
    float drawX  = (float)tile->x + offsetX;
    float drawY  = (float)tile->y + offsetY;

    int32_t cL = tpag->targetX, cT = tpag->targetY;
    if (cL > srcX) { drawX += (float)(cL - srcX) * tile->scaleX; srcW -= cL - srcX; srcX = cL; }
    if (cT > srcY) { drawY += (float)(cT - srcY) * tile->scaleY; srcH -= cT - srcY; srcY = cT; }
    int32_t cR = tpag->targetX + tpag->sourceWidth;
    int32_t cB = tpag->targetY + tpag->sourceHeight;
    if (srcX + srcW > cR) srcW = cR - srcX;
    if (srcY + srcH > cB) srcH = cB - srcY;
    if (srcW <= 0 || srcH <= 0) return;

    int32_t atlasOffX = srcX - tpag->targetX;
    int32_t atlasOffY = srcY - tpag->targetY;

    TextureCacheEntry* entry = TextureCache_getOrLoad(
        &opt->textureCache, sdl, dw, (uint32_t)pageId);
    SDL_Surface* src = TextureCache_getSurface(entry, sdl);
    if (!src) return;

    SDL_Rect srcRect = {
        tpag->sourceX + atlasOffX,
        tpag->sourceY + atlasOffY,
        srcW, srcH
    };

    float bx, by;
    viewToScreen(sdl, drawX, drawY, &bx, &by);

    blitScaledThreshold(src, &srcRect, sdl->screen,
                        (int)bx, (int)by,
                        (int)scaleW(sdl, (float)srcW * tile->scaleX),
                        (int)scaleH(sdl, (float)srcH * tile->scaleY));
}

// ===[ Vtable ]===
#ifdef __cplusplus
static RendererVtable sdlOptVtable = {
    sdlOptInit, sdlOptDestroy,
    sdlOptBeginFrame, sdlOptEndFrame,
    sdlOptBeginView, sdlOptEndView,
    sdlOptDrawSprite, sdlOptDrawSpritePart,
    sdlOptDrawRectangle, sdlOptDrawLine, sdlOptDrawLineColor,
    sdlOptDrawTriangle, sdlOptDrawText, sdlOptDrawTextColor,
    sdlOptFlush, sdlOptCreateSpriteFromSurface,
    sdlOptDeleteSprite, sdlOptDrawTile,
};
#else
static RendererVtable sdlOptVtable = {
    .init                    = sdlOptInit,
    .destroy                 = sdlOptDestroy,
    .beginFrame              = sdlOptBeginFrame,
    .endFrame                = sdlOptEndFrame,
    .beginView               = sdlOptBeginView,
    .endView                 = sdlOptEndView,
    .drawSprite              = sdlOptDrawSprite,
    .drawSpritePart          = sdlOptDrawSpritePart,
    .drawRectangle           = sdlOptDrawRectangle,
    .drawLine                = sdlOptDrawLine,
    .drawLineColor           = sdlOptDrawLineColor,
    .drawTriangle            = sdlOptDrawTriangle,
    .drawText                = sdlOptDrawText,
    .drawTextColor           = sdlOptDrawTextColor,
    .flush                   = sdlOptFlush,
    .createSpriteFromSurface = sdlOptCreateSpriteFromSurface,
    .deleteSprite            = sdlOptDeleteSprite,
    .drawTile                = sdlOptDrawTile,
};
#endif

// ===[ Public constructor ]===
Renderer* SDLRendererOpt_create(void)
{
    SDLRendererOpt* opt = safeCalloc(1, sizeof(SDLRendererOpt));
    opt->base.base.vtable     = &sdlOptVtable;
    opt->base.base.drawColor  = 0xFFFFFF;
    opt->base.base.drawAlpha  = 1.0f;
    opt->base.base.drawFont   = -1;
    opt->base.base.drawHalign = 0;
    opt->base.base.drawValign = 0;
    return (Renderer*)opt;
}
