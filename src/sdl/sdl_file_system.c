#include "sdl_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

// ===[ Vtable Implementations ]===

static char* sdlResolvePath(FileSystem* fs, const char* relativePath) {
    SdlFileSystem* sdlFs = (SdlFileSystem*)fs;
    
    if (!relativePath || relativePath[0] == '\0') {
        return NULL;
    }
    
    // If path is absolute, return as-is
    if (relativePath[0] == '/') {
        return strdup(relativePath);
    }
    
    // If basePath is set, prepend it
    if (sdlFs->basePath != NULL) {
        char fullPath[1024];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", sdlFs->basePath, relativePath);
        return strdup(fullPath);
    }
    
    // Otherwise return as-is (relative to current directory)
    return strdup(relativePath);
}

static bool sdlFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = sdlResolvePath(fs, relativePath);
    if (!fullPath) return false;
    
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

static char* sdlReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = sdlResolvePath(fs, relativePath);
    if (!fullPath) return NULL;
    
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);
    
    return buffer;
}

static bool sdlWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = sdlResolvePath(fs, relativePath);
    if (!fullPath) return false;
    
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    
    if (!f) return false;
    
    fputs(contents, f);
    fclose(f);
    return true;
}

static bool sdlDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = sdlResolvePath(fs, relativePath);
    if (!fullPath) return false;
    
    bool result = (remove(fullPath) == 0);
    free(fullPath);
    return result;
}

static FileSystemVtable sdlVtable = {
    .resolvePath = sdlResolvePath,
    .fileExists = sdlFileExists,
    .readFileText = sdlReadFileText,
    .writeFileText = sdlWriteFileText,
    .deleteFile = sdlDeleteFile,
};

// ===[ Public API ]===

FileSystem* SdlFileSystem_create(const char* dataWinPath) {
    SdlFileSystem* fs = safeCalloc(1, sizeof(SdlFileSystem));
    
    // Extract base directory from data.win path
    if (dataWinPath != NULL) {
        char* pathCopy = strdup(dataWinPath);
        char* dir = dirname(pathCopy);
        fs->basePath = strdup(dir);
        free(pathCopy);
        
        fprintf(stderr, "FileSystem: Base path set to '%s'\n", fs->basePath);
    }
    
    fs->base.vtable = &sdlVtable;
    return &fs->base;
}

void SdlFileSystem_destroy(FileSystem* fs) {
    if (!fs) return;
    
    SdlFileSystem* sdlFs = (SdlFileSystem*)fs;
    if (sdlFs->basePath != NULL) {
        free(sdlFs->basePath);
    }
    free(fs);
}
