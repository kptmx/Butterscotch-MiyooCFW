// Tremor (libvorbisidec) custom decoding backend for miniaudio
// Provides OGG Vorbis decoding via integer arithmetic — fast on embedded ARM

#include "tremor_backend.h"
#include <ivorbisfile.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

// ===[ Memory stream for reading OGG from data.win buffer ]===

typedef struct {
    const uint8_t* data;
    size_t         size;
    size_t         pos;
} MemStream;

static size_t mem_read(void* ptr, size_t size, size_t nmemb, void* ds) {
    MemStream* ms = (MemStream*)ds;
    size_t bytes = size * nmemb;
    size_t avail = ms->size - ms->pos;
    if (bytes > avail) bytes = avail;
    if (bytes == 0) return 0;
    memcpy(ptr, ms->data + ms->pos, bytes);
    ms->pos += bytes;
    return bytes / size;
}

static int mem_seek(void* ds, ogg_int64_t off, int whence) {
    MemStream* ms = (MemStream*)ds;
    ogg_int64_t newpos;
    switch (whence) {
        case SEEK_SET: newpos = off; break;
        case SEEK_CUR: newpos = (ogg_int64_t)ms->pos + off; break;
        case SEEK_END: newpos = (ogg_int64_t)ms->size + off; break;
        default: return -1;
    }
    if (newpos < 0 || (size_t)newpos > ms->size) return -1;
    ms->pos = (size_t)newpos;
    return 0;
}

static long mem_tell(void* ds) {
    return (long)((MemStream*)ds)->pos;
}

static const ov_callbacks MEM_CALLBACKS = { mem_read, mem_seek, NULL, mem_tell };

// ===[ Decoder instance ]===

typedef struct {
    ma_data_source_base base;   // must be first — miniaudio requires this
    OggVorbis_File      vf;
    MemStream           ms;     // only used for memory-based (embedded) audio
    bool                fromMem;
    ma_uint32           channels;
    ma_uint32           sampleRate;
} MaTremor;

// ===[ ma_data_source vtable implementation ]===

static ma_result tremor_read(ma_data_source* ds, void* out,
                              ma_uint64 frameCount, ma_uint64* pRead) {
    MaTremor* t = (MaTremor*)ds;
    int16_t* buf = (int16_t*)out;
    int bpf = (int)(sizeof(int16_t) * t->channels);
    ma_uint64 total = 0;

    while (total < frameCount) {
        int bs = 0;
        // Tremor returns int16 directly — no float conversion needed
        long got = ov_read(
            &t->vf,
            (char*)(buf + total * t->channels),
            (int)((frameCount - total) * bpf),
            &bs
        );
        if (got <= 0) break;
        total += (ma_uint64)(got / bpf);
    }

    if (pRead) *pRead = total;
    return (total > 0) ? MA_SUCCESS : MA_AT_END;
}

static ma_result tremor_seek(ma_data_source* ds, ma_uint64 frame) {
    MaTremor* t = (MaTremor*)ds;
    return ov_pcm_seek(&t->vf, (ogg_int64_t)frame) == 0
           ? MA_SUCCESS : MA_ERROR;
}

static ma_result tremor_get_format(ma_data_source* ds, ma_format* fmt,
                                    ma_uint32* ch, ma_uint32* sr,
                                    ma_channel* map, size_t mapCap) {
    MaTremor* t = (MaTremor*)ds;
    (void)map;
    (void)mapCap;
    if (fmt) *fmt = ma_format_s16;  // Tremor always outputs int16
    if (ch)  *ch  = t->channels;
    if (sr)  *sr  = t->sampleRate;
    return MA_SUCCESS;
}

static ma_result tremor_get_cursor(ma_data_source* ds, ma_uint64* cursor) {
    MaTremor* t = (MaTremor*)ds;
    *cursor = (ma_uint64)ov_pcm_tell(&t->vf);
    return MA_SUCCESS;
}

static ma_result tremor_get_length(ma_data_source* ds, ma_uint64* length) {
    MaTremor* t = (MaTremor*)ds;
    ogg_int64_t len = ov_pcm_total(&t->vf, -1);
    if (len < 0) return MA_ERROR;
    *length = (ma_uint64)len;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_tremorDsVTable = {
    tremor_read,
    tremor_seek,
    tremor_get_format,
    tremor_get_cursor,
    tremor_get_length,
    NULL,  // setLooping — miniaudio manages looping itself
    0
};

// ===[ Common init after ov_open* ]===

static ma_result tremor_init_common(MaTremor* t,
                                     const ma_allocation_callbacks* cb) {
    vorbis_info* vi = ov_info(&t->vf, -1);
    if (!vi) {
        ov_clear(&t->vf);
        return MA_ERROR;
    }

    t->channels   = (ma_uint32)vi->channels;
    t->sampleRate = (ma_uint32)vi->rate;

    ma_data_source_config dscfg = ma_data_source_config_init();
    dscfg.vtable = &g_tremorDsVTable;
    ma_result res = ma_data_source_init(&dscfg, &t->base);
    if (res != MA_SUCCESS) ov_clear(&t->vf);
    return res;
}

// ===[ Backend vtable ]===

static ma_result backend_init_memory(void* pUserData,
                                      const void* data, size_t sz,
                                      const ma_decoding_backend_config* cfg,
                                      const ma_allocation_callbacks* cb,
                                      ma_data_source** ppOut) {
    (void)pUserData;
    (void)cfg;
    MaTremor* t = (MaTremor*)ma_malloc(sizeof(MaTremor), cb);
    if (!t) return MA_OUT_OF_MEMORY;
    memset(t, 0, sizeof(*t));

    // Point directly to data.win buffer — no copying
    t->ms.data = (const uint8_t*)data;
    t->ms.size = sz;
    t->ms.pos  = 0;
    t->fromMem = true;

    if (ov_open_callbacks(&t->ms, &t->vf, NULL, 0, MEM_CALLBACKS) < 0) {
        ma_free(t, cb);
        return MA_INVALID_FILE;
    }

    ma_result res = tremor_init_common(t, cb);
    if (res != MA_SUCCESS) { ma_free(t, cb); return res; }

    *ppOut = (ma_data_source*)t;
    return MA_SUCCESS;
}

static ma_result backend_init_file(void* pUserData,
                                    const char* path,
                                    const ma_decoding_backend_config* cfg,
                                    const ma_allocation_callbacks* cb,
                                    ma_data_source** ppOut) {
    (void)pUserData;
    (void)cfg;
    MaTremor* t = (MaTremor*)ma_malloc(sizeof(MaTremor), cb);
    if (!t) return MA_OUT_OF_MEMORY;
    memset(t, 0, sizeof(*t));
    t->fromMem = false;

    FILE* f = fopen(path, "rb");
    if (!f) { ma_free(t, cb); return MA_ERROR; }

    // ov_open takes ownership of FILE* and closes it in ov_clear
    if (ov_open(f, &t->vf, NULL, 0) < 0) {
        fclose(f);
        ma_free(t, cb);
        return MA_INVALID_FILE;
    }

    ma_result res = tremor_init_common(t, cb);
    if (res != MA_SUCCESS) { ma_free(t, cb); return res; }

    *ppOut = (ma_data_source*)t;
    return MA_SUCCESS;
}

static void backend_uninit(void* pUserData,
                            ma_data_source* ds,
                            const ma_allocation_callbacks* cb) {
    (void)pUserData;
    MaTremor* t = (MaTremor*)ds;
    ma_data_source_uninit(&t->base);
    ov_clear(&t->vf);  // closes FILE* for files, for memory just cleans up
    ma_free(t, cb);
    // entry->data in data.win is not ours — do not touch
}

static ma_decoding_backend_vtable g_tremorBackend = {
    NULL,                  // onInit
    backend_init_file,     // onInitFile
    NULL,                  // onInitFileW
    backend_init_memory,   // onInitMemory
    backend_uninit,        // onUninit
};

const ma_decoding_backend_vtable* getTremorBackendVTable(void) {
    return &g_tremorBackend;
}
