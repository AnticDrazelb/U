# Crema: FAQ

Answers to the questions and problems players have run into. If yours isn't here,
[open an issue](https://github.com/iHaiDeeZ/Crema/issues).

- [Installing and updating](#installing-and-updating)
- [Games, updates and DLC](#games-updates-and-dlc)
- [Controls](#controls)
- [Settings and graphic packs](#settings-and-graphic-packs)
- [Speed and stutter](#speed-and-stutter)
- [Online play](#online-play)
- [Crashes, freezes and bug reports](#crashes-freezes-and-bug-reports)

## Installing and updating

**What do I need?**
A jailbroken PS4 with **GoldHEN**, an FTP connection to it (GoldHEN's FTP server on port 2121, with
an FTP client such as FileZilla), and dumps of Wii U games you own.

**How do I install Crema?**
Copy the `Crema-vXX.XX.pkg` from the [releases page](https://github.com/iHaiDeeZ/Crema/releases/latest)
to the PS4 (a USB drive, or FTP to `/data/pkg`), install it with GoldHEN's **Package Installer**,
then start **Crema** from the home screen.

**There's no `Crema` folder in `/data`.**
Crema creates `/data/Crema/` the first time it starts. Start the app once, then put your games in
`/data/Crema/games/`.

**How do I update Crema?**
Install the newer package on top of the old one. Your games, saves and settings in `/data/Crema/`
stay. **Settings → About** shows which version you're on.

## Games, updates and DLC

**Which game formats work?**
Unpacked game folders (with `code`, `content` and `meta` inside), `.wua`, `.wud` / `.wux`, `.wuhb`,
NUS folders (with a `title.tmd`), and `.rpx` / `.elf` homebrew. `.zip`, `.rar` and `.7z` aren't
read: extract them first.

**Which format is best?**
`.wua`: one compressed file with the game, its update and its DLC. Cemu on a PC makes them: in its
*Title Manager*, right-click the game and choose *Convert to compressed Wii U archive (.wua)*.
Unpacked folders work just as well, they're only bigger.

**My game doesn't show up.**
Put it in `/data/Crema/games/` (or the folder chosen in **Settings → Game Folder**), then press
**Square** in the library. Encrypted disc images (`.wud` / `.wux`) need their disc key in
`/data/Crema/keys.txt`, one key per line.

**Where do updates and DLC go?**
Unpacked (decrypted), in Crema's Wii U storage, by the game's title ID:

- **Update**: `/data/Crema/mlc01/usr/title/0005000e/<last 8 digits of the ID>`
- **DLC**: `/data/Crema/mlc01/usr/title/0005000c/<last 8 digits of the ID>`

For *Breath of the Wild* (USA, `00050000101c9400`): `.../0005000e/101c9400` and
`.../0005000c/101c9400`. **Triangle on the game → Information** shows whether the update and the
DLC were found. Many games need their latest update to run well.

**Where are my saves?**
In `/data/Crema/mlc01/usr/save/`, as on a Wii U. They stay when you update Crema.

**A game with Miis crashes (New Super Mario Bros. U, Mario Kart 8, Nintendo Land).**
Keep **Settings → Emulation → Mii Data When Missing** on *Crema Miis*. Without the Wii U's Mii data,
those games crash or show Miis without heads; Crema's own simple Miis stand in. A dump of your
Wii U's Mii data is used instead when it's there.

## Controls

**How do the DualShock 4's buttons map to the GamePad?**
By position, as on a Wii U GamePad: Circle is A, Cross is B, Triangle is X, Square is Y, L1 / R1 are
L / R, L2 / R2 are ZL / ZR and OPTIONS is +. The full table is in the
[README](README.md#default-button-layout).

**How do I touch the GamePad screen?**
Click the touchpad: the GamePad screen is touched where your finger is, and a pointer shows where
(*Settings → Display → Touch Pointer*). Two fingers press Minus (left) or HOME (right). If a game
needs Minus and HOME more than touch, set *Settings → Controls → Touchpad* to *Minus / HOME*.

**The game needs the GamePad screen.**
Press **L3 + R3** and set *TV Shows* to *GamePad Screen*. *Settings → Display → TV Shows* picks
which one a game starts with.

**Tilting the controller does nothing.**
Motion controls need an official DualShock 4: many third-party controllers have no motion sensors.
Also check *Settings → Controls → Motion Controls* is On.

**Can I play with friends?**
Yes, locally. Press **L3 + R3 → Controls** to give each player a controller: a Wii U Pro
Controller, Classic Controller or Wii Remote (with or without a Nunchuk) on their own DualShock 4.

**How do I open the menu instead of my Default Game?**
Hold **L1** while Crema starts.

## Settings and graphic packs

**Where are the settings?**
Global ones in the **Settings** tab. For one game: **Triangle on the game → Game Settings**, then
set *Apply To* to *This Game*; the game keeps its own copy from then on.

**What do the main settings do?**

- **CPU Mode**: *Auto* (recommended) follows Cemu's profile for each game.
- **Thread Quantum**: leave it on *Default* unless a game's notes say otherwise.
- **Graphics Accuracy**: *Accurate* (default), or *Fast*: faster in busy games, but some may flicker.
- **Shader Compilation**: *Asynchronous* means no stutter, but objects may appear a moment late.
- **Compare Results**: *Fast*; use *Exact* if a game misbehaves.
- **Thread Fairness**: only for games that need it (Tekken Tag Tournament 2).
- **VSync**: *On* removes tearing, at a little delay.
- **Render Resolution**: *75%* or *50%* draws the game smaller and scales it up to the TV: less GPU
  work, a softer picture (pair it with the *FSR 1* upscaling filter). A resolution graphic pack wins
  over it.
- **Speed Preset**: *Fast* or *Fastest* turns on the speed-ups that cost picture quality or
  accuracy in one go (Graphics Accuracy Fast, a lower Render Resolution and Texture Quality, FSR 1
  upscaling, Core Layout Clustered; *Fastest* also native Quad Draws). Go back to *Off* for a game that looks wrong.
- **Core Layout**: *Clustered* keeps the emulated Wii U cores and the GPU thread on one of the
  PS4 CPU's two 4-core modules (one shared cache, fast hand-offs); *Split* also moves everything
  else to the other module. Compare against *Spread* with the FPS counter.
- **Texture Quality**: *Reduced* or *Low* reads textures from their smaller mipmaps with simpler
  filtering: less GPU memory traffic, blurrier surfaces.
- **Diagnostics**: keep everything *Off* for the best speed. Turn them on only to report a problem.

**Which settings are best for my game?**
The tested ones are in [`game-settings.ini`](game-settings.ini) and the
[compatibility table](README.md#game-compatibility). For *Breath of the Wild*: *Graphics Accuracy:
Fast*, *Thread Quantum: 100000*, and the **FPS++** (Normal) and **Draw Distance** graphic packs
(NPCs Very Low, Terrain Low, Trees Potato, Grass Potato). For *Donkey Kong Country: Tropical
Freeze*: *Thread Quantum: 100000*. For *Tekken Tag Tournament 2*: *Thread Fairness: On*.

**How do graphic packs work?**
**Triangle on a game → Graphic Packs.** Cemu's community packs are downloaded the first time, and
*Check for Updates* gets the newest. Turn a pack on and pick its presets; it applies the next time
the game starts. Avoid *Extended Memory* packs: they crash on the PS4.

**How do I customise the launcher?**
- **Covers**: *Settings → Download Covers*, or put `<product code>.png` in `/data/Crema/covers/`
- **Wallpapers**: PNG images in `/data/Crema/themes/`
- **Menu sounds**: 16-bit WAV files in `/data/Crema/themes/sounds/`
- **Default game**: Triangle on a game → *Set as Default Game*

## Speed and stutter

**Why is a game slow at first?**
Crema compiles the game's code and shaders the first minutes it runs. It keeps both, so later
starts are smoother: the functions a game needed last time are prepared in advance.

**Why does a game slow down in busy scenes?**
The PS4's CPU is usually the bottleneck, not its GPU. Keep every Diagnostics switch off, and try
*Graphics Accuracy: Fast* in that game's settings. Graphic packs that raise the resolution or the
frame rate cost speed. When the GPU is the limit (the *Performance Log* shows its load), *Render
Resolution: 75%* and *Texture Quality: Reduced* in **Settings → Display** take work off it; they
don't help when the CPU is the limit.

**Does a PS4 Pro help?**
Yes. Turn on **Settings → System → Boost Mode** in the PS4's own settings, then start Crema: its CPU
then runs about 30% faster, which is what limits most games.

**Can I use my shader and pipeline caches from Cemu on a PC?**
Yes. Crema uses the same **transferable** caches as Cemu, in the same layout, so bringing yours
over saves the stutter when new effects appear.

1. From Cemu's `shaderCache/transferable` folder, take `<title ID>_shaders.bin` and
   `<title ID>_vkpipeline.bin` (for *Breath of the Wild* USA: `00050000101c9400_shaders.bin` and
   `00050000101c9400_vkpipeline.bin`).
2. Close Crema, and back up the PS4's own files for that game if there are any.
3. Copy both files into `/data/Crema/cache/shaderCache/transferable/` over FTP.
4. Start the game. It shows *Loading cached pipelines* while it compiles them: the first time can
   take a few minutes, after that it's much faster.

The title ID must be your game's region (*Breath of the Wild*: USA `101c9400`, Europe
`101c9500`, Japan `101c9300`). Only copy the transferable files: the `precompiled` and `driver`
caches are made for a PC's GPU and won't work. Crema builds these caches by itself as you play
too.

## Online play

**Can I play online?**
On [Pretendo Network](https://pretendo.network), with your own Wii U's files. Dump them from your
Wii U with Dumpling and put them in `/data/Crema/online/pretendo/`, then set *Settings → Online* to
*Pretendo*. The *Pretendo Files* row shows what's still missing (`otp.bin`, `seeprom.bin`, the
certificates and your account).

**Can I use my own server?**
Yes: set *Online* to *Private Server*, type your server's address (for example
`192.168.0.10:8888`, for pretendo-docker) in *Server Address*, and put its files in
`/data/Crema/online/private/`.

## Crashes, freezes and bug reports

**A game shows a black screen or flickers.**
- Set *Graphics Accuracy* back to *Accurate*.
- In the in-game menu (L3 + R3), set *TV Shows* to *TV Screen*.
- Turn off the game's graphic packs.

**A game crashes or closes.**
- Make sure you're on the latest Crema.
- Check the game's update and DLC are in the right folders.
- Turn off its graphic packs, set *CPU Mode* to *Auto* and *Compare Results* to *Exact*.

**A game stops on "Loading cached pipelines".**
A damaged cache, usually left by an earlier crash. Close Crema and rename that game's files in
`/data/Crema/cache/shaderCache/transferable/` and `/data/Crema/cache/shaderCache/driver/vk/` (for
example to `.bak`): Crema builds new ones.

**How do I report a bug?**
[Open an issue](https://github.com/iHaiDeeZ/Crema/issues) right after it happens and attach these
files from `/data/Crema/`:

- `log.txt`
- `boot-trace.log`
- `crash.log` or `hang.log`, if there is one

Mention the game and its region, the Crema version (**Settings → About**), your PS4 model and
firmware, and what you were doing. For a freeze, turn on *Settings → Diagnostics → Freeze Reports*
first and let it sit 15 seconds: `hang.log` then shows where it stopped.
