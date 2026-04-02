#pragma once

#include "renderer.h"
#include <SDL/SDL.h>
#include <stdint.h>

typedef struct {
    Renderer base; // Must be first

    SDL_Surface* screen;      // 320x240 window surface (never freed manually)
    SDL_Surface* backbuffer;  // 320x240 fixed render target

    SDL_Surface** textures;   // one surface per TXTR page (RGBA), loaded on-demand
    uint32_t textureCount;

    // Current view/port state (set in beginView)
    int32_t viewX, viewY, viewW, viewH;
    int32_t portX, portY, portW, portH;
    
    // Scaled port state for consistent coordinate transformation
    int32_t scaledPortX, scaledPortY, scaledPortW, scaledPortH;

    // Game resolution (set in beginFrame) - used for coordinate scaling
    int32_t gameW, gameH;
    // Window resolution (always 320x240)
    int32_t windowW, windowH;
} SDLRenderer;

Renderer* SDLRenderer_create(void);

// Memory-optimized renderer with LRU texture cache
Renderer* SDLRendererOpt_create(void);
