#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"
#include "utils.h"

// ===[ RValue - Tagged Union ]===
typedef enum {
    RVALUE_REAL = 0,
    RVALUE_STRING = 1,
    RVALUE_INT32 = 2,
    RVALUE_INT64 = 3,
    RVALUE_BOOL = 4,
    RVALUE_UNDEFINED = 5,
} RValueType;

typedef struct {
    union {
        double real;
        int32_t int32;
        int64_t int64;
        const char* string;
    };
    RValueType type;
    bool ownsString;
} RValue;

static RValue RValue_makeReal(double val) {
    return (RValue){ .real = val, .type = RVALUE_REAL };
}

static RValue RValue_makeInt32(int32_t val) {
    return (RValue){ .int32 = val, .type = RVALUE_INT32 };
}

static RValue RValue_makeInt64(int64_t val) {
    return (RValue){ .int64 = val, .type = RVALUE_INT64 };
}

static RValue RValue_makeBool(bool val) {
    return (RValue){ .int32 = val ? 1 : 0, .type = RVALUE_BOOL };
}

static RValue RValue_makeString(const char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = false };
}

static RValue RValue_makeOwnedString(char* val) {
    return (RValue){ .string = val, .type = RVALUE_STRING, .ownsString = true };
}

static RValue RValue_makeUndefined(void) {
    return (RValue){ .type = RVALUE_UNDEFINED };
}

// Converts an RValue to a heap-allocated string representation.
// The caller must free the returned string
static char* RValue_toString(RValue val) {
    char buf[64];
    switch (val.type) {
        case RVALUE_REAL:
            snprintf(buf, sizeof(buf), "%.16g", val.real);
            return strdup(buf);
        case RVALUE_INT32:
            snprintf(buf, sizeof(buf), "%d", val.int32);
            return strdup(buf);
        case RVALUE_INT64:
            snprintf(buf, sizeof(buf), "%lld", (long long) val.int64);
            return strdup(buf);
        case RVALUE_STRING:
            return strdup(val.string != nullptr ? val.string : "");
        case RVALUE_BOOL:
            return strdup(val.int32 ? "1" : "0");
        case RVALUE_UNDEFINED:
            return strdup("undefined");
    }
    return strdup("");
}

static void RValue_free(RValue* val) {
    if (val->type == RVALUE_STRING && val->ownsString && val->string != nullptr) {
        free((void*) val->string);
        val->string = nullptr;
        val->ownsString = false;
    }
}

static double RValue_toReal(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return val.real;
        case RVALUE_INT32:  return (double) val.int32;
        case RVALUE_INT64:  return (double) val.int64;
        case RVALUE_BOOL:   return (double) val.int32;
        case RVALUE_STRING: return strtod(val.string, nullptr);
        default:            return 0.0;
    }
}

static int32_t RValue_toInt32(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return (int32_t) val.real;
        case RVALUE_INT32:  return val.int32;
        case RVALUE_INT64:  return (int32_t) val.int64;
        case RVALUE_BOOL:   return val.int32;
        case RVALUE_STRING: return (int32_t) strtod(val.string, nullptr);
        default:            return 0;
    }
}

static int64_t RValue_toInt64(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return (int64_t) val.real;
        case RVALUE_INT32:  return (int64_t) val.int32;
        case RVALUE_INT64:  return val.int64;
        case RVALUE_BOOL:   return (int64_t) val.int32;
        case RVALUE_STRING: return (int64_t) strtod(val.string, nullptr);
        default:            return 0;
    }
}

static bool RValue_toBool(RValue val) {
    switch (val.type) {
        case RVALUE_REAL:   return val.real > 0.5;
        case RVALUE_INT32:  return val.int32 > 0;
        case RVALUE_INT64:  return val.int64 > 0;
        case RVALUE_BOOL:   return val.int32 != 0;
        case RVALUE_STRING: return val.string != nullptr && val.string[0] != '\0';
        default:            return false;
    }
}

// ===[ ArrayMapEntry - used by all array variable storage ]===
typedef struct {
    int64_t key;
    RValue value;
} ArrayMapEntry;

static void RValue_freeAllRValuesInMap(ArrayMapEntry* map) {
    repeat(hmlen(map), i) {
        RValue_free(&map[i].value);
    }
}