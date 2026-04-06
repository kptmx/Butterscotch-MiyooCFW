#pragma once

#include "audio_system.h"
#include <SDL/SDL_mixer.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SOUND_INSTANCES 128
#define SOUND_INSTANCE_ID_BASE 100000
#define MAX_AUDIO_STREAMS 32
#define MAX_CACHED_SOUNDS 32
// This is the index space that the native runner uses
#define AUDIO_STREAM_INDEX_BASE 300000

// PCM data decoded by Tremor (int16 -> float conversion for SDL_mixer)
typedef struct {
    int16_t* pcmData;      // Decoded PCM (mono, int16)
    uint64_t sampleCount;  // Number of PCM samples
    uint32_t sampleRate;   // Sample rate of PCM data
} TremorDecodedSound;

typedef struct {
    Mix_Chunk* chunk;      // SDL_mixer chunk for frequent sounds
    int32_t soundIndex;    // SOND resource index
    bool owned;            // true if we own the chunk (needs Mix_FreeChunk)
} CachedSound;

typedef struct {
    bool active;
    int32_t soundIndex;    // SOND resource that spawned this
    int32_t instanceId;    // unique ID returned to GML
    int channel;           // SDL_mixer channel (-1 if not playing)
    Mix_Chunk* chunk;      // Audio chunk (nullptr if streaming)
    Mix_Music* music;      // Music stream (for looping/streams)
    bool ownsChunk;        // true if chunk needs freeing on destroy
    bool isMusic;          // true if this is a music stream (uses Mix_PlayingMusic)
    float targetGain;
    float currentGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float startGain;
    int32_t priority;
} SoundInstance;

typedef struct {
    bool active;
    char* filePath;        // resolved file path (owned, freed on destroy)
    Mix_Music* music;      // SDL_mixer music object
} AudioStreamEntry;

typedef struct {
    AudioSystem base;
    bool initialized;
    SoundInstance instances[MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;
    FileSystem* fileSystem;
    AudioStreamEntry streams[MAX_AUDIO_STREAMS];
    CachedSound cachedSounds[MAX_CACHED_SOUNDS]; // Pre-decoded PCM cache for frequent sounds
    int masterVolume;      // 0-128
} SDLMixAudioSystem;

SDLMixAudioSystem* SDLMixAudioSystem_create(void);

#ifdef __cplusplus
}
#endif
