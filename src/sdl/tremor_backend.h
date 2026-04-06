#pragma once

#include "miniaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the custom Tremor decoding backend vtable for miniaudio.
// Use this in ma_decoder_config.ppCustomBackendVTables to enable OGG decoding
// via Tremor (libvorbisidec) — integer-based, fast, no FPU needed.
const ma_decoding_backend_vtable* getTremorBackendVTable(void);

#ifdef __cplusplus
}
#endif
