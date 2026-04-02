# SDL 1.2 Graphics Backend for Butterscotch

## Overview

This is a software rendering graphics backend for Butterscotch using SDL 1.2. It provides:

- **Software rendering** (no GPU required) using SDL 1.2's surface blitting
- **320x240 native resolution** (for embedded devices with /dev/fb0)
- **Optional scaling** (640x480) for PC testing
- **Cross-platform support** (Windows, Linux, macOS)

## Building

### Prerequisites

On Linux:
```bash
sudo apt-get install libsdl1.2-dev
```

On macOS:
```bash
brew install sdl
```

On Windows (MINGW with pkg-config):
Ensure SDL 1.2 development libraries are installed.

### Build Commands

```bash
# Create build directory
mkdir -p build-sdl
cd build-sdl

# Configure with SDL platform
cmake -DPLATFORM=sdl ..

# Build
make -j4
```

The executable will be at `./butterscotch`

## Running

```bash
./butterscotch /path/to/data.win
```

### Command-line Options

The SDL backend supports most of the same options as GLFW:

```bash
./butterscotch --help  # (not implemented, but options shown below)
```

Available options:
- `--headless` - Run without displaying a window (for batch processing)
- `--debug` - Enable debug mode with frame stepping (P to pause, O to step)
- `--seed=N` - Use fixed RNG seed
- `--speed=X` - Run at X times normal speed
- `--exit-at-frame=N` - Exit after N frames
- `--dump-frame=N` - Dump runner state at frame N
- `--dump-frame-json=N` - Dump JSON state at frame N
- `--dump-frame-json-file=PATTERN` - Pattern for JSON dump filenames
- Various trace options for debugging (`--trace-variable-reads`, etc.)

## Architecture

### Files

- **sdl_renderer.h/c** - Software renderer implementation
  - Manages backbuffer (dynamic size based on game resolution)
  - Loads TXTR pages from data.win as SDL_Surface
  - Implements sprite, primitive, and text drawing
  - Scales backbuffer to 320x240 window using SDL_SoftStretch

- **sdl_file_system.h/c** - File system abstraction
  - Resolves game-relative paths to absolute paths
  - Implements file I/O interface expected by Butterscotch

- **main.c** - Entry point and main loop
  - SDL event handling
  - Keyboard input mapping
  - Frame rate limiting
  - Game state management

### Display Resolution

The backend uses **320x240 window resolution**, which is:
- Native resolution for embedded devices with /dev/fb0 framebuffer
- Backbuffer is dynamically sized to match game resolution (from room views/ports)
- Backbuffer is scaled to fit the 320x240 window using SDL_SoftStretch

## Limitations

### Current Implementation

- **No PNG decoding** - Placeholder surfaces created instead. To use actual textures, integrate stbi_image_read or similar
- **No scaling** - Sprites are blitted at 1:1 with integer positioning
- **No rotation** - Only translation and basic scaling supported
- **Basic text rendering** - Text rendering is a no-op stub
- **No alpha blending** - Sprites drawn with full opacity
- **Audio disabled** - Uses noop audio system (can be extended with SDL_mixer)

### To Improve

1. **Add PNG Decoding**:
   ```c
   // In sdl_renderer.c SDLRenderer_init()
   #include "stb_image.h"
   
   // Decode TXTR blob data
   uint8_t* pixels = stbi_load_from_memory(pngData, pngSize, &w, &h, &channels, 4);
   sdl->spriteSurfaces[i] = SDL_CreateRGBSurfaceFrom(...);
   ```

2. **Add Audio Support**:
   ```bash
   # Install SDL_mixer
   sudo apt-get install libsdl-mixer1.2-dev
   ```
   Then create a sdl_audio_system component

3. **Add Font Rendering**:
   - Use SDL_ttf for TrueType font support
   - Or implement pixel font blitting

4. **Optimize Rendering**:
   - Cache scaled surfaces for common scales
   - Use hardware acceleration where available (SDL 1.2 has limited HW support)

## Framebuffer Support (/dev/fb0)

To run on embedded devices with framebuffer:

1. Set `__EMBEDDED__` define during compilation:
   ```bash
   cmake -DPLATFORM=sdl -DCMAKE_C_FLAGS="-D__EMBEDDED__" ..
   ```

2. Use SDL 1.2 with framebuffer driver:
   ```bash
   SDL_VIDEODRIVER=fbcon ./butterscotch /path/to/data.win
   ```

3. Or modify main.c to directly write to `/dev/fb0` for lower-level control.

## Comparison with GLFW Backend

| Feature | SDL 1.2 | GLFW (GL) |
|---------|---------|-----------|
| Rendering | Software (CPU) | Hardware (GPU) |
| Resolution | 320x240 fixed | Flexible |
| Performance | Slower | Faster |
| Dependencies | SDL 1.2 | GLFW3, OpenGL, GLAD |
| Embedded Devices | Better (framebuffer) | Requires GPU/driver |
| Text Rendering | Stub | Stub |
| Audio | Disabled | miniaudio |

## Debugging

Enable SDL debug logging:
```bash
SDL_DEBUG=1 ./butterscotch /path/to/data.win
```

Check compilation:
```bash
cd build-sdl && make VERBOSE=1
```

## Future Enhancements

- [ ] PNG texture decoding  from TXTR blobs
- [ ] SDL_mixer audio system
- [ ] Font rendering (SDL_ttf)
- [ ] Scaling/interpolation for better image quality
- [ ] Partial alpha/transparency support
- [ ] Direct framebuffer backend (bypass SDL for lower latency)
- [ ] Performance optimization (dirty rect tracking, surface caching)

## License

Same as Butterscotch project
