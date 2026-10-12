# Security Breach Minecraft Passthrough

Walk through FNAF: Security Breach with Minecraft's movement. A Minecraft client runs next to the
game, and a UE4SS mod links the two through shared memory: your keys go to Minecraft, and
Minecraft's player position moves Gregory.

This is a fork of [SkyCraft](https://github.com/chasmlol/SkyCraft) (Skyrim + Minecraft) with the
Skyrim half swapped for a Security Breach mod. The Minecraft half and the shared-memory protocol
are SkyCraft's.

> **Status: work in progress, not playable yet.** Only the link and basic walking exist. Minecraft
> is not drawn inside Security Breach, and there is no release build.
> This is a fan project. It isn't affiliated with Mojang, Microsoft, Steel Wool Studios or
> ScottGames, and you need to own both games.

## What works

- The mod loads in Security Breach, finds Gregory and starts Minecraft by itself.
- Gregory's position and view direction are sent to Minecraft.
- With Minecraft mode on (F6), W/A/S/D, Space, Shift and Ctrl go to Minecraft, the game's own
  walking is switched off, and Minecraft's horizontal position is applied to Gregory. Height stays
  Security Breach's, so Gregory keeps standing on the real floor.
- **Walls (experimental):** Gregory is swept against Security Breach's geometry, steps up stairs
  and slides along walls. Minecraft doesn't know about the wall, so it is put back on Gregory
  when it walks into one, which makes sliding along walls jerky.

## What doesn't work yet

- **Collision in Minecraft:** Security Breach's geometry isn't sent to Minecraft. Minecraft runs in
  passthrough mode: no ground, height taken from Gregory.
- **Rendering:** Minecraft's blocks, HUD and inventory aren't drawn in Security Breach. Minecraft
  runs in its own window.
- Everything else SkyCraft does in Skyrim (digging, combat, water, multiplayer).

## Controls

| Key | |
|---|---|
| F6 | Minecraft mode on/off |
| F7 | Emergency off |

Minecraft mode also turns itself off when Minecraft stops producing frames for three seconds.

## Building

You need Visual Studio 2026 (C++), CMake, Git, Rust (UE4SS needs it) and JDK 25.

```bat
git clone --recursive https://github.com/Mielesgames/securitybreach-minecraft-passthrough
cd securitybreach-minecraft-passthrough
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Game__Shipping__Win64 --target SecurityBreachMinecraftPassthrough
```

The first build also builds UE4SS itself, which takes a while.

## Installing

1. Install UE4SS into `fnaf9\Binaries\Win64` of your Security Breach folder. Use the `UE4SS.dll`
   from this build (`build\Game__Shipping__Win64\bin`): the mod is built against it.
2. Copy `build\sb\Game__Shipping__Win64\SecurityBreachMinecraftPassthrough.dll` to
   `fnaf9\Binaries\Win64\Mods\SecurityBreachMinecraftPassthrough\dlls\main.dll`. The file has to
   be called `main.dll`.
3. Add `SecurityBreachMinecraftPassthrough : 1` to `Mods\mods.txt`.
4. Set the environment variable `SBMP_FABRIC_DIR` to this repository's `fabric` folder. The mod
   starts Minecraft from there with `gradlew runClient`.

Start Security Breach and load a save. Minecraft starts in the background (the first start takes
a few minutes), then F6 hands walking to Minecraft.

`UE4SS.log` next to the game's exe shows what the mod is doing: its lines start with `SBMP`.

## Layout

| Folder | |
|---|---|
| `sb/` | The Security Breach mod (C++, [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)) |
| `fabric/` | The Minecraft Fabric mod (Java, Minecraft 26.3), from SkyCraft plus a passthrough mode |
| `protocol/` | The shared-memory layout both sides follow |
| `skse/` | SkyCraft's original Skyrim plugin, kept as a reference. Not built here. |
| `tools/` | SkyCraft's packaging and test tools, and `crash_stack.py` for UE4SS crash dumps |
| `docs/DESIGN.md` | How SkyCraft's two halves fit together (written for Skyrim) |

## License

[MIT](LICENSE). SkyCraft is by chasmlol.
