#pragma once

#include "audio_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SOUND_INSTANCES 128
#define SOUND_INSTANCE_ID_BASE 100000
#define MAX_AUDIO_STREAMS 32
#define MAX_CACHED_SOUNDS 128
#define AUDIO_STREAM_INDEX_BASE 300000

typedef struct MaTremorAudioSystem MaTremorAudioSystem;

MaTremorAudioSystem* MaTremorAudioSystem_create(void);

#ifdef __cplusplus
}
#endif
