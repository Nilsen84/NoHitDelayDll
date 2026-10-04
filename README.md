# NoHitDelayDll

Removes the hit delay (the cooldown after a missed left-click) in Minecraft 1.8.9.

Supports Windows, macOS and Linux with vanilla, MCP/Forge and Legacy Fabric mappings.

## Building

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build
```

Requires a C++20 compiler and a JDK for the JNI headers.

## Loading

Windows: inject the DLL into a running game.

macOS/Linux: load the shared library after Minecraft has loaded.
Initialization starts automatically on a worker thread.
Keep the library loaded for the lifetime of the process.

On macOS/Linux, run `build/inject.sh [pid]`; omit the PID to select a Java process.
Linux requires LLDB and may require `sudo` depending on ptrace restrictions.
