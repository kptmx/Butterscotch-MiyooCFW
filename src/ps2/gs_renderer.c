#include "gs_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <kernel.h>

#include "binary_reader.h"
#include "binary_utils.h"
#include "utils.h"
#include "text_utils.h"
#include "ps2_utils.h"

// ===[ Constants ]===
#define ATLAS_WIDTH 512
#define ATLAS_HEIGHT 512
#define PS2_SCREEN_WIDTH 640.0f
#define PS2_SCREEN_HEIGHT 448.0f
#define TEX_HEADER_SIZE 128
#define CLUT4_ENTRY_SIZE 64    // 16 colors * 4 bytes
#define CLUT8_ENTRY_SIZE 1024  // 256 colors * 4 bytes

// ===[ File Loading Helper ]===

// Loads an entire file from host into a memalign'd buffer. Returns size via outSize.
// Aborts on failure.
static uint8_t* loadFileRaw(const char* path, uint32_t* outSize) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open %s\n", path);
        abort();
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (0 >= size) {
        fprintf(stderr, "GsRenderer: File %s is empty or unreadable (size=%ld)\n", path, size);
        fclose(f);
        abort();
    }

    // 128-byte aligned for DMA transfers
    uint8_t* data = (uint8_t*) memalign(128, (size_t) size);
    if (data == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to allocate %ld bytes for %s\n", size, path);
        fclose(f);
        abort();
    }

    size_t read = fread(data, 1, (size_t) size, f);
    fclose(f);

    if (read != (size_t) size) {
        fprintf(stderr, "GsRenderer: Short read on %s (expected %ld, got %zu)\n", path, size, read);
        abort();
    }

    *outSize = (uint32_t) size;
    return data;
}

// ===[ Atlas Loading ]===
static void loadAtlas(GsRenderer* gs) {
    FILE* f = fopen("host:ATLAS.BIN", "rb");
    if (f == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to open host:ATLAS.BIN\n");
        abort();
    }

    fseek(f, 0, SEEK_END);
    size_t fileSize = (size_t) ftell(f);
    fseek(f, 0, SEEK_SET);

    BinaryReader reader = BinaryReader_create(f, fileSize);

    uint8_t version = BinaryReader_readUint8(&reader);
    if (version != 0) {
        fprintf(stderr, "GsRenderer: Unsupported ATLAS.BIN version %u\n", version);
        abort();
    }

    gs->atlasTPAGCount = BinaryReader_readUint16(&reader);
    gs->atlasTileCount = BinaryReader_readUint16(&reader);

    // Parse TPAG entries
    gs->atlasTPAGEntries = safeMalloc(gs->atlasTPAGCount * sizeof(AtlasTPAGEntry));

    repeat(gs->atlasTPAGCount, i) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[i];
        entry->atlasId = BinaryReader_readUint16(&reader);
        entry->atlasX = BinaryReader_readUint16(&reader);
        entry->atlasY = BinaryReader_readUint16(&reader);
        entry->width = BinaryReader_readUint16(&reader);
        entry->height = BinaryReader_readUint16(&reader);
        entry->clutIndex = BinaryReader_readUint16(&reader);
        entry->bpp = BinaryReader_readUint8(&reader);
    }

    // Tile entries skipped for now (will be added later)

    fclose(f);

    // Determine atlas count (find max atlasId) and build bpp table
    uint16_t maxAtlasId = 0;
    repeat(gs->atlasTPAGCount, i) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[i];
        if (entry->atlasId != 0xFFFF && entry->atlasId > maxAtlasId) {
            maxAtlasId = entry->atlasId;
        }
    }

    gs->atlasCount = maxAtlasId + 1;
    gs->atlasBpp = safeCalloc(gs->atlasCount, sizeof(uint8_t));
    gs->atlasToChunk = safeMalloc(gs->atlasCount * sizeof(int16_t));
    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    // Build bpp table from TPAG entries
    repeat(gs->atlasTPAGCount, i) {
        AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[i];
        if (entry->atlasId != 0xFFFF && gs->atlasCount > entry->atlasId) {
            gs->atlasBpp[entry->atlasId] = entry->bpp;
        }
    }

    fprintf(stderr, "GsRenderer: ATLAS.BIN loaded - %u TPAG entries, %u tile entries, %u atlases\n", gs->atlasTPAGCount, gs->atlasTileCount, gs->atlasCount);
}

// ===[ CLUT Loading and VRAM Upload ]===
// Each CLUT is uploaded individually to its own VRAM address. This is necessary because
// the PS2 GS VRAM has a block-swizzled layout - bulk-uploading stacked CLUTs and computing
// linear offsets for CBP does NOT work (the BITBLT write path and CLUT read path use
// block-based addressing, so CLUTs don't land at simple linear offsets within a bulk upload).
static void loadAndUploadCLUTs(GsRenderer* gs) {
    GSGLOBAL* gsGlobal = gs->gsGlobal;

    // 128-byte aligned temp buffer for DMA transfers (reused for each CLUT send)
    // Large enough for one 8bpp CLUT (1024 bytes)
    uint8_t* tempBuf = (uint8_t*) memalign(128, CLUT8_ENTRY_SIZE);
    if (tempBuf == nullptr) {
        fprintf(stderr, "GsRenderer: Failed to allocate CLUT temp buffer\n");
        abort();
    }

    // Load and upload CLUT4 (4bpp palettes: 16 colors * 4 bytes = 64 bytes each)
    {
        uint32_t clut4FileSize;
        uint8_t* clut4Data = loadFileRaw("host:CLUT4.BIN", &clut4FileSize);
        gs->clut4Count = clut4FileSize / CLUT4_ENTRY_SIZE;
        fprintf(stderr, "GsRenderer: CLUT4.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut4Count, clut4FileSize);

        gs->clut4VramAddrs = safeMalloc(gs->clut4Count * sizeof(uint32_t));

        repeat(gs->clut4Count, i) {
            // gsKit uploads 4bpp CLUTs as 8x2 CT32 (16 entries in 8-wide, 2-tall grid)
            uint32_t vramSize = gsKit_texture_size(8, 2, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                fprintf(stderr, "GsRenderer: Failed to allocate VRAM for CLUT4 index %u\n", i);
                abort();
            }

            // Copy to aligned temp buffer for DMA
            memcpy(tempBuf, clut4Data + i * CLUT4_ENTRY_SIZE, CLUT4_ENTRY_SIZE);
            gsKit_texture_send((u32*) tempBuf, 8, 2, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut4VramAddrs[i] = vramAddr;
        }

        fprintf(stderr, "GsRenderer: CLUT4 uploaded (%u CLUTs)\n", gs->clut4Count);
        free(clut4Data);
    }

    // Load and upload CLUT8 (8bpp palettes: 256 colors * 4 bytes = 1024 bytes each)
    {
        uint32_t clut8FileSize;
        uint8_t* clut8Data = loadFileRaw("host:CLUT8.BIN", &clut8FileSize);
        gs->clut8Count = clut8FileSize / CLUT8_ENTRY_SIZE;
        fprintf(stderr, "GsRenderer: CLUT8.BIN loaded - %u CLUTs (%u bytes)\n", gs->clut8Count, clut8FileSize);

        gs->clut8VramAddrs = safeMalloc(gs->clut8Count * sizeof(uint32_t));

        repeat(gs->clut8Count, i) {
            // gsKit uploads 8bpp CLUTs as 16x16 CT32 (256 entries in 16-wide, 16-tall grid)
            uint32_t vramSize = gsKit_texture_size(16, 16, GS_PSM_CT32);
            uint32_t vramAddr = gsKit_vram_alloc(gsGlobal, vramSize, GSKIT_ALLOC_USERBUFFER);
            if (vramAddr == GSKIT_ALLOC_ERROR) {
                fprintf(stderr, "GsRenderer: Failed to allocate VRAM for CLUT8 index %u\n", i);
                abort();
            }

            // 8bpp CLUTs are 1024 bytes; source is 128-byte aligned (1024 is a multiple of 128)
            gsKit_texture_send((u32*) (clut8Data + i * CLUT8_ENTRY_SIZE), 16, 16, vramAddr, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
            gs->clut8VramAddrs[i] = vramAddr;
        }

        fprintf(stderr, "GsRenderer: CLUT8 uploaded (%u CLUTs)\n", gs->clut8Count);
        free(clut8Data);
    }

    free(tempBuf);

    fprintf(stderr, "GsRenderer: VRAM after CLUTs: 0x%08X / 0x%08X\n", gsGlobal->CurrentPointer, GS_VRAM_SIZE);
}

// ===[ VRAM Texture Cache (Buddy System with LRU Eviction) ]===
// Manages a pool of 128KB VRAM chunks for atlas textures.
// 4bpp atlases use 1 chunk, 8bpp atlases use 2 consecutive chunks.

#define FONTM_RESERVED_VRAM 65536 // 64KB reserved for gsKit's TexManager (FONTM debug overlay)

// Initializes the chunk pool from the remaining VRAM after CLUTs.
// Reserves 64KB at the end for gsKit's TexManager (used by FONTM).
// Our chunk pool occupies the middle, between CLUTs and the FONTM region.
//
// VRAM layout: [Framebuffers] [CLUTs] [Chunk Pool ...] [64KB FONTM]
static void initTextureCache(GsRenderer* gs) {
    gs->textureVramBase = gs->gsGlobal->CurrentPointer;
    uint32_t availableVram = GS_VRAM_SIZE - gs->textureVramBase - FONTM_RESERVED_VRAM;
    gs->chunkCount = availableVram / VRAM_CHUNK_SIZE;

    gs->chunks = safeMalloc(gs->chunkCount * sizeof(VRAMChunk));
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }

    gs->frameCounter = 1;

    // Advance CurrentPointer past our chunk pool so the TexManager only
    // manages the 64KB FONTM region at the end of VRAM.
    gs->gsGlobal->CurrentPointer = gs->textureVramBase + gs->chunkCount * VRAM_CHUNK_SIZE;
    gsKit_TexManager_init(gs->gsGlobal);

    uint32_t fontmVram = GS_VRAM_SIZE - gs->gsGlobal->CurrentPointer;
    fprintf(stderr, "GsRenderer: Texture cache initialized - %u chunks (%u KB each), base 0x%08X, %u KB for textures, %u KB for FONTM\n", gs->chunkCount, VRAM_CHUNK_SIZE / 1024, gs->textureVramBase, gs->chunkCount * (VRAM_CHUNK_SIZE / 1024), fontmVram / 1024);
}

// Find the first run of consecutive free chunks.
// Returns the index of the first chunk, or -1 if not found.
static int32_t findConsecutiveFreeChunks(GsRenderer* gs, int chunksNeeded) {
    int consecutive = 0;
    forEachIndexed(VRAMChunk, chunk, i, gs->chunks, gs->chunkCount) {
        if (0 > chunk->atlasId) {
            consecutive++;
            if (consecutive >= chunksNeeded) {
                return (int32_t) (i - (uint32_t) chunksNeeded + 1);
            }
        } else {
            consecutive = 0;
        }
    }
    return -1;
}

// Count total free chunks.
static uint32_t countFreeChunks(GsRenderer* gs) {
    uint32_t count = 0;
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (0 > chunk->atlasId)
            count++;
    }
    return count;
}

// Find the atlas with the oldest lastUsed time (LRU victim).
// Returns the atlasId, or -1 if no loaded atlases.
static int16_t findLRUVictim(GsRenderer* gs) {
    uint64_t oldest = UINT64_MAX;
    int16_t victimAtlas = -1;
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunk->atlasId >= 0 && oldest > chunk->lastUsed) {
            oldest = chunk->lastUsed;
            victimAtlas = chunk->atlasId;
        }
    }
    return victimAtlas;
}

// Evict an atlas from the cache, freeing its chunk(s).
static void evictAtlas(GsRenderer* gs, int16_t atlasId) {
    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        if (chunk->atlasId == atlasId) {
            chunk->atlasId = -1;
            chunk->lastUsed = 0;
        }
    }

    if (atlasId >= 0 && gs->atlasCount > (uint16_t) atlasId) {
        gs->atlasToChunk[atlasId] = -1;
    }

    uint32_t availableChunks = countFreeChunks(gs);
    fprintf(stderr, "GsRenderer: Evicted atlas %d from VRAM (available chunks = %d)\n", atlasId, availableChunks);
}

// Defragment the texture cache by evicting all loaded atlases.
// They will be reloaded on-demand as needed during subsequent draw calls.
static void defragTextureCache(GsRenderer* gs) {
    fprintf(stderr, "GsRenderer: Defragmenting VRAM texture cache...\n");

    forEach(VRAMChunk, chunk, gs->chunks, gs->chunkCount) {
        chunk->atlasId = -1;
        chunk->lastUsed = 0;
    }

    repeat(gs->atlasCount, i) {
        gs->atlasToChunk[i] = -1;
    }

    fprintf(stderr, "GsRenderer: Defrag complete - all %u chunks freed\n", gs->chunkCount);
}

// Allocate consecutive chunks for an atlas. Evicts LRU victims or defrags if needed.
// Returns the first chunk index, or -1 if VRAM is truly exhausted.
static int32_t allocateChunks(GsRenderer* gs, int chunksNeeded) {
    // Attempt 1: find free consecutive chunks
    int32_t idx = findConsecutiveFreeChunks(gs, chunksNeeded);
    if (idx >= 0) return idx;

    // Attempt 2: evict LRU victims one at a time until space is found
    repeat(gs->chunkCount, attempts) {
        int16_t victim = findLRUVictim(gs);
        if (0 > victim)
            break;

        evictAtlas(gs, victim);

        idx = findConsecutiveFreeChunks(gs, chunksNeeded);

        if (idx >= 0)
            return idx;
    }

    // Attempt 3: defrag - evict ALL and let them reload on demand
    // Handles fragmentation where enough free chunks exist but aren't consecutive
    if (countFreeChunks(gs) >= (uint32_t) chunksNeeded) {
        defragTextureCache(gs);
        idx = findConsecutiveFreeChunks(gs, chunksNeeded);

        if (idx >= 0)
            return idx;
    }

    // VRAM truly exhausted
    return -1;
}

// Upload atlas pixel data from TEX file to the given VRAM chunk(s).
static void uploadAtlasToChunk(GsRenderer* gs, uint16_t atlasId, int32_t firstChunk) {
    char path[64];
    snprintf(path, sizeof(path), "host:TEX%u.BIN", atlasId);

    uint32_t fileSize;
    uint8_t* data = loadFileRaw(path, &fileSize);

    if (TEX_HEADER_SIZE > fileSize) {
        fprintf(stderr, "GsRenderer: %s too small for header (%u bytes)\n", path, fileSize);
        abort();
    }

    uint8_t version = data[0];
    if (version != 0) {
        fprintf(stderr, "GsRenderer: Unsupported TEX version %u in %s\n", version, path);
        abort();
    }

    uint16_t width = BinaryUtils_readUint16(data + 1);
    uint16_t height = BinaryUtils_readUint16(data + 3);
    uint8_t bpp = BinaryUtils_readUint8(data + 5);
    uint32_t pixelDataSize = BinaryUtils_readUint32(data + 6);

    if (width != ATLAS_WIDTH || height != ATLAS_HEIGHT) {
        fprintf(stderr, "GsRenderer: %s unexpected dimensions %ux%u (expected %ux%u)\n", path, width, height, ATLAS_WIDTH, ATLAS_HEIGHT);
        abort();
    }

    if (bpp != 4 && bpp != 8) {
        fprintf(stderr, "GsRenderer: %s unsupported bpp %u\n", path, bpp);
        abort();
    }

    uint32_t expectedFileSize = TEX_HEADER_SIZE + pixelDataSize;
    if (expectedFileSize > fileSize) {
        fprintf(stderr, "GsRenderer: %s truncated (expected %u, got %u)\n", path, expectedFileSize, fileSize);
        abort();
    }

    // Upload pixel data to VRAM at the chunk's address
    uint8_t psm = (bpp == 4) ? GS_PSM_T4 : GS_PSM_T8;
    uint32_t tbw = ATLAS_WIDTH / 64;
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) firstChunk * VRAM_CHUNK_SIZE;
    uint8_t* pixelData = data + TEX_HEADER_SIZE;

    gsKit_texture_send((u32*) pixelData, ATLAS_WIDTH, ATLAS_HEIGHT, vramAddr, psm, tbw, GS_CLUT_TEXTURE);

    // Update chunk state
    int chunksUsed = (bpp == 8) ? 2 : 1;
    for (int i = 0; chunksUsed > i; i++) {
        gs->chunks[firstChunk + i].atlasId = (int16_t) atlasId;
        gs->chunks[firstChunk + i].lastUsed = gs->frameCounter;
    }
    gs->atlasToChunk[atlasId] = (int16_t) firstChunk;

    fprintf(stderr, "GsRenderer: TEX%u uploaded to chunk %d (VRAM 0x%08X, %ubpp)\n", atlasId, firstChunk, vramAddr, bpp);

    free(data);
}

// Ensure an atlas is loaded into VRAM, using LRU eviction if needed.
// Returns true on success, false on failure.
static bool ensureAtlasLoaded(GsRenderer* gs, uint16_t atlasId) {
    if (atlasId >= gs->atlasCount) {
        fprintf(stderr, "GsRenderer: Atlas ID %u out of range (max %u)\n", atlasId, gs->atlasCount - 1);
        return false;
    }

    // Already loaded? Just touch LRU timestamp
    if (gs->atlasToChunk[atlasId] >= 0) {
        int16_t firstChunk = gs->atlasToChunk[atlasId];
        uint8_t bpp = gs->atlasBpp[atlasId];
        int chunksUsed = (bpp == 8) ? 2 : 1;
        repeat(chunksUsed, i) {
            gs->chunks[firstChunk + i].lastUsed = gs->frameCounter;
        }
        return true;
    }

    // Determine how many chunks we need
    uint8_t bpp = gs->atlasBpp[atlasId];
    if (bpp != 4 && bpp != 8) {
        fprintf(stderr, "GsRenderer: Atlas %u has unknown bpp %u\n", atlasId, bpp);
        return false;
    }
    int chunksNeeded = (bpp == 8) ? 2 : 1;

    // Allocate chunks (may evict or defrag)
    int32_t chunkIdx = allocateChunks(gs, chunksNeeded);
    if (0 > chunkIdx) {
        fprintf(stderr, "GsRenderer: VRAM exhausted! Cannot allocate %d chunk(s) for atlas %u (%ubpp)\n", chunksNeeded, atlasId, bpp);
        abort();
    }

    // Load TEX file and upload to the allocated chunk(s)
    uploadAtlasToChunk(gs, atlasId, chunkIdx);
    return true;
}

// ===[ GSTEXTURE setup for a given TPAG entry ]===
// Configures a GSTEXTURE struct for rendering a specific atlas region.
// The GSTEXTURE points to the atlas's VRAM location and the appropriate CLUT.
static bool setupTextureForTPAG(GsRenderer* gs, GSTEXTURE* tex, int32_t tpagIndex) {
    if (0 > tpagIndex || (uint32_t) tpagIndex >= gs->atlasTPAGCount) return false;

    AtlasTPAGEntry* entry = &gs->atlasTPAGEntries[tpagIndex];
    if (entry->atlasId == 0xFFFF) return false;

    // Ensure the atlas texture is loaded into VRAM (may trigger LRU eviction)
    if (!ensureAtlasLoaded(gs, entry->atlasId))
        return false;

    // Compute VRAM address from chunk index
    int16_t chunkIdx = gs->atlasToChunk[entry->atlasId];
    uint32_t vramAddr = gs->textureVramBase + (uint32_t) chunkIdx * VRAM_CHUNK_SIZE;

    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = ATLAS_WIDTH;
    tex->Height = ATLAS_HEIGHT;
    tex->TBW = ATLAS_WIDTH / 64;
    tex->Vram = vramAddr;
    tex->Filter = GS_FILTER_NEAREST;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    if (entry->bpp == 4) {
        tex->PSM = GS_PSM_T4;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut4Count) {
            fprintf(stderr, "GsRenderer: CLUT4 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut4Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut4VramAddrs[entry->clutIndex];
    } else {
        tex->PSM = GS_PSM_T8;
        tex->ClutPSM = GS_PSM_CT32;

        if (entry->clutIndex >= gs->clut8Count) {
            fprintf(stderr, "GsRenderer: CLUT8 index %u out of range (max %u) for TPAG %d\n", entry->clutIndex, gs->clut8Count - 1, tpagIndex);
            abort();
        }

        tex->VramClut = gs->clut8VramAddrs[entry->clutIndex];
    }

    return true;
}

// ===[ Vtable Implementations ]===

static void gsInit(Renderer* renderer, DataWin* dataWin) {
    GsRenderer* gs = (GsRenderer*) renderer;

    renderer->dataWin = dataWin;
    renderer->drawColor = 0xFFFFFF;
    renderer->drawAlpha = 1.0f;
    renderer->drawFont = -1;
    renderer->drawHalign = 0;
    renderer->drawValign = 0;

    // Enable alpha blending
    gs->gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    // Alpha blend: (Cs - Cd) * As / 128 + Cd (standard source-over)
    gsKit_set_primalpha(gs->gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    // Load atlas metadata
    loadAtlas(gs);

    // Upload CLUTs to VRAM
    loadAndUploadCLUTs(gs);

    // Initialize the texture cache chunk pool (uses remaining VRAM after CLUTs)
    initTextureCache(gs);

    fprintf(stderr, "GsRenderer: Initialized (textured mode)\n");
}

static void gsDestroy(Renderer* renderer) {
    GsRenderer* gs = (GsRenderer*) renderer;
    free(gs->atlasTPAGEntries);
    free(gs->chunks);
    free(gs->atlasToChunk);
    free(gs->atlasBpp);
    free(gs->clut4VramAddrs);
    free(gs->clut8VramAddrs);
    free(gs);
}

static void gsBeginFrame(Renderer* renderer, [[maybe_unused]] int32_t gameW, [[maybe_unused]] int32_t gameH, [[maybe_unused]] int32_t windowW, [[maybe_unused]] int32_t windowH) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->zCounter = 1;
    gs->frameCounter++;
}

static void gsEndFrame([[maybe_unused]] Renderer* renderer) {
    // No-op: flip happens in main loop
}

static void gsBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, [[maybe_unused]] int32_t portX, [[maybe_unused]] int32_t portY, [[maybe_unused]] int32_t portW, [[maybe_unused]] int32_t portH, [[maybe_unused]] float viewAngle) {
    GsRenderer* gs = (GsRenderer*) renderer;
    gs->viewX = viewX;
    gs->viewY = viewY;

    // Scale game view to PS2 screen (640x448 NTSC interlaced)
    if (viewW > 0 && viewH > 0) {
        gs->scaleX = 640.0f / (float) viewW;
        gs->scaleY = gs->scaleX;
    } else {
        gs->scaleX = 2.0f;
        gs->scaleY = 2.0f;
    }

    // Center vertically
    float renderedH = (float) viewH * gs->scaleY;
    gs->offsetX = 0.0f;
    gs->offsetY = (448.0f - renderedH) / 2.0f;
}

static void gsEndView([[maybe_unused]] Renderer* renderer) {
    // No-op
}

static void gsDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, [[maybe_unused]] float angleDeg, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float boundW = (float) tpag->boundingWidth;
    float boundH = (float) tpag->boundingHeight;

    // Compute screen rect in game coordinates
    float gameX1 = x - originX * xscale;
    float gameY1 = y - originY * yscale;
    float gameX2 = x + (boundW - originX) * xscale;
    float gameY2 = y + (boundH - originY) * yscale;

    // Apply view offset and scale
    float sx1 = (gameX1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (gameY1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (gameX2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (gameY2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    // View frustum culling: skip if entirely off-screen (handles negative scales via min/max)
    float minSX = (sx1 < sx2) ? sx1 : sx2;
    float maxSX = (sx1 > sx2) ? sx1 : sx2;
    float minSY = (sy1 < sy2) ? sy1 : sy2;
    float maxSY = (sy1 > sy2) ? sy1 : sy2;
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT)
        return;

    // Set up GSTEXTURE for this TPAG entry
    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) {
        // Fallback: draw colored rectangle if no atlas mapping
        uint8_t r = BGR_R(color);
        uint8_t g = BGR_G(color);
        uint8_t b = BGR_B(color);
        uint8_t a = (uint8_t) (alpha * 128.0f);
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, GS_SETREG_RGBAQ(r, g, b, a, 0x00));
        gs->zCounter++;
        return;
    }

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];

    // The atlas entry has the actual sprite dimensions in the atlas
    // The TPAG has the original bounding dimensions
    // If downscaled, the GS hardware rescales because we draw boundW x boundH but
    // sample from atlasW x atlasH texels

    // UV coords within the 512x512 atlas (in texels for gsKit)
    float u1 = (float) atlasEntry->atlasX;
    float v1 = (float) atlasEntry->atlasY;
    float u2 = u1 + (float) atlasEntry->width;
    float v2 = v1 + (float) atlasEntry->height;

    // GS modulate mode: Output = Texture * Vertex / 128
    // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 128 (1.0x multiplier)
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = (uint8_t) (alpha * 128.0f);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, gs->zCounter, gsColor);
    gs->zCounter++;
}

static void gsDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    if (0 > tpagIndex || (uint32_t) tpagIndex >= renderer->dataWin->tpag.count) return;

    // Compute screen position
    float gameX1 = x - (float) gs->viewX;
    float gameY1 = y - (float) gs->viewY;
    float gameX2 = gameX1 + (float) srcW * xscale;
    float gameY2 = gameY1 + (float) srcH * yscale;

    float sx1 = gameX1 * gs->scaleX + gs->offsetX;
    float sy1 = gameY1 * gs->scaleY + gs->offsetY;
    float sx2 = gameX2 * gs->scaleX + gs->offsetX;
    float sy2 = gameY2 * gs->scaleY + gs->offsetY;

    // View frustum culling: skip if entirely off-screen (handles negative scales via min/max)
    float minSX = (sx1 < sx2) ? sx1 : sx2;
    float maxSX = (sx1 > sx2) ? sx1 : sx2;
    float minSY = (sy1 < sy2) ? sy1 : sy2;
    float maxSY = (sy1 > sy2) ? sy1 : sy2;
    if (maxSX < 0.0f || minSX > PS2_SCREEN_WIDTH || maxSY < 0.0f || minSY > PS2_SCREEN_HEIGHT) return;

    // Set up GSTEXTURE for this TPAG entry
    GSTEXTURE tex;
    if (!setupTextureForTPAG(gs, &tex, tpagIndex)) {
        // Fallback: draw colored rectangle
        uint8_t r = BGR_R(color);
        uint8_t g = BGR_G(color);
        uint8_t b = BGR_B(color);
        uint8_t a = (uint8_t) (alpha * 128.0f);
        gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, GS_SETREG_RGBAQ(r, g, b, a, 0x00));
        gs->zCounter++;
        return;
    }

    AtlasTPAGEntry* atlasEntry = &gs->atlasTPAGEntries[tpagIndex];
    TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];

    // Compute the ratio between atlas size and original TPAG size
    // (in case the preprocessor downscaled)
    float origW = (float) tpag->sourceWidth;
    float origH = (float) tpag->sourceHeight;
    float ratioX = (origW > 0) ? ((float) atlasEntry->width / origW) : 1.0f;
    float ratioY = (origH > 0) ? ((float) atlasEntry->height / origH) : 1.0f;

    // Map srcOffX/Y/W/H from original TPAG space to atlas space
    float atlasOffX = (float) srcOffX * ratioX;
    float atlasOffY = (float) srcOffY * ratioY;
    float atlasSrcW = (float) srcW * ratioX;
    float atlasSrcH = (float) srcH * ratioY;

    // UV coords in atlas texels
    float u1 = (float) atlasEntry->atlasX + atlasOffX;
    float v1 = (float) atlasEntry->atlasY + atlasOffY;
    float u2 = u1 + atlasSrcW;
    float v2 = v1 + atlasSrcH;

    // GS modulate mode: Output = Texture * Vertex / 128
    uint8_t r = BGR_R(color) >> 1;
    uint8_t g = BGR_G(color) >> 1;
    uint8_t b = BGR_B(color) >> 1;
    uint8_t a = (uint8_t) (alpha * 128.0f);
    u64 gsColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, gs->zCounter, gsColor);
    gs->zCounter++;
}

static void gsDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, [[maybe_unused]] bool outline) {
    GsRenderer* gs = (GsRenderer*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t) (alpha * 128.0f);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 rectColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
    gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, rectColor);
    gs->zCounter++;
}

static void gsDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, [[maybe_unused]] float width, uint32_t color, float alpha) {
    GsRenderer* gs = (GsRenderer*) renderer;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t) (alpha * 128.0f);

    float sx1 = (x1 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy1 = (y1 - (float) gs->viewY) * gs->scaleY + gs->offsetY;
    float sx2 = (x2 - (float) gs->viewX) * gs->scaleX + gs->offsetX;
    float sy2 = (y2 - (float) gs->viewY) * gs->scaleY + gs->offsetY;

    u64 lineColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);
    gsKit_prim_line(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, lineColor);
    gs->zCounter++;
}

static void gsDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, [[maybe_unused]] float angleDeg) {
    GsRenderer* gs = (GsRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > renderer->drawFont || (uint32_t) renderer->drawFont >= dw->font.count) return;

    Font* font = &dw->font.fonts[renderer->drawFont];

    // Resolve font texture page and set up GSTEXTURE
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    bool hasTexture = false;
    GSTEXTURE tex;
    AtlasTPAGEntry* atlasEntry = nullptr;
    float ratioX = 1.0f;
    float ratioY = 1.0f;

    if (fontTpagIndex >= 0) {
        hasTexture = setupTextureForTPAG(gs, &tex, fontTpagIndex);
        if (hasTexture) {
            atlasEntry = &gs->atlasTPAGEntries[fontTpagIndex];
            TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];

            // Compute ratio between atlas size and original TPAG size (in case the preprocessor downscaled)
            float origW = (float) fontTpag->sourceWidth;
            float origH = (float) fontTpag->sourceHeight;
            ratioX = (origW > 0) ? ((float) atlasEntry->width / origW) : 1.0f;
            ratioY = (origH > 0) ? ((float) atlasEntry->height / origH) : 1.0f;
        }
    }

    // GS modulate mode: Output = Texture * Vertex / 128
    // Scale vertex RGB from 0-255 to 0-128 so white (255) becomes 1.0x multiplier
    uint32_t color = renderer->drawColor;
    uint8_t r = hasTexture ? (BGR_R(color) >> 1) : BGR_R(color);
    uint8_t g = hasTexture ? (BGR_G(color) >> 1) : BGR_G(color);
    uint8_t b = hasTexture ? (BGR_B(color) >> 1) : BGR_B(color);
    uint8_t a = (uint8_t) (renderer->drawAlpha * 128.0f);
    u64 textColor = GS_SETREG_RGBAQ(r, g, b, a, 0x00);

    // Preprocess GML text (# -> \n, \# -> #)
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);

    // Vertical alignment
    int32_t lineCount = TextUtils_countLines(processed, textLen);
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    while (textLen >= lineStart) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }

        int32_t lineLen = lineEnd - lineStart;
        const char* line = processed + lineStart;

        // Horizontal alignment
        float lineWidth = TextUtils_measureLineWidth(font, line, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Draw each glyph
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(line, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;

            if (glyph->sourceWidth > 0 && glyph->sourceHeight > 0) {
                float glyphX = x + (cursorX + (float) glyph->offset) * xscale * font->scaleX;
                float glyphY = y + cursorY * yscale * font->scaleY;
                float glyphW = (float) glyph->sourceWidth * xscale * font->scaleX;
                float glyphH = (float) glyph->sourceHeight * yscale * font->scaleY;

                float sx1 = (glyphX - (float) gs->viewX) * gs->scaleX + gs->offsetX;
                float sy1 = (glyphY - (float) gs->viewY) * gs->scaleY + gs->offsetY;
                float sx2 = (glyphX + glyphW - (float) gs->viewX) * gs->scaleX + gs->offsetX;
                float sy2 = (glyphY + glyphH - (float) gs->viewY) * gs->scaleY + gs->offsetY;

                if (hasTexture) {
                    // Compute UV coordinates: map glyph position within the font TPAG to atlas space
                    float u1 = (float) atlasEntry->atlasX + (float) glyph->sourceX * ratioX;
                    float v1 = (float) atlasEntry->atlasY + (float) glyph->sourceY * ratioY;
                    float u2 = u1 + (float) glyph->sourceWidth * ratioX;
                    float v2 = v1 + (float) glyph->sourceHeight * ratioY;

                    gsKit_prim_sprite_texture(gs->gsGlobal, &tex, sx1, sy1, u1, v1, sx2, sy2, u2, v2, gs->zCounter, textColor);
                } else {
                    // Fallback: draw colored rectangle if font texture is not available
                    gsKit_prim_sprite(gs->gsGlobal, sx1, sy1, sx2, sy2, gs->zCounter, textColor);
                }
            }

            cursorX += (float) glyph->shift;

            // Kerning
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(line, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        gs->zCounter++;

        // Next line
        cursorY += (float) font->emSize;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            break;
        }
    }

    free(processed);
}

static void gsFlush([[maybe_unused]] Renderer* renderer) {
    // No-op: gsKit queues commands, executed in main loop
}

static int32_t gsCreateSpriteFromSurface([[maybe_unused]] Renderer* renderer, [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y, [[maybe_unused]] int32_t w, [[maybe_unused]] int32_t h, [[maybe_unused]] bool removeback, [[maybe_unused]] bool smooth, [[maybe_unused]] int32_t xorig, [[maybe_unused]] int32_t yorig) {
    fprintf(stderr, "GsRenderer: createSpriteFromSurface not supported on PS2\n");
    return -1;
}

static void gsDeleteSprite([[maybe_unused]] Renderer* renderer, [[maybe_unused]] int32_t spriteIndex) {
    // No-op
}

// ===[ Vtable ]===

static RendererVtable gsVtable = {
    .init = gsInit,
    .destroy = gsDestroy,
    .beginFrame = gsBeginFrame,
    .endFrame = gsEndFrame,
    .beginView = gsBeginView,
    .endView = gsEndView,
    .drawSprite = gsDrawSprite,
    .drawSpritePart = gsDrawSpritePart,
    .drawRectangle = gsDrawRectangle,
    .drawLine = gsDrawLine,
    .drawText = gsDrawText,
    .flush = gsFlush,
    .createSpriteFromSurface = gsCreateSpriteFromSurface,
    .deleteSprite = gsDeleteSprite,
};

// ===[ Public API ]===

Renderer* GsRenderer_create(GSGLOBAL* gsGlobal) {
    GsRenderer* gs = safeCalloc(1, sizeof(GsRenderer));
    gs->base.vtable = &gsVtable;
    gs->gsGlobal = gsGlobal;
    gs->scaleX = 2.0f;
    gs->scaleY = 2.0f;
    gs->zCounter = 1;
    return (Renderer*) gs;
}
