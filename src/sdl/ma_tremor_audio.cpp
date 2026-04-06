// miniaudio + Tremor audio system for SDL build
// Uses miniaudio engine for mixing/playback + Tremor (libvorbisidec) for OGG decoding

#ifdef _WIN32
#include <windows.h>
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "tremor_backend.h"
#include "data_win.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_ds.h"

// Include the audio system base header
#include "audio_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SOUND_INSTANCES 128
#define SOUND_INSTANCE_ID_BASE 100000
#define MAX_AUDIO_STREAMS 32
#define MAX_CACHED_SOUNDS 128
#define AUDIO_STREAM_INDEX_BASE 300000

typedef struct {
    ma_data_source_base base;     // MUST be first — miniaudio expects isLooping/refCount here
    int32_t soundIndex;
    float* pcmData;
    uint64_t frameCount;
    uint32_t sampleRate;
    uint64_t cursorFrame;
} CachedSound;

typedef struct {
    bool active;
    int32_t soundIndex;
    int32_t instanceId;
    ma_sound maSound;
    ma_decoder decoder;
    bool ownsDecoder;
    float targetGain;
    float currentGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float startGain;
    int32_t priority;
} SoundInstance;

typedef struct {
    bool active;
    char* filePath;
} AudioStreamEntry;

typedef struct {
    AudioSystem base;
    ma_engine engine;
    SoundInstance instances[MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;
    FileSystem* fileSystem;
    AudioStreamEntry streams[MAX_AUDIO_STREAMS];
    CachedSound cachedSounds[MAX_CACHED_SOUNDS];
} MaTremorAudioSystem;

MaTremorAudioSystem* MaTremorAudioSystem_create(void);

#ifdef __cplusplus
}
#endif

// ===[ CachedSound ma_data_source vtable ]===
static ma_result cachedSoundReadPCMFrames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead);
static ma_result cachedSoundSeek(ma_data_source* pDataSource, ma_uint64 frameIndex);
static ma_result cachedSoundGetFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap);
static ma_result cachedSoundGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor);
static ma_result cachedSoundGetLength(ma_data_source* pDataSource, ma_uint64* pLength);
static ma_result cachedSoundSetLooping(ma_data_source* pDataSource, ma_bool32 isLooping);

static ma_data_source_vtable g_cachedSoundVtable = {
    cachedSoundReadPCMFrames,
    cachedSoundSeek,
    cachedSoundGetFormat,
    cachedSoundGetCursor,
    cachedSoundGetLength,
    cachedSoundSetLooping,
    0
};

static CachedSound* findCachedSound(MaTremorAudioSystem* sys, int32_t soundIndex) {
    for (int i = 0; i < MAX_CACHED_SOUNDS; i++) {
        if (sys->cachedSounds[i].soundIndex == soundIndex && sys->cachedSounds[i].pcmData) {
            return &sys->cachedSounds[i];
        }
    }
    return nullptr;
}

// Find free cache slot — returns nullptr if all slots are occupied.
// NEVER evicts — if cache is full, we just don't cache.
static CachedSound* findFreeCacheSlot(MaTremorAudioSystem* sys) {
    for (int i = 0; i < MAX_CACHED_SOUNDS; i++) {
        if (!sys->cachedSounds[i].pcmData) return &sys->cachedSounds[i];
    }
    // Cache full — return nullptr, don't evict
    return nullptr;
}

// ===[ CachedSound ma_data_source vtable ]===
static ma_result cachedSoundReadPCMFrames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
    CachedSound* cached = (CachedSound*)pDataSource;
    float* out = (float*)pFramesOut;
    ma_uint64 available = cached->frameCount - cached->cursorFrame;
    ma_uint64 toRead = frameCount < available ? frameCount : available;
    for (ma_uint64 i = 0; i < toRead; i++) {
        out[i] = cached->pcmData[cached->cursorFrame + i];
    }
    cached->cursorFrame += toRead;
    if (pFramesRead) *pFramesRead = toRead;
    return toRead > 0 ? MA_SUCCESS : MA_AT_END;
}

static ma_result cachedSoundSeek(ma_data_source* pDataSource, ma_uint64 frameIndex) {
    CachedSound* cached = (CachedSound*)pDataSource;
    if (frameIndex > cached->frameCount) return MA_INVALID_ARGS;
    cached->cursorFrame = frameIndex;
    return MA_SUCCESS;
}

static ma_result cachedSoundGetFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap) {
    CachedSound* cached = (CachedSound*)pDataSource;
    (void)pChannelMap;
    (void)channelMapCap;
    if (pFormat) *pFormat = ma_format_f32;
    if (pChannels) *pChannels = 1;
    if (pSampleRate) *pSampleRate = cached->sampleRate;
    return MA_SUCCESS;
}

static ma_result cachedSoundGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor) {
    CachedSound* cached = (CachedSound*)pDataSource;
    if (pCursor) *pCursor = cached->cursorFrame;
    return MA_SUCCESS;
}

static ma_result cachedSoundGetLength(ma_data_source* pDataSource, ma_uint64* pLength) {
    CachedSound* cached = (CachedSound*)pDataSource;
    if (pLength) *pLength = cached->frameCount;
    return MA_SUCCESS;
}

static ma_result cachedSoundSetLooping(ma_data_source* pDataSource, ma_bool32 isLooping) {
    (void)pDataSource;
    (void)isLooping;
    // Looping is managed by ma_sound, not the data source
    return MA_SUCCESS;
}

static SoundInstance* findFreeSlot(MaTremorAudioSystem* sys) {
    // First pass: find an inactive slot
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (!sys->instances[i].active) {
            return &sys->instances[i];
        }
    }

    // Second pass: evict the lowest-priority ended sound
    SoundInstance* best = nullptr;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (!ma_sound_is_playing(&inst->maSound)) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        ma_sound_uninit(&best->maSound);
        if (best->ownsDecoder) {
            ma_decoder_uninit(&best->decoder);
        }
        best->active = false;
    }

    return best;
}

static SoundInstance* findInstanceById(MaTremorAudioSystem* sys, int32_t instanceId) {
    int32_t slotIndex = instanceId - SOUND_INSTANCE_ID_BASE;
    if (slotIndex < 0 || slotIndex >= MAX_SOUND_INSTANCES) return nullptr;
    SoundInstance* inst = &sys->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}

static char* resolveExternalPath(MaTremorAudioSystem* sys, Sound* sound) {
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    bool hasExtension = (strchr(file, '.') != nullptr);
    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }

    char* resolvedPath = sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
    fprintf(stderr, "Audio: Resolved '%s' -> '%s'\n", file, resolvedPath ? resolvedPath : "NULL");
    return resolvedPath;
}

// ===[ Vtable Implementations ]===

static void maInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;
    arrput(sys->base.audioGroups, dataWin);
    sys->fileSystem = fileSystem;

    // Configure miniaudio
    ma_engine_config config = ma_engine_config_init();
    config.sampleRate = 44100;
    config.channels = 1;  // Mono — half the CPU and memory of stereo
    config.noDevice = MA_FALSE;

    ma_result result = ma_engine_init(&config, &sys->engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Audio: Failed to initialize miniaudio engine (error %d)\n", result);
        return;
    }

    fprintf(stderr, "Audio: miniaudio + Tremor engine initialized (sample rate: %u, channels: %u, mono)\n",
            config.sampleRate, config.channels);

    memset(sys->instances, 0, sizeof(sys->instances));
    sys->nextInstanceCounter = 0;
}

static void maDestroy(AudioSystem* audio) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (sys->instances[i].active) {
            ma_sound_uninit(&sys->instances[i].maSound);
            if (sys->instances[i].ownsDecoder) {
                ma_decoder_uninit(&sys->instances[i].decoder);
            }
            sys->instances[i].active = false;
        }
    }

    for (int i = 0; i < MAX_AUDIO_STREAMS; i++) {
        if (sys->streams[i].active) {
            free(sys->streams[i].filePath);
        }
    }

    for (int i = 0; i < MAX_CACHED_SOUNDS; i++) {
        if (sys->cachedSounds[i].pcmData) {
            ma_free(sys->cachedSounds[i].pcmData, nullptr);
            sys->cachedSounds[i].pcmData = nullptr;
        }
    }

    ma_engine_uninit(&sys->engine);
    free(sys);
}

static void maUpdate(AudioSystem* audio, float deltaTime) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (!inst->active) continue;

        // Handle gain fading
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            ma_sound_set_volume(&inst->maSound, inst->currentGain);
        }

        // Clean up ended sounds
        if (ma_sound_at_end(&inst->maSound) && !ma_sound_is_looping(&inst->maSound)) {
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }
}

static int32_t maPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    bool isStream = (soundIndex >= AUDIO_STREAM_INDEX_BASE);
    Sound* sound = nullptr;
    char* streamPath = nullptr;

    if (isStream) {
        int32_t streamSlot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (streamSlot < 0 || streamSlot >= MAX_AUDIO_STREAMS || !sys->streams[streamSlot].active) {
            fprintf(stderr, "Audio: Invalid stream index %d\n", soundIndex);
            return -1;
        }
        streamPath = sys->streams[streamSlot].filePath;
    } else {
        DataWin* dw = sys->base.audioGroups[0];
        if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) {
            fprintf(stderr, "Audio: Invalid sound index %d\n", soundIndex);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
    }

    SoundInstance* slot = findFreeSlot(sys);
    if (slot == nullptr) {
        fprintf(stderr, "Audio: No free sound slots for sound %d\n", soundIndex);
        return -1;
    }

    int32_t slotIndex = (int32_t)(slot - sys->instances);
    ma_result result;

    if (isStream) {
        // Stream: create decoder with Tremor backend, then attach to sound
        ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 1, 44100);
        ma_decoding_backend_vtable* pStreamCustomBackends[] = { (ma_decoding_backend_vtable*)getTremorBackendVTable() };
        decoderConfig.ppCustomBackendVTables = pStreamCustomBackends;
        decoderConfig.customBackendCount = 1;
        decoderConfig.pCustomBackendUserData = nullptr;

        result = ma_decoder_init_file(streamPath, &decoderConfig, &slot->decoder);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to decode stream file '%s' (error %d)\n", streamPath, result);
            return -1;
        }
        slot->ownsDecoder = true;
        result = ma_sound_init_from_data_source(&sys->engine, &slot->decoder, 0, nullptr, &slot->maSound);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to load stream file '%s' (error %d)\n", streamPath, result);
            return -1;
        }
        slot->ownsDecoder = false;
    } else {
        bool isEmbedded = (sound->flags & 0x01) != 0;

        if (isEmbedded) {
            if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= sys->base.audioGroups[sound->audioGroup]->audo.count) {
                fprintf(stderr, "Audio: Invalid audio file index %d for sound '%s'\n",
                        sound->audioFile, sound->name);
                return -1;
            }

            AudioEntry* entry = &sys->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];

            // Check cache first
            CachedSound* cached = findCachedSound(sys, sound->audioFile);
            if (cached) {
                // Play from cache
                cached->cursorFrame = 0;
                result = ma_sound_init_from_data_source(&sys->engine, (ma_data_source*)cached, 0, nullptr, &slot->maSound);
                if (result == MA_SUCCESS) {
                    slot->ownsDecoder = false;
                    slot->soundIndex = soundIndex;
                    slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
                    slot->priority = priority;
                    slot->targetGain = sound->volume;
                    slot->currentGain = sound->volume;
                    slot->fadeTimeRemaining = 0;
                    slot->startGain = sound->volume;
                    ma_sound_set_volume(&slot->maSound, slot->currentGain);
                    if (sound->pitch != 1.0f) {
                        ma_sound_set_pitch(&slot->maSound, sound->pitch);
                    }
                    ma_sound_set_looping(&slot->maSound, loop);
                    ma_sound_start(&slot->maSound);
                    slot->active = true;
                    return slot->instanceId;
                }
            }

            // Not cached — decode from memory to PCM first
            ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 1, 44100);
            ma_decoding_backend_vtable* pDecCustomBackends[] = { (ma_decoding_backend_vtable*)getTremorBackendVTable() };
            decoderConfig.ppCustomBackendVTables = pDecCustomBackends;
            decoderConfig.customBackendCount = 1;
            decoderConfig.pCustomBackendUserData = nullptr;

            ma_decoder decoder;
            result = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &decoder);
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to init decoder for '%s' (error %d)\n", sound->name, result);
                return -1;
            }

            // Read all PCM frames to determine size
            float tempBuf[4096];
            ma_uint64 totalFrames = 0;
            while (true) {
                ma_uint64 frames;
                ma_result r = ma_decoder_read_pcm_frames(&decoder, tempBuf, 4096, &frames);
                if (r != MA_SUCCESS || frames == 0) break;
                totalFrames += frames;
            }

            // Try to find a cache slot
            CachedSound* cacheSlot = findFreeCacheSlot(sys);
            bool playedFromCache = false;

            if (cacheSlot && totalFrames > 0) {
                // Second pass: read into cache buffer
                float* pcmBuffer = (float*)ma_malloc(totalFrames * sizeof(float), nullptr);
                if (pcmBuffer) {
                    ma_decoder_seek_to_pcm_frame(&decoder, 0);
                    ma_uint64 framesRead = 0;
                    result = ma_decoder_read_pcm_frames(&decoder, pcmBuffer, totalFrames, &framesRead);
                    if (result == MA_SUCCESS) {
                        // Initialize the ma_data_source_base part
                        ma_data_source_config dsCfg = ma_data_source_config_init();
                        dsCfg.vtable = &g_cachedSoundVtable;
                        ma_data_source_init(&dsCfg, (ma_data_source*)&cacheSlot->base);

                        cacheSlot->soundIndex = sound->audioFile;
                        cacheSlot->pcmData = pcmBuffer;
                        cacheSlot->frameCount = framesRead;
                        cacheSlot->sampleRate = decoder.outputSampleRate;
                        cacheSlot->cursorFrame = 0;
                        fprintf(stderr, "Audio: Cached sound '%s' (%llu frames, mono f32)\n", sound->name, (unsigned long long)framesRead);

                        // Play from the new cache
                        cacheSlot->cursorFrame = 0;
                        result = ma_sound_init_from_data_source(&sys->engine, (ma_data_source*)cacheSlot, 0, nullptr, &slot->maSound);
                        ma_decoder_uninit(&decoder);
                        if (result == MA_SUCCESS) {
                            slot->ownsDecoder = false;
                            playedFromCache = true;
                        }
                    } else {
                        ma_free(pcmBuffer, nullptr);
                        cacheSlot->pcmData = nullptr;
                    }
                }
            }

            if (!playedFromCache) {
                // Cache unavailable — play directly via decoder
                ma_decoder_uninit(&decoder);
                result = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &slot->decoder);
                if (result != MA_SUCCESS) {
                    fprintf(stderr, "Audio: Failed to init decoder for '%s' (error %d)\n", sound->name, result);
                    return -1;
                }
                slot->ownsDecoder = true;
                result = ma_sound_init_from_data_source(&sys->engine, &slot->decoder, 0, nullptr, &slot->maSound);
                if (result != MA_SUCCESS) {
                    fprintf(stderr, "Audio: Failed to init sound from decoder for '%s' (error %d)\n", sound->name, result);
                    ma_decoder_uninit(&slot->decoder);
                    return -1;
                }
            }
        } else {
            char* path = resolveExternalPath(sys, sound);
            if (path == nullptr) {
                fprintf(stderr, "Audio: Could not resolve path for sound '%s'\n", sound->name);
                return -1;
            }

            // Create decoder with Tremor backend
            ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 1, 44100);
            ma_decoding_backend_vtable* pExtCustomBackends[] = { (ma_decoding_backend_vtable*)getTremorBackendVTable() };
            decoderConfig.ppCustomBackendVTables = pExtCustomBackends;
            decoderConfig.customBackendCount = 1;
            decoderConfig.pCustomBackendUserData = nullptr;

            result = ma_decoder_init_file(path, &decoderConfig, &slot->decoder);
            free(path);
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to load file for '%s' (error %d)\n", sound->name, result);
                return -1;
            }
            slot->ownsDecoder = true;
            result = ma_sound_init_from_data_source(&sys->engine, &slot->decoder, 0, nullptr, &slot->maSound);
        }
    }

    // Apply properties
    float volume = isStream ? 1.0f : sound->volume;
    float pitch = isStream ? 1.0f : sound->pitch;
    ma_sound_set_volume(&slot->maSound, volume);
    if (pitch != 1.0f) {
        ma_sound_set_pitch(&slot->maSound, pitch);
    }
    ma_sound_set_looping(&slot->maSound, loop);

    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->startGain = volume;
    slot->priority = priority;

    sys->nextInstanceCounter++;
    fprintf(stderr, "Audio: Playing sound '%s' (instance ID %d, priority %d, loop %s)\n",
            isStream ? streamPath : sound->name, slot->instanceId, priority, loop ? "yes" : "no");
    ma_sound_start(&slot->maSound);

    return slot->instanceId;
}

static void maStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
                ma_sound_uninit(&inst->maSound);
                if (inst->ownsDecoder) {
                    ma_decoder_uninit(&inst->decoder);
                }
                inst->active = false;
            }
        }
    }
}

static void maStopAll(AudioSystem* audio) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (inst->active) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }
}

static bool maIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        return inst != nullptr && ma_sound_is_playing(&inst->maSound);
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance && ma_sound_is_playing(&inst->maSound)) {
                return true;
            }
        }
        return false;
    }
}

static void maPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
            }
        }
    }
}

static void maResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_start(&inst->maSound);
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_start(&inst->maSound);
            }
        }
    }
}

static void maPauseAll(AudioSystem* audio) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (inst->active && ma_sound_is_playing(&inst->maSound)) {
            ma_sound_stop(&inst->maSound);
        }
    }
}

static void maResumeAll(AudioSystem* audio) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (inst->active) {
            ma_sound_start(&inst->maSound);
        }
    }
}

static void maSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                ma_sound_set_volume(&inst->maSound, gain);
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float)timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (timeMs == 0) {
                    inst->currentGain = gain;
                    inst->targetGain = gain;
                    inst->fadeTimeRemaining = 0.0f;
                    ma_sound_set_volume(&inst->maSound, gain);
                } else {
                    inst->startGain = inst->currentGain;
                    inst->targetGain = gain;
                    inst->fadeTotalTime = (float)timeMs / 1000.0f;
                    inst->fadeTimeRemaining = inst->fadeTotalTime;
                }
            }
        }
    }
}

static float maGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) return inst->currentGain;
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return inst->currentGain;
            }
        }
    }
    return 0.0f;
}

static void maSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_set_pitch(&inst->maSound, pitch);
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_set_pitch(&inst->maSound, pitch);
            }
        }
    }
}

static float maGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) return ma_sound_get_pitch(&inst->maSound);
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return ma_sound_get_pitch(&inst->maSound);
            }
        }
    }
    return 1.0f;
}

static float maGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            float cursor;
            ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
            if (result == MA_SUCCESS) return cursor;
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                float cursor;
                ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
                if (result == MA_SUCCESS) return cursor;
            }
        }
    }
    return 0.0f;
}

static void maSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64)(positionSeconds * 44100.0f));
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64)(positionSeconds * 44100.0f));
            }
        }
    }
}

static void maSetMasterGain(AudioSystem* audio, float gain) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;
    ma_engine_set_volume(&sys->engine, gain);
}

static void maSetChannelCount([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t count) {
    // miniaudio handles channel management internally
}

static void maGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex > 0) {
        int sz = snprintf(nullptr, 0, "audiogroup%d.dat", groupIndex);
        char buf[sz + 1];
        snprintf(buf, sizeof(buf), "audiogroup%d.dat", groupIndex);
        DataWin* audioGroup = DataWin_parse(((MaTremorAudioSystem*)audio)->fileSystem->vtable->resolvePath(((MaTremorAudioSystem*)audio)->fileSystem, buf),
        (DataWinParserOptions){ 0 });
        arrput(audio->audioGroups, audioGroup);
    }
}

static bool maGroupIsLoaded([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

// ===[ Audio Streams ]===

static int32_t maCreateStream(AudioSystem* audio, const char* filename) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    int32_t freeSlot = -1;
    for (int i = 0; i < MAX_AUDIO_STREAMS; i++) {
        if (!sys->streams[i].active) {
            freeSlot = (int32_t)i;
            break;
        }
    }

    if (freeSlot < 0) {
        fprintf(stderr, "Audio: No free stream slots for '%s'\n", filename);
        return -1;
    }

    char* resolved = sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
    if (resolved == nullptr) {
        fprintf(stderr, "Audio: Could not resolve path for stream '%s'\n", filename);
        return -1;
    }

    sys->streams[freeSlot].active = true;
    sys->streams[freeSlot].filePath = resolved;

    int32_t streamIndex = AUDIO_STREAM_INDEX_BASE + freeSlot;
    fprintf(stderr, "Audio: Created stream %d for '%s' -> '%s'\n", streamIndex, filename, resolved);
    return streamIndex;
}

static bool maDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*) audio;

    int32_t slotIndex = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (slotIndex < 0 || slotIndex >= MAX_AUDIO_STREAMS) {
        fprintf(stderr, "Audio: Invalid stream index %d for destroy\n", streamIndex);
        return false;
    }

    AudioStreamEntry* entry = &sys->streams[slotIndex];
    if (!entry->active) return false;

    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (inst->active && inst->soundIndex == streamIndex) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }

    free(entry->filePath);
    entry->filePath = nullptr;
    entry->active = false;
    fprintf(stderr, "Audio: Destroyed stream %d\n", streamIndex);
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable maAudioSystemVtable = {
    .init = maInit,
    .destroy = maDestroy,
    .update = maUpdate,
    .playSound = maPlaySound,
    .stopSound = maStopSound,
    .stopAll = maStopAll,
    .isPlaying = maIsPlaying,
    .pauseSound = maPauseSound,
    .resumeSound = maResumeSound,
    .pauseAll = maPauseAll,
    .resumeAll = maResumeAll,
    .setSoundGain = maSetSoundGain,
    .getSoundGain = maGetSoundGain,
    .setSoundPitch = maSetSoundPitch,
    .getSoundPitch = maGetSoundPitch,
    .getTrackPosition = maGetTrackPosition,
    .setTrackPosition = maSetTrackPosition,
    .setMasterGain = maSetMasterGain,
    .setChannelCount = maSetChannelCount,
    .groupLoad = maGroupLoad,
    .groupIsLoaded = maGroupIsLoaded,
    .createStream = maCreateStream,
    .destroyStream = maDestroyStream,
};

// ===[ Lifecycle ]===

MaTremorAudioSystem* MaTremorAudioSystem_create(void) {
    MaTremorAudioSystem* sys = (MaTremorAudioSystem*)safeCalloc(1, sizeof(MaTremorAudioSystem));
    sys->base.vtable = &maAudioSystemVtable;
    return sys;
}
