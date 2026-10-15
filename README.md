# FNAFSBCraft

Play all of FNAF: Security Breach as Steve. A Minecraft client runs next to the game, and a UE4SS
mod links the two through shared memory: Minecraft's physics, controls, HUD, blocks and mobs, inside
the Pizzaplex.

This is a fork of [SkyCraft](https://github.com/chasmlol/SkyCraft) (Skyrim + Minecraft) with the
Skyrim half swapped for a Security Breach mod. Internally the Minecraft mod is still called
`skycraft`.

> **Status: 1.0.0-beta.** A lot works, much of it is new and only lightly tested: expect bugs, and
> please report them with your `UE4SS.log`.
> This is a fan project. It isn't affiliated with Mojang, Microsoft, Steel Wool Studios or
> ScottGames, and you need to own both games.

## Features

### Playing as Steve

- **Minecraft mode is on by default** once the game and Minecraft are ready. F6 switches between
  Steve and Gregory, F7 turns Minecraft mode off until the next F6.
- Minecraft's own movement and physics: walking, sprinting, jumping, sneaking, swimming, and flying
  in creative. The Pizzaplex's walls and floors are scanned around you and sent to Minecraft as
  collision, at an eighth of a block.
- Minecraft's HUD, hand, inventory and screens are drawn over the game. Minecraft's blocks, mobs,
  items, arrows and the block outline are drawn into the game's world, hidden behind its walls.
- F5 third person, night vision, and the spyglass zoom the game's camera.
- Torches, lava, glowstone and other Minecraft lights light up the game's world.
- Footsteps on the game's floor. The animatronics hear them; sneaking is silent.
- **Escape** opens a Minecraft-style pause menu that really pauses the game: Back to Game,
  Advancements, Statistics, Minecraft Options, Game Options (the game's own menu), Quit to Title,
  Quit Game.
- What the characters say (the game's subtitles) appears in Minecraft's chat, like
  `<Chica> Employees only.`
- "Gregory joined the game" and "Moon joined the game", in yellow, when the cast shows up.
- **Vents:** hold Shift in front of one and walk in: Steve crawls and fits.
- **Gregory's Sneakers:** boots in your starting kit that make Steve as fast as Gregory.
- **One Minecraft world per save:** each Security Breach save has its own blocks and inventory.
  Saving into another slot takes your world along.
- The game's own scenes (cutscenes, Freddy carrying Gregory, the boot screen) take over the
  controls; Steve comes back when they're done. **Tab** opens the Fazwatch.

### The Pizzaplex

- **Right-click** uses what Gregory would use: doors, buttons, save stations, collectibles. Doors
  and buttons the game has locked stay locked.
- **Hiding spots** work: you sit inside, and Shift gets you out again.
- **Break things:** cardboard cutouts, doors, fences, statues, furniture and small props break
  (one hit in creative, several in survival). They drop a piece of paper with their name;
  right-click it to put the object back. TNT breaks them too. The building itself never breaks.
- **Hit the animatronics:** a blow stuns them and knocks them back. Bots slide away, loose objects
  fly.
- **Chests** sometimes appear the first time you enter a room, with supplies and fun items.
- **Steve's Survival Guide** and a starting kit when you first join (refilled if you spawn with an
  empty inventory).
- **Achievements:** Security Breach's Steam achievements show up as Minecraft advancements, in
  their own tab. The mod's tricks have a tab of their own too.

### Creative

- A **Security Breach** creative tab with a spawn egg and a bucket for every character, plus the
  mod's tools.
- **Middle-click** an animatronic for its spawn egg, or a breakable object for its paper (copies).
  Hold the middle button to pick it up and carry it.

### Commands

| Command | |
|---|---|
| `/freddy`, `/tp freddy` | Brings that character to you |
| `/summon freddy` | A new one (it has to have been met once) |
| `/chica jockey`, `/summon chica jockey` | The same, riding a chicken |
| `/tp atrium` | To an area: `kitchen`, `atrium`, `east_arcade`, `west_arcade`, `roxy_raceway`, `gator_golf`, `daycare`, `utilidoors`, `pizza_sim`, `fazer_blast` |
| `/repairworld` | Closes every hole dug into the Pizzaplex near you |
| `/clearworld confirm` | Removes all Minecraft blocks and entities near you (not your inventory) |
| `/experiment enable block_breaking` | Turns on an experiment (see below) |

Characters: `freddy`, `chica`, `monty`, `roxy`, `vanny`, `vanessa`, `moon`, `sun`, `bot`, `endo`,
`musicman`.

### Gamerules

| Gamerule | Default | |
|---|---|---|
| `allow_cheating` | false | Off: survival only, and what the game says can't be used now can't be used. On: creative, spectator, locked doors and switched-off buttons. |
| `allow_opening_doors` | true | Steve can open (and break) the game's doors |
| `allow_animatronic_water_bucket` | true | A water bucket can scoop up an animatronic |
| `experiment_block_breaking` | false | Set with `/experiment` |

Minecraft gamerules that do nothing in the Pizzaplex (raids, phantoms, weather, ...) are hidden.

### Experiments (off by default, buggy)

- **block_breaking:** a pickaxe or shovel digs into the Pizzaplex itself, a block at a time. You
  drop into the hole, and through thin floors into the room below. The **Void Filler** closes a
  hole again.

## Spoilers: secret features

<details>
<summary><b>Spoilers ahead.</b> Don't open this if you want to find them yourself. The books in the loot chests hint at most of them.</summary>

<br>

**Animatronics**

- **Ride Freddy:** right-click him. To the game, Gregory is inside Freddy: charging stations work.
  Press E while riding for his saddle and chest (it keeps its contents). Put an elytra on him with
  a dispenser.
- **Pumpkin:** give an animatronic a carved pumpkin (right-click, or drop it at its feet): it can't
  see and stumbles around like a headless chicken. Shears take things off again.
- **Ice:** ice or frost walker boots, or standing on an ice block, make the floor slippery for it.
  You can shove it away.
- **Slime or honey** slows it down to a crawl. Honey blocks do the same.
- **Ender pearl:** it teleports like an enderman. Freddy just wanders.
- **Wings:** drop an elytra at its feet. Most of them hunt you from the air like phantoms; Freddy
  floats along like an allay.
- **Lead:** animatronics go on a lead, and you can tie them to a fence.
- **Water bucket:** scoop one up, and put it back wherever you like.
- **Chicken jockeys:** sometimes an animatronic rides a chicken. Or force it: `/chica jockey`.
- **Dinnerbone** or **Grumm** on a name tag turns one upside down. **jeb_** makes it a rainbow.
- **Anvil:** drop one on an animatronic and it gets squashed flat.
- **Shield:** raise it and animatronics bounce off; lasers stop at it (and at Minecraft blocks).
- **Iron golems** hate animatronics. Animatronics hurt villagers.
- Water, bubble columns, slime blocks, lava and fire move or launch them; boats and minecarts take
  them for a ride.
- **Sneaking** and wearing a carved pumpkin make the animatronics see you less far.

**The end portal**

- Twelve end portal frames are hidden around the Pizzaplex. Find nine of them (click one to pick it
  up; you can't drop them), then put them down: the whole end portal builds itself.
- Step in for a new ending.

</details>

## Installing

You need **FNAF: Security Breach** on Steam (Windows) and **Minecraft: Java Edition** on a Microsoft
account. You don't install Minecraft yourself: the mod brings its own launcher (Prism Launcher),
which downloads Minecraft and Java the first time.

1. **Download** `FNAFSBCraft-1.0.0-beta.zip` from the
   [Releases](https://github.com/Mielesgames/securitybreach-minecraft-passthrough/releases) page.
2. **Find the game's folder:** in Steam, right-click Five Nights at Freddy's: Security Breach →
   Manage → Browse local files. That opens the `Quarters` folder.
3. **Unpack the zip into that folder.** It adds files to `fnaf9\Binaries\Win64` (UE4SS and the
   mod). Say yes if Windows asks to merge folders.
4. **Launch options:** in Steam, right-click the game → Properties → General → Launch Options,
   and type `-dx11`. Without it (DirectX 12) the mod can't draw anything.
5. **Start Security Breach** from Steam. The first time:
   - Prism Launcher opens and asks you to sign in with the Microsoft account that owns Minecraft.
     Alt-tab to it if you don't see it.
   - It then downloads Minecraft and Java. That takes a few minutes; Minecraft runs in the
     background, you don't see its window.
6. **Load a save** (or start a new game). You become Steve as soon as both are ready. F6 switches
   between Steve and Gregory.

Already using UE4SS with other mods? This release brings its own `UE4SS.dll` (the mod needs that
build); keep your own `Mods` folder, the zip only adds `Mods\SecurityBreachMinecraftPassthrough`.

**Which Minecraft:** `Mods\SecurityBreachMinecraftPassthrough\FNAFSBCraft.ini` says `Minecraft = prism`
(the bundled launcher with your own account, so your skin and cape). Developers can switch it to
`Minecraft = source` (see Building). Starting the FNAFSBCraft instance in Prism on its own starts
Security Breach through Steam.

**To uninstall:** delete `dwmapi.dll`, `UE4SS.dll`, `UE4SS-settings.ini` and
`Mods\SecurityBreachMinecraftPassthrough` from `fnaf9\Binaries\Win64`. Minecraft's files are in
`%LOCALAPPDATA%\FNAFSBCraft`.

**If something goes wrong:** `UE4SS.log` in `fnaf9\Binaries\Win64` shows what the mod does (lines
starting with `SBMP`). Minecraft's log is in
`%LOCALAPPDATA%\FNAFSBCraft\Prism\instances\FNAFSBCraft\.minecraft\logs\latest.log`.

## Building

You need Visual Studio 2026 (C++), CMake, Git, Rust (UE4SS needs it) and JDK 25.

```bat
git clone --recursive https://github.com/Mielesgames/securitybreach-minecraft-passthrough
cd securitybreach-minecraft-passthrough
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Game__Shipping__Win64 --target SecurityBreachMinecraftPassthrough
```

The first build also builds UE4SS itself, which takes a while.

**Running a build from source:** install the `UE4SS.dll` from `build\Game__Shipping__Win64\bin`
(with UE4SS 3.0.1's `dwmapi.dll`) and copy
`build\sb\Game__Shipping__Win64\SecurityBreachMinecraftPassthrough.dll` to
`fnaf9\Binaries\Win64\Mods\SecurityBreachMinecraftPassthrough\dlls\main.dll`, with an empty
`enabled.txt` next to `dlls`. Set the environment variable `SBMP_FABRIC_DIR` to this repository's
`fabric` folder: the mod then starts Minecraft from there with `gradlew runClient` (JDK 25) instead
of the bundled launcher. Its log is `fabric\run\logs\latest.log`.

**Making a release:** `powershell -ExecutionPolicy Bypass -File tools\package-sb.ps1` builds both
halves and writes the zips to `dist\` (the release zip, its debug symbols, and the Minecraft mod's
jar on its own).

## Known issues

- Only one Freddy can be seen at a time; the game seems to allow just one.
- Flying very fast into a new area holds you back for a moment while its walls and floors are
  scanned.
- DirectX 12 isn't supported (use `-dx11`).
- Minecraft runs with Security Breach and closes with it; starting Minecraft alone doesn't start the
  game.

## Layout

| Folder | |
|---|---|
| `sb/` | The Security Breach mod (C++, [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)) |
| `fabric/` | The Minecraft Fabric mod (Java, Minecraft 26.3), from SkyCraft plus a passthrough mode |
| `protocol/` | The shared-memory layout both sides follow |
| `skse/` | SkyCraft's original Skyrim plugin, kept as a reference. Not built here. |
| `tools/` | SkyCraft's packaging and test tools, and `crash_stack.py` for crash dumps |
| `docs/DESIGN.md` | How SkyCraft's two halves fit together (written for Skyrim) |

## Credits

SkyCraft is by chasmlol. FNAFSBCraft and the new ending's art are by Mielesgames.

## License

[MIT](LICENSE).
