<p align="center">
  <img src="docs/images/banner.png" alt="Crema, a Wii U emulator for PS4" width="100%">
</p>

<p align="center">
  <a href="https://github.com/iHaiDeeZ/Crema/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/iHaiDeeZ/Crema?label=release&color=9b7bff"></a>
  <img alt="PS4 homebrew" src="https://img.shields.io/badge/PS4-homebrew-1f5fd6">
  <img alt="Wii U" src="https://img.shields.io/badge/Wii%20U-Cemu-48e0ff">
  <a href="LICENSE"><img alt="MPL-2.0" src="https://img.shields.io/badge/license-MPL--2.0-ff8ad4"></a>
</p>

<h3 align="center">Your Wii U library, on the PS4.</h3>

<p align="center">
  <a href="https://github.com/iHaiDeeZ/Crema/releases/latest"><b>⬇ Get the latest version</b></a> ·
  <a href="#getting-started">Getting started</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#game-compatibility">Compatibility</a> ·
  <a href="FAQ.md">FAQ</a> ·
  <a href="CREDITS.md">Credits</a>
</p>

---

## Overview

Crema brings the [Cemu](https://cemu.info) Wii U emulator to jailbroken PS4 consoles as an app you
launch from the home screen like any other. You browse your games in a cover-flow library (or a
PSP-style XMB, if you prefer), press **Cross**, and play with a DualShock 4 standing in for the Wii U
GamePad: its touchpad is the GamePad's touch screen and its motion sensors are the GamePad's gyro.
Pressing **L3 + R3** during a game opens a menu to swap the TV and GamePad screens, change settings
or go back to the launcher.

Under the hood, Cemu's recompiler turns the Wii U's PowerPC code into x86-64 code for the PS4's CPU,
and rendering goes through Vulkan: Mesa's RADV driver, adapted to submit work directly to the PS4's
AMD GPU.

You bring your own game backups. Nothing copyrighted is bundled: no games, firmware or keys.

## Highlights

**A launcher made for the couch**
- Your games as a row of covers, with the selected one up front and its title and region.
  Tabs for Library, Settings and Themes switch with **L1 / R1**.
- **OPTIONS** sorts the games A-Z or by Recently Played; **Square** looks for new games.
- Prefer the PSP look? **Settings → Menu Style → XMB** brings back the columns.
- Box art is fetched from GameTDB each time the app starts, only for the games that are still
  missing a cover.
- Set a game as the **Default Game** and Crema starts it by itself (hold **L1** while the app opens
  to get the menu instead). A short animated intro plays at launch.
- Menus in 13 languages: English, Español, Français, Deutsch, Italiano, Português, Nederlands,
  Polski, Türkçe, Русский, 日本語, 中文 and 한국어.

**The DualShock 4 as a Wii U GamePad**
- Click the touchpad to touch the GamePad screen where your finger is, with a pointer showing
  where it lands. Two fingers press Minus (left) or HOME (right).
- Tilt and turn the controller to move the GamePad's gyro.
- The TV shows the game's TV picture or its GamePad screen, swapped in the in-game menu.
- The Wii U Pro Controller, Classic Controller and Wii Remote (with Nunchuk) can be emulated too,
  for local multiplayer.

**Every game tuned the way you like**
- Each setting can be global or saved for a single game. Switch a game to *This Game* and it
  keeps its own copy; switch it back and it follows the global values again.
- **Graphic packs** from the Cemu community download on their own. Turn them on per game and
  choose their presets (frame rate, resolution, draw distance...) from the game's options.
- Cemu's own game profiles are followed (*CPU Mode: Auto*), so games that need single-core
  emulation get it.

**An in-game menu on L3 + R3**
- Swap the TV and GamePad screens, the touch pointer, the touchpad mode, motion controls, game
  volume and the FPS counter, all applied at once.
- Change the controllers and their buttons, restart the game, or quit straight back to the launcher.

**Online, your way**
- Play on [Pretendo Network](https://pretendo.network) with your own Wii U's files, or on your own
  private Pretendo server.

**Make it yours**
- Twelve colour themes, from *Crema Night* and *Espresso* to *Latte* and *Midnight*.
- Drop PNG images in a folder to use them as wallpapers, or WAV files to replace the menu sounds.

**Built for the PS4's hardware**
- Shaders are compiled in the background, and the shader and pipeline caches are kept for next
  time. Caches from Cemu on a PC work too.
- The recompiler runs on spare CPU cores and remembers the game's code: from the second start,
  the functions it needed last time are prepared in advance, so there are fewer hitches.
- Games without the Wii U's Mii data get Crema's own simple Miis instead of crashing.
- Logs, freeze reports and a profiler you can switch on when something needs investigating.

## Getting started

**You need:** a jailbroken PS4 with **GoldHEN** (developed on a launch-model PS4), an FTP connection
to it, and dumps of games you own.

1. Grab the latest **`Crema-vXX.XX.pkg`** from the [releases page](https://github.com/iHaiDeeZ/Crema/releases/latest)
   and install it with GoldHEN's **Package Installer**.
2. Start **Crema** once: it creates **`/data/Crema/`**.
3. Put your games in **`/data/Crema/games/`**. Accepted formats are unpacked game folders (with
   `code`, `content` and `meta` inside), `.wua`, `.wud` / `.wux`, `.wuhb`, NUS folders (with a
   `title.tmd`), and `.rpx` / `.elf` (homebrew). Encrypted disc images need their key in
   `/data/Crema/keys.txt`.
4. Put updates and DLC, unpacked, in Crema's Wii U storage (see below), then press **Square** in
   the library.

> **Tip:** `.wua` is the best format to store games in: one compressed file with the game, its
> update and its DLC. Cemu on a PC makes them: in its *Title Manager*, right-click the game and
> choose *Convert to compressed Wii U archive (.wua)*.

**Updates and DLC** go in by title ID: the update in
`/data/Crema/mlc01/usr/title/0005000e/<last 8 digits of the ID>`, the DLC in
`/data/Crema/mlc01/usr/title/0005000c/<last 8 digits of the ID>`. For *Breath of the Wild*
(`00050000101c9400`) that's `.../0005000e/101c9400` and `.../0005000c/101c9400`.
**Triangle on the game → Information** shows whether both were found. Games often need their
latest update to run well.

**Updating Crema:** install the newer package on top of the old one. Everything you've set up lives
in `/data/Crema/` and survives updates. **Settings → About** shows which version you're on.

## Controls

### In the launcher

| Press | To |
|---|---|
| D-pad / left stick | Move around |
| L1 / R1 | Switch tabs (Library), or scroll five entries at a time (XMB) |
| Cross | Play the game / confirm |
| Circle | Go back |
| Triangle | Open a game's options (start, game settings, graphic packs, information, default game, delete) |
| Square | Look for new games |
| OPTIONS | Sort the games (A-Z, Recently Played) |

### While playing

| Press | To |
|---|---|
| L3 + R3 | Open the in-game menu (again, or Circle, to go back) |
| Left / Right in the menu | Change a value |

### Default button layout

The Wii U's buttons sit where they are on a Wii U GamePad, so A is on the right and B at the bottom.

| DualShock 4 | Wii U GamePad |
|---|---|
| Circle | A |
| Cross | B |
| Triangle | X |
| Square | Y |
| L1 / R1 | L / R |
| L2 / R2 | ZL / ZR |
| OPTIONS | + |
| Touchpad, two fingers left / right | − / HOME |
| Touchpad click | Touch screen (at your finger) |
| Left / right stick | Left / right stick |
| L3 / R3 | Stick buttons |
| D-pad | D-pad |
| Tilt and turn | Gyro |

With *Settings → Controls → Touchpad: Minus / HOME*, the touchpad's left and right halves are
simply those two buttons. The in-game menu's **Controls** changes buttons and which controller each
player has.

## Settings at a glance

| Group | Highlights |
|---|---|
| Display | TV Shows (TV or GamePad screen), touch pointer, FPS counter, VSync, picture size, upscaling filter, render resolution (100 / 75 / 50%), texture quality |
| Emulation | Speed preset (Off, Fast, Fastest), core layout (Spread, Clustered, Split), CPU mode (Auto, multi-core, single-core), thread quantum, shader compilation, graphics accuracy, console language, Mii data, compare results, thread fairness |
| Audio | Game volume, GamePad speaker, audio buffer |
| Controls | Touchpad mode (touch screen or Minus / HOME), motion controls |
| Online | Off, Pretendo or a private server (with its address), and whether each one's files are there |
| Diagnostics | Performance log, freeze reports, TV screenshots, file access log, profiler, GPU fault recorder |

The launcher also has its language, sounds, volume, menu style and clock, the game folder, a cover
downloader, **Reset All Settings** and **About**.

### Graphic packs

**Triangle on a game → Graphic Packs.** Cemu's community packs are downloaded the first time
(*Check for Updates* gets the newest). Each pack for the game can be turned on or off, with its
presets, and applies the next time the game starts. Packs that raise the resolution cost GPU
time, and frame-rate packs need CPU time the PS4 often doesn't have. Avoid *Extended Memory*
packs: they crash on the PS4.

### Recommended settings

Every game we've tested has its best settings in [`game-settings.ini`](game-settings.ini), with notes
on what they fix. Crema carries that file and downloads the newest copy from GitHub each time it
opens, so new findings reach you without an update. With **Settings → Emulation → Recommended
Settings** on (the default), a tested game starts with its recommended settings, unless you've given
it its own settings. **Triangle on a game → Information** shows how it was tested and what it uses.
The [compatibility table](#game-compatibility) lists the same games.

## Your files

Everything the app creates sits in **`/data/Crema/`**:

- `games/`: your games
- `mlc01/`: the Wii U's internal storage: updates, DLC and your saves (`mlc01/usr/save`)
- `keys.txt`: keys for encrypted disc images
- `covers/`: box art, one `<product code>.png` per game. Swap in your own art if you like.
- `themes/`: custom wallpapers (PNG) and menu sounds (`themes/sounds/*.wav`)
- `graphicPacks/`: Cemu's community graphic packs
- `cache/`: shader and pipeline caches (`cache/shaderCache`) and the recompiler's notes (`cache/jit`)
- `online/pretendo/`, `online/private/`: your Wii U's files for online play
- `crema.ini`: your settings, global and per game
- `launcher.ini`: launcher preferences (theme, wallpaper, sounds, menu style, last game)
- `settings.xml`: Cemu's own settings (graphic packs, accounts)
- `log.txt`, `boot-trace.log`, `crash.log`, `hang.log`: logs and freeze reports for bug reports

## Game compatibility

| Status | Meaning |
|---|---|
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Runs well enough to play through |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | Gets into the game, but slowdowns, glitches or freezes get in the way |
| ![Intro](https://img.shields.io/badge/Intro-e08a1e?style=flat-square) | Shows the intro or menus, but doesn't get into the game |
| ![Loadable](https://img.shields.io/badge/Loadable-e74c3c?style=flat-square) | Starts, but stays on a black screen |
| ![Nothing](https://img.shields.io/badge/Nothing-455556?style=flat-square) | Doesn't start, or closes right away |

**Tested by us** on a launch-model PS4, frames per second while playing, with every Diagnostics
switch off unless noted. The settings named here are applied for you (see
[Recommended settings](#recommended-settings)):

| Status | Game | FPS |
|---|---|---|
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | The Legend of Zelda: Breath of the Wild | 20–24 (its normal rate is 30) with *Graphics Accuracy: Fast*, *Thread Quantum: 100000* and the **FPS++** and **Draw Distance** graphic packs. The first 2–3 minutes after loading are slow while its code is compiled. |
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Donkey Kong Country: Tropical Freeze | Menus ~20, cutscenes ~45 (measured with diagnostics on); smooth with them off. *Thread Quantum: 100000*. |
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Mario Kart 8 | Races 45–52, menus 60 |
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Minecraft: Wii U Edition | ~37 with its update, 15+ minutes without a problem. *Thread Fairness: Off*. |
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Nintendo Land | ~60 |
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Paper Mario: Color Splash | ~28 (its normal rate is 30), 21 in busy towns. *Graphics Accuracy: Fast* gives ~29.5, but parts of the map may show black: being looked into. |
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Super Mario Maker | 60 (its normal rate). Needs Crema v03.14 or newer. Edit courses on the touchpad. |
| ![Playable](https://img.shields.io/badge/Playable-1ebc61?style=flat-square) | Tokyo Mirage Sessions #FE | 30, locked (its normal rate). Needs Crema Lab v02.66 or newer. |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | Batman: Arkham Origins | Not measured. Needs *CPU Mode: Auto* (single-core); the first start takes about a minute. |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | New Super Mario Bros. U | ~30 (its normal rate is 60) |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | Ninja Gaiden 3: Razor's Edge | 17–30 (60 on a PC). Needs *Thread Fairness: On* for its cutscenes; *Graphics Accuracy: Fast*. |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | Sonic Lost World | 20–32 (its normal rate is 60). Needs Crema Lab v02.60 or newer, *Graphics Accuracy: Fast*, *Thread Quantum: 100000* and the **Graphic Options** pack at TV 854x480, GamePad 320x180. Crashes now and then. |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | Super Mario 3D World | ~30 (its normal rate is 60) |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | Tekken Tag Tournament 2 | Fights 33–38, menus 60 (the game slows down with its frame rate). Needs *Thread Fairness: On*. |
| ![Ingame](https://img.shields.io/badge/Ingame-f9b32f?style=flat-square) | Xenoblade Chronicles X | ~30 in the intro. Needs its update and *CPU Mode: Auto*. |
| ![Nothing](https://img.shields.io/badge/Nothing-455556?style=flat-square) | Donkey Kong Country (Virtual Console) | Closed at start (a GPU fault); fixed in Crema Lab v02.27, waiting for a test. |

A range means the frame rate depends on what's on screen. Tested a game that isn't listed?
[Open an issue](https://github.com/iHaiDeeZ/Crema/issues) with the game, its region, your console
and firmware, and how it ran.

## FAQ

The full list is in **[FAQ.md](FAQ.md)**: installing, games, updates and DLC, controls, speed,
online play and bug reports. The most common questions:

**Why is a game slow at first?**
Crema compiles the game's code and shaders the first minutes it runs. It keeps what it compiled,
so the next starts are smoother.

**Why does a game slow down in busy scenes?**
The PS4's CPU is usually the bottleneck, not its GPU. Keep every Diagnostics switch off, and try
*Graphics Accuracy: Fast* in that game's settings. On a PS4 Pro, turn on **Boost Mode** in the PS4's
settings.

**A game shows a black screen or flickers.**
Set *Graphics Accuracy* back to *Accurate*, make sure the TV shows the TV picture (L3 + R3 → *TV
Shows*), and turn off that game's graphic packs.

**A game crashes or closes.**
Check its update (and DLC) are in the right folders, set *Compare Results* to *Exact* and *CPU
Mode* to *Auto* in its settings, and turn off its graphic packs. If it still happens, report it.

**How do I report a bug?**
[Open an issue](https://github.com/iHaiDeeZ/Crema/issues). Attach `log.txt`, `boot-trace.log` and,
if there is one, `crash.log` or `hang.log` from `/data/Crema/`, and mention the game, its region,
the Crema version (**Settings → About**), your PS4 model and firmware.

## Good to know

- The Wii U shows two screens; Crema shows one at a time. Swap them with L3 + R3 → *TV Shows*.
- Games built around the GamePad's camera or microphone, or two screens at once, are hard to play
  this way.
- Save states aren't supported; save in the game. Your saves are in `/data/Crema/mlc01/usr/save`
  and carry over to new versions.
- Online play needs files from your own Wii U (made with Dumpling).
- Only the original PS4 has been tested so far. PS4 Pro reports are welcome.

## Thanks

- **[Cemu](https://cemu.info)**: the emulator itself, from its creators **Exzap** and **Petergov**
  to everyone who has contributed since it was open-sourced
- **[Mesa](https://mesa3d.org)**: the RADV Vulkan driver that draws every frame
- **[OpenOrbis](https://github.com/OpenOrbis)** and **[PacBrew](https://github.com/PacBrew)**: the
  PS4 toolchains
- **[love-ps4](https://github.com/Mari0/love-ps4)**: the reference PS4 setup
- **[Cemu graphic packs](https://github.com/cemu-project/cemu_graphic_packs)** and their authors
- **[Pretendo Network](https://pretendo.network)**: online play
- **[GameTDB](https://www.gametdb.com)**: box art

Everyone involved, with licences, is listed in **[CREDITS.md](CREDITS.md)**.

## License and disclaimer

Released under the **Mozilla Public License 2.0**, the same licence as Cemu. See [LICENSE](LICENSE).
The launcher and in-game menu are adapted from [Dolphin for PS4](https://github.com/iHaiDeeZ/DolphinPS4).

This is a fan-made project with no ties to Nintendo, Sony Interactive Entertainment or the Cemu
project. Wii U is a trademark of Nintendo; PlayStation and PS4 are trademarks of Sony Interactive
Entertainment. Please only play games you own. This project won't help you find or share game
files.
