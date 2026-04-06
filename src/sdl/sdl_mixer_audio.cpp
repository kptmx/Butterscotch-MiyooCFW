// SDL_mixer + Tremor audio system
// Replaces miniaudio + stb_vorbis for better performance on embedded ARM

#include "sdl_mixer_audio.h"
#include "data_win.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_ds.h"

// Tremor (libvorbisidec) - integer OGG Vorbis decoder
#include <ivorbisfile.h>

// ===[ Helpers ]===

// Check if sound name contains "TXT" (frequently played sounds)
static bool isFrequentSound(const char* name) {
    if (!name) return false;
    // Case-insensitive search for "txt" in the name
    for (const char* p = name; *p; p++) {
        if ((p[0] == 'T' || p[0] == 't') &&
            (p[1] == 'X' || p[1] == 'x') &&
            (p[2] == 'T' || p[2] == 't')) {
            return true;
        }
    }
    return false;
}

// Find cached sound by index, returns nullptr if not found
static CachedSound* findCachedSound(SDLMixAudioSystem* sys, int32_t soundIndex) {
    for (int i = 0; i < MAX_CACHED_SOUNDS; i++) {
        if (sys->cachedSounds[i].soundIndex == soundIndex && sys->cachedSounds[i].chunk) {
            return &sys->cachedSounds[i];
        }
    }
    return nullptr;
}

// Find free cache slot
static CachedSound* findFreeCacheSlot(SDLMixAudioSystem* sys) {
    for (int i = 0; i < MAX_CACHED_SOUNDS; i++) {
        if (!sys->cachedSounds[i].chunk) return &sys->cachedSounds[i];
    }
    // Cache full — evict oldest (index 0)
    CachedSound* slot = &sys->cachedSounds[0];
    if (slot->chunk) {
        Mix_FreeChunk(slot->chunk);
        slot->chunk = nullptr;
    }
    return slot;
}

// Decode OGG to PCM using Tremor, then create SDL_mixer chunk
static Mix_Chunk* decodeOGGToChunk(const char* filename, int* outSampleRate) {
    OggVorbis_File vf;
    
    // Try opening as file
    FILE* f = fopen(filename, "rb");
    if (!f) return nullptr;
    
    int error = ov_open(f, &vf, nullptr, 0);
    if (error < 0) {
        fclose(f);
        fprintf(stderr, "Audio: Tremor failed to open OGG '%s' (error %d)\n", filename, error);
        return nullptr;
    }
    
    vorbis_info* vi = ov_info(&vf, -1);
    int sampleRate = vi->rate;
    
    if (outSampleRate) *outSampleRate = sampleRate;
    
    // Read all PCM data (convert to mono int16)
    const int bufferSize = 4096;
    int16_t* pcmBuffer = (int16_t*)malloc(bufferSize * sizeof(int16_t));
    if (!pcmBuffer) {
        ov_clear(&vf);
        return nullptr;
    }
    
    // First pass: count total samples
    uint64_t totalSamples = 0;
    long samplesRead;
    int current_section;
    while ((samplesRead = ov_read(&vf, (char*)pcmBuffer, bufferSize * sizeof(int16_t), &current_section)) > 0) {
        totalSamples += samplesRead / sizeof(int16_t);
    }
    
    if (totalSamples == 0) {
        free(pcmBuffer);
        ov_clear(&vf);
        fprintf(stderr, "Audio: No PCM data decoded from '%s'\n", filename);
        return nullptr;
    }
    
    // Second pass: read into final buffer
    int16_t* finalBuffer = (int16_t*)malloc(totalSamples * sizeof(int16_t));
    if (!finalBuffer) {
        free(pcmBuffer);
        ov_clear(&vf);
        return nullptr;
    }
    
    // Rewind by reopening file
    ov_clear(&vf);
    f = fopen(filename, "rb");
    if (!f) {
        free(pcmBuffer);
        free(finalBuffer);
        return nullptr;
    }
    
    ov_open(f, &vf, nullptr, 0);
    
    uint64_t offset = 0;
    while ((samplesRead = ov_read(&vf, (char*)pcmBuffer, bufferSize * sizeof(int16_t), &current_section)) > 0) {
        int count = samplesRead / sizeof(int16_t);
        if (offset + count <= totalSamples) {
            memcpy(finalBuffer + offset, pcmBuffer, samplesRead);
            offset += count;
        }
    }
    
    free(pcmBuffer);
    ov_clear(&vf);
    
    // Create SDL_mixer chunk
    Mix_Chunk* chunk = Mix_QuickLoad_RAW((Uint8*)finalBuffer, (Uint32)(totalSamples * sizeof(int16_t)));
    if (!chunk) {
        free(finalBuffer);
        fprintf(stderr, "Audio: Mix_QuickLoad_RAW failed for '%s'\n", filename);
        return nullptr;
    }
    
    // Mark that we own the buffer (SDL_mixer doesn't copy it)
    chunk->allocated = 1;
    
    fprintf(stderr, "Audio: Decoded OGG '%s' -> %llu samples @ %dHz (mono int16)\n", 
            filename, (unsigned long long)totalSamples, sampleRate);
    
    return chunk;
}

// Cache sound by decoding OGG to Mix_Chunk
static bool cacheSound(SDLMixAudioSystem* sys, Sound* sound) {
    if (!sound) return false;
    
    // Resolve file path
    const char* file = sound->file;
    if (!file || file[0] == '\0') return false;
    
    bool hasExtension = (strchr(file, '.') != nullptr);
    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }
    
    char* resolvedPath = sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
    if (!resolvedPath) return false;
    
    // Decode OGG to Mix_Chunk using Tremor
    int sampleRate = 0;
    Mix_Chunk* chunk = decodeOGGToChunk(resolvedPath, &sampleRate);
    free(resolvedPath);
    
    if (!chunk) return false;
    
    // Store in cache
    CachedSound* slot = findFreeCacheSlot(sys);
    slot->chunk = chunk;
    slot->soundIndex = sound->audioFile;
    slot->owned = true;
    
    fprintf(stderr, "Audio: Cached sound '%s' (chunk %p)\n", sound->name, (void*)chunk);
    return true;
}

static SoundInstance* findFreeSlot(SDLMixAudioSystem* sys) {
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
        bool ended = false;
        if (inst->isMusic) {
            ended = !Mix_PlayingMusic();
        } else if (inst->channel >= 0) {
            ended = !Mix_Playing(inst->channel);
        }
        if (inst->active && ended) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        // Only stop THIS instance's audio, not global music
        if (best->isMusic && best->music) {
            Mix_HaltMusic();
            Mix_FreeMusic(best->music);
            best->music = nullptr;
        }
        if (best->ownsChunk && best->chunk) {
            Mix_FreeChunk(best->chunk);
            best->chunk = nullptr;
        }
        best->active = false;
        best->channel = -1;
        best->isMusic = false;
    }

    return best;
}

static SoundInstance* findInstanceById(SDLMixAudioSystem* sys, int32_t instanceId) {
    int32_t slotIndex = instanceId - SOUND_INSTANCE_ID_BASE;
    if (slotIndex < 0 || slotIndex >= MAX_SOUND_INSTANCES) return nullptr;
    SoundInstance* inst = &sys->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}

// Helper: resolve external audio file path from Sound entry
static char* resolveExternalPath(SDLMixAudioSystem* sys, Sound* sound) {
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;
    
    // If the filename has no extension, append ".ogg"
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

static void sdlMixInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    arrput(sys->base.audioGroups, dataWin);
    sys->fileSystem = fileSystem;
    sys->masterVolume = MIX_MAX_VOLUME; // 128
    
    // Initialize SDL_mixer
    int audio_rate = 44100;
    Uint16 audio_format = AUDIO_S16SYS;
    int audio_channels = 1; // Mono - saves CPU on embedded
    int audio_buffers = 1024;
    
    if (Mix_OpenAudio(audio_rate, audio_format, audio_channels, audio_buffers) < 0) {
        fprintf(stderr, "Audio: Failed to initialize SDL_mixer: %s\n", Mix_GetError());
        sys->initialized = false;
        return;
    }
    
    // Allocate more channels for polyphony
    Mix_AllocateChannels(32);
    
    sys->initialized = true;
    memset(sys->instances, 0, sizeof(sys->instances));
    sys->nextInstanceCounter = 0;
    
    fprintf(stderr, "Audio: SDL_mixer initialized (rate=%d, channels=%d, buffers=%d)\n",
            audio_rate, audio_channels, audio_buffers);
}

static void sdlMixDestroy(AudioSystem* audio) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
    // Uninit all active sound instances
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (sys->instances[i].active) {
            if (sys->instances[i].music) {
                Mix_HaltChannel(sys->instances[i].channel);
                Mix_FreeMusic(sys->instances[i].music);
                sys->instances[i].music = nullptr;
            }
            if (sys->instances[i].ownsChunk && sys->instances[i].chunk) {
                Mix_FreeChunk(sys->instances[i].chunk);
                sys->instances[i].chunk = nullptr;
            }
            sys->instances[i].active = false;
        }
    }
    
    // Free stream entries
    for (int i = 0; i < MAX_AUDIO_STREAMS; i++) {
        if (sys->streams[i].active) {
            if (sys->streams[i].music) {
                Mix_FreeMusic(sys->streams[i].music);
                sys->streams[i].music = nullptr;
            }
            free(sys->streams[i].filePath);
        }
    }
    
    // Free cached chunks
    for (int i = 0; i < MAX_CACHED_SOUNDS; i++) {
        if (sys->cachedSounds[i].chunk && sys->cachedSounds[i].owned) {
            Mix_FreeChunk(sys->cachedSounds[i].chunk);
            sys->cachedSounds[i].chunk = nullptr;
        }
    }
    
    Mix_CloseAudio();
    free(sys);
}

static void sdlMixUpdate(AudioSystem* audio, float deltaTime) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;

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
            // SDL_mixer volume is 0-128
            int vol = (int)(inst->currentGain * MIX_MAX_VOLUME);
            if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
            if (vol < 0) vol = 0;
            if (inst->isMusic) {
                Mix_VolumeMusic(vol);
            } else if (inst->channel >= 0) {
                Mix_Volume(inst->channel, vol);
            }
        }

        // Clean up ended sounds
        bool ended = false;
        if (inst->isMusic) {
            // For music, check if it's still playing
            ended = !Mix_PlayingMusic();
        } else if (inst->channel >= 0) {
            ended = !Mix_Playing(inst->channel);
        }

        if (ended) {
            if (inst->music) {
                Mix_FreeMusic(inst->music);
                inst->music = nullptr;
            }
            if (inst->ownsChunk && inst->chunk) {
                Mix_FreeChunk(inst->chunk);
                inst->chunk = nullptr;
            }
            inst->active = false;
            inst->channel = -1;
            inst->isMusic = false;
        }
    }
}

static int32_t sdlMixPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
    if (!sys->initialized) {
        fprintf(stderr, "Audio: System not initialized\n");
        return -1;
    }
    
    // Check if this is a stream index (created by audio_create_stream)
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
        DataWin* dw = sys->base.audioGroups[0]; // Audio Group 0 should always be data.win
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
    
    if (isStream) {
        // Stream audio: load from file path
        Mix_Music* music = Mix_LoadMUS(streamPath);
        if (!music) {
            fprintf(stderr, "Audio: Failed to load stream '%s': %s\n", streamPath, Mix_GetError());
            return -1;
        }

        slot->music = music;
        slot->chunk = nullptr;
        slot->ownsChunk = false;
        slot->isMusic = true;
        slot->channel = -1;

        int loops = loop ? -1 : 0;
        if (Mix_PlayMusic(music, loops) < 0) {
            fprintf(stderr, "Audio: Failed to play stream: %s\n", Mix_GetError());
            Mix_FreeMusic(music);
            slot->music = nullptr;
            return -1;
        }
    } else {
        bool isEmbedded = (sound->flags & 0x01) != 0;
        
        if (isEmbedded) {
            // Check cache first for frequent sounds (TXT)
            CachedSound* cached = nullptr;
            if (isFrequentSound(sound->name)) {
                cached = findCachedSound(sys, sound->audioFile);
            }
            
            if (cached) {
                // Play from cached chunk
                int loops = loop ? -1 : 0;
                int vol = (int)(sound->volume * MIX_MAX_VOLUME);
                slot->channel = Mix_PlayChannel(-1, cached->chunk, loops);
                if (slot->channel < 0) {
                    fprintf(stderr, "Audio: Failed to play cached sound '%s'\n", sound->name);
                    return -1;
                }
                Mix_Volume(slot->channel, vol);
                
                slot->chunk = cached->chunk;
                slot->ownsChunk = false;
                slot->music = nullptr;
                slot->isMusic = false;
                slot->soundIndex = soundIndex;
                slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
                slot->priority = priority;
                slot->targetGain = sound->volume;
                slot->currentGain = sound->volume;
                slot->fadeTimeRemaining = 0;
                slot->startGain = sound->volume;
                slot->active = true;
                return slot->instanceId;
            }
            
            // Not cached - decode and play
            // For embedded sounds, we need to decode from memory
            // Since Tremor doesn't support memory directly, use SDL_mixer's built-in decoder
            if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= sys->base.audioGroups[sound->audioGroup]->audo.count) {
                fprintf(stderr, "Audio: Invalid audio file index %d for sound '%s'\n", 
                        sound->audioFile, sound->name);
                return -1;
            }
            
            // Write embedded audio to temp file for SDL_mixer
            AudioEntry* entry = &sys->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];
            
            // Create temp file
            char tempPath[512];
            snprintf(tempPath, sizeof(tempPath), "/tmp/butterscotch_audio_%d.ogg", sound->audioFile);
            FILE* f = fopen(tempPath, "wb");
            if (f) {
                fwrite(entry->data, 1, entry->dataSize, f);
                fclose(f);
                
                Mix_Chunk* chunk = Mix_LoadWAV(tempPath);
                if (chunk) {
                    int loops = loop ? -1 : 0;
                    slot->channel = Mix_PlayChannel(-1, chunk, loops);
                    if (slot->channel < 0) {
                        Mix_FreeChunk(chunk);
                        fprintf(stderr, "Audio: Failed to play embedded sound '%s'\n", sound->name);
                        return -1;
                    }
                    
                    slot->chunk = chunk;
                    slot->ownsChunk = true;
                } else {
                    fprintf(stderr, "Audio: Failed to load embedded sound '%s' from temp file\n", sound->name);
                    return -1;
                }
            } else {
                fprintf(stderr, "Audio: Failed to create temp file for embedded sound '%s'\n", sound->name);
                return -1;
            }
            
            slot->music = nullptr;
            slot->isMusic = false;
            slot->soundIndex = soundIndex;
            slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
            slot->priority = priority;
            slot->targetGain = sound->volume;
            slot->currentGain = sound->volume;
            slot->fadeTimeRemaining = 0;
            slot->startGain = sound->volume;
            slot->active = true;
            return slot->instanceId;
        } else {
            // External audio: load from file
            char* path = resolveExternalPath(sys, sound);
            if (path == nullptr) {
                fprintf(stderr, "Audio: Could not resolve path for sound '%s'\n", sound->name);
                return -1;
            }
            
            Mix_Chunk* chunk = Mix_LoadWAV(path);
            free(path);
            
            if (!chunk) {
                fprintf(stderr, "Audio: Failed to load file for '%s': %s\n", sound->name, Mix_GetError());
                return -1;
            }
            
            int loops = loop ? -1 : 0;
            slot->channel = Mix_PlayChannel(-1, chunk, loops);
            if (slot->channel < 0) {
                Mix_FreeChunk(chunk);
                fprintf(stderr, "Audio: Failed to play '%s'\n", sound->name);
                return -1;
            }
            
            slot->chunk = chunk;
            slot->ownsChunk = true;
            slot->music = nullptr;
            slot->isMusic = false;
        }
    }

    // Apply properties
    float volume = isStream ? 1.0f : sound->volume;
    int vol = (int)(volume * MIX_MAX_VOLUME);
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
    if (vol < 0) vol = 0;
    if (!slot->isMusic && slot->channel >= 0) {
        Mix_Volume(slot->channel, vol);
    }

    // Set up instance tracking
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
    
    return slot->instanceId;
}

static void sdlMixStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            if (inst->isMusic) { Mix_HaltMusic(); }
            else if (inst->channel >= 0) { Mix_HaltChannel(inst->channel); }
            if (inst->music) { Mix_FreeMusic(inst->music); inst->music = nullptr; }
            if (inst->ownsChunk && inst->chunk) { Mix_FreeChunk(inst->chunk); inst->chunk = nullptr; }
            inst->active = false;
            inst->channel = -1;
            inst->isMusic = false;
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (inst->isMusic) { Mix_HaltMusic(); }
                else if (inst->channel >= 0) { Mix_HaltChannel(inst->channel); }
                if (inst->music) { Mix_FreeMusic(inst->music); inst->music = nullptr; }
                if (inst->ownsChunk && inst->chunk) { Mix_FreeChunk(inst->chunk); inst->chunk = nullptr; }
                inst->active = false;
                inst->channel = -1;
                inst->isMusic = false;
            }
        }
    }
}

static void sdlMixStopAll(AudioSystem* audio) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
    Mix_HaltMusic();
    
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (inst->active) {
            if (inst->channel >= 0) Mix_HaltChannel(inst->channel);
            if (inst->music) { Mix_FreeMusic(inst->music); inst->music = nullptr; }
            if (inst->ownsChunk && inst->chunk) { Mix_FreeChunk(inst->chunk); inst->chunk = nullptr; }
            inst->active = false;
            inst->channel = -1;
        }
    }
}

static bool sdlMixIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            if (inst->isMusic) return Mix_PlayingMusic();
            return inst->channel >= 0 && Mix_Playing(inst->channel);
        }
        return false;
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (inst->isMusic && Mix_PlayingMusic()) return true;
                if (inst->channel >= 0 && Mix_Playing(inst->channel)) return true;
            }
        }
        return false;
    }
}

static void sdlMixPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            if (inst->isMusic) { Mix_PauseMusic(); }
            else if (inst->channel >= 0) { Mix_Pause(inst->channel); }
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (inst->isMusic) { Mix_PauseMusic(); }
                else if (inst->channel >= 0) { Mix_Pause(inst->channel); }
            }
        }
    }
}

static void sdlMixResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            if (inst->isMusic) { Mix_ResumeMusic(); }
            else if (inst->channel >= 0) { Mix_Resume(inst->channel); }
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (inst->isMusic) { Mix_ResumeMusic(); }
                else if (inst->channel >= 0) { Mix_Resume(inst->channel); }
            }
        }
    }
}

static void sdlMixPauseAll(AudioSystem* audio) {
    Mix_Pause(-1);
}

static void sdlMixResumeAll(AudioSystem* audio) {
    Mix_Resume(-1);
}

static void sdlMixSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                int vol = (int)(gain * MIX_MAX_VOLUME);
                if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
                if (vol < 0) vol = 0;
                if (inst->channel >= 0) Mix_Volume(inst->channel, vol);
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
                    int vol = (int)(gain * MIX_MAX_VOLUME);
                    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
                    if (vol < 0) vol = 0;
                    if (inst->channel >= 0) Mix_Volume(inst->channel, vol);
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

static float sdlMixGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
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

static void sdlMixSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    // SDL_mixer 1.2 doesn't support pitch shifting natively
    // This is a no-op for now
    (void)audio;
    (void)soundOrInstance;
    (void)pitch;
}

static float sdlMixGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    // SDL_mixer 1.2 doesn't support pitch shifting
    (void)audio;
    (void)soundOrInstance;
    return 1.0f;
}

static float sdlMixGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr && inst->channel >= 0) {
            // SDL_mixer 1.2 doesn't have a direct way to get position for chunks
            // For music we could use Mix_GetMusicPosition but it's not available in 1.2
            return 0.0f;
        }
    } else {
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
            SoundInstance* inst = &sys->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return 0.0f;
            }
        }
    }
    return 0.0f;
}

static void sdlMixSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    // SDL_mixer 1.2 doesn't support seeking for chunks
    // Only works for music streams with limited support
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    (void)positionSeconds; // Unused due to API limitation
    
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(sys, soundOrInstance);
        if (inst != nullptr && inst->music) {
            // No direct seek API for music in SDL_mixer 1.2
            fprintf(stderr, "Audio: Seeking music not supported in SDL_mixer 1.2\n");
        }
    }
}

static void sdlMixSetMasterGain(AudioSystem* audio, float gain) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    sys->masterVolume = (int)(gain * MIX_MAX_VOLUME);
    if (sys->masterVolume > MIX_MAX_VOLUME) sys->masterVolume = MIX_MAX_VOLUME;
    if (sys->masterVolume < 0) sys->masterVolume = 0;
    Mix_VolumeMusic(sys->masterVolume);
}

static void sdlMixSetChannelCount(AudioSystem* audio, int32_t count) {
    // SDL_mixer handles channel allocation internally
    Mix_AllocateChannels(count > 0 ? count : 32);
}

static void sdlMixGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex > 0) {
        int sz = snprintf(nullptr, 0, "audiogroup%d.dat", groupIndex);
        char buf[sz + 1];
        snprintf(buf, sizeof(buf), "audiogroup%d.dat", groupIndex);
        DataWin* audioGroup = DataWin_parse(((SDLMixAudioSystem*)audio)->fileSystem->vtable->resolvePath(((SDLMixAudioSystem*)audio)->fileSystem, buf),
        (DataWinParserOptions) {
            .parseAudo = true,
        });
        arrput(audio->audioGroups, audioGroup);
    }
}

static bool sdlMixGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

// ===[ Audio Streams ]===

static int32_t sdlMixCreateStream(AudioSystem* audio, const char* filename) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
    // Find a free stream slot
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
    sys->streams[freeSlot].music = nullptr;
    
    int32_t streamIndex = AUDIO_STREAM_INDEX_BASE + freeSlot;
    fprintf(stderr, "Audio: Created stream %d for '%s' -> '%s'\n", streamIndex, filename, resolved);
    return streamIndex;
}

static bool sdlMixDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*) audio;
    
    int32_t slotIndex = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (slotIndex < 0 || slotIndex >= MAX_AUDIO_STREAMS) {
        fprintf(stderr, "Audio: Invalid stream index %d for destroy\n", streamIndex);
        return false;
    }
    
    AudioStreamEntry* entry = &sys->streams[slotIndex];
    if (!entry->active) return false;
    
    // Stop all sound instances that were playing this stream
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &sys->instances[i];
        if (inst->active && inst->soundIndex == streamIndex) {
            if (inst->isMusic) { Mix_HaltMusic(); }
            else if (inst->channel >= 0) { Mix_HaltChannel(inst->channel); }
            if (inst->music) { Mix_FreeMusic(inst->music); inst->music = nullptr; }
            if (inst->ownsChunk && inst->chunk) { Mix_FreeChunk(inst->chunk); inst->chunk = nullptr; }
            inst->active = false;
            inst->channel = -1;
            inst->isMusic = false;
        }
    }
    
    if (entry->music) {
        Mix_FreeMusic(entry->music);
        entry->music = nullptr;
    }
    free(entry->filePath);
    entry->filePath = nullptr;
    entry->active = false;
    fprintf(stderr, "Audio: Destroyed stream %d\n", streamIndex);
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable sdlMixAudioSystemVtable = {
    .init = sdlMixInit,
    .destroy = sdlMixDestroy,
    .update = sdlMixUpdate,
    .playSound = sdlMixPlaySound,
    .stopSound = sdlMixStopSound,
    .stopAll = sdlMixStopAll,
    .isPlaying = sdlMixIsPlaying,
    .pauseSound = sdlMixPauseSound,
    .resumeSound = sdlMixResumeSound,
    .pauseAll = sdlMixPauseAll,
    .resumeAll = sdlMixResumeAll,
    .setSoundGain = sdlMixSetSoundGain,
    .getSoundGain = sdlMixGetSoundGain,
    .setSoundPitch = sdlMixSetSoundPitch,
    .getSoundPitch = sdlMixGetSoundPitch,
    .getTrackPosition = sdlMixGetTrackPosition,
    .setTrackPosition = sdlMixSetTrackPosition,
    .setMasterGain = sdlMixSetMasterGain,
    .setChannelCount = sdlMixSetChannelCount,
    .groupLoad = sdlMixGroupLoad,
    .groupIsLoaded = sdlMixGroupIsLoaded,
    .createStream = sdlMixCreateStream,
    .destroyStream = sdlMixDestroyStream,
};

// ===[ Lifecycle ]===

SDLMixAudioSystem* SDLMixAudioSystem_create(void) {
    SDLMixAudioSystem* sys = (SDLMixAudioSystem*)safeCalloc(1, sizeof(SDLMixAudioSystem));
    sys->base.vtable = &sdlMixAudioSystemVtable;
    sys->initialized = false;
    sys->masterVolume = MIX_MAX_VOLUME;
    return sys;
}
