#pragma once

#include "file_system.h"

typedef struct {
    FileSystem base;
    char* basePath;  // Base directory (where data.win is located)
} SdlFileSystem;

FileSystem* SdlFileSystem_create(const char* dataWinPath);
void SdlFileSystem_destroy(FileSystem* fs);
