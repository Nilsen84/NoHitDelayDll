# NoHitDelayDll

Removes the hit delay (the cooldown after a missed left-click) in Minecraft 1.8.9. Inject the DLL into a running game.

Supports vanilla, MCP/Forge and Legacy Fabric mappings.

## Building

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build
```

Requires a JDK for the JNI headers
