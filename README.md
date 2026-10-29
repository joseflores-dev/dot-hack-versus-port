# .hack//Versus — native port for Android and Windows

*English. [Versión en español](README.es.md).*

> [!IMPORTANT]
> No game files are included. You need your own legally obtained copy of **.hack//Versus** (PlayStation 3,
> BLJS93008) as a disc image (`.iso`). A fan project, not affiliated with CyberConnect2, Bandai Namco or Sony.

> [!NOTE]
> Work in progress. Fights, menus, saving and controllers work; Story Mode and the online modes have not been played
> through. Bug reports, fixes and improvements are very welcome: open an issue or a pull request.

A **native** port (no emulator) of the fighting game .hack//Versus to **Android ARM64** and **Windows x64**, made by
static recompilation with [ps3recomp](https://github.com/sp00nznet/ps3recomp): the game's PPU code is translated to
C++ and compiled for the target, its SPU programs are translated the same way, and the RSX GPU is emulated on Vulkan.
It is not an emulator. Tested on a Samsung Galaxy S22 Ultra (Snapdragon 8 Gen 1, Adreno 730, Android 16) and on a
Windows 10 PC (Ryzen 7 5700X3D, GeForce RTX 3050).

![Fight: Sora vs Sora in Aura's Garden](docs/pelea.png)

## Features

- The game runs as a native app: install the APK, pick your ISO, play. No PS3 firmware, nothing to extract.
- Reads the disc image in place (its own ISO9660 reader): no copy, no storage permission.
- Bluetooth controllers (DualShock 4 and compatibles) with the PS3 mapping, L3/R3 included; an on-screen touch pad
  that can be hidden (**PAD** button).
- Saving, sound (music, voices and hits), intro trimmed to the logos, full-screen rendering.
- Performance: menus at 30 fps (the game's cap); fights at 25–27 fps on the phone with its battery saver on, and a
  steady 30 fps on the PC.
- Windows: full screen at the desktop resolution (F11 or Alt+Enter toggles a window), controllers through SDL and the
  keyboard as a fallback.

![Title](docs/titulo.png)

## Requirements

**Android**
- An ARM64 phone with Vulkan 1.1 and Android 12 or later; 8 GB of RAM recommended.
- About 4.3 GB free for the ISO (the app does not copy it) plus 2 GB for the game's own HDD install. The APK is 35 MB.
- Your disc image of .hack//Versus (see "Supported versions").

**Windows**
- Windows 10 or 11, 64-bit, a CPU with AVX2 (Intel Haswell / AMD Zen or later) and a GPU with Vulkan 1.1 drivers.
- About 4.3 GB for the extracted disc plus 2 GB for the game's HDD install.

## Supported versions

| Disc | Status |
|---|---|
| BLJS93008 (Japan, the game's only release) with the English translation patch applied | The version the port was made and tested with |
| BLJS93008 unpatched | Should work (the executable shipped in the APK is the patched one; texts would be Japanese), untested |
| Other regions or digital versions | None exist for this game |

The APK ships the game's executable already translated (what the PS3 ran as `EBOOT.BIN`): nothing to decrypt, no
emulator needed. If you have your own `EBOOT.ELF`, the app prefers it when it sits next to the data.

## How to play (Android)

1. Download `hackversus.apk` from **Releases** and install it (allow "unknown apps" if Android asks).
2. Copy your `.iso` to the phone, any folder (Downloads, for instance).
3. Open the app and tap **Elegir ISO** ("Choose ISO"): Android's file picker opens; select your image. The app keeps
   the permission on that file and never asks again. The first run takes longer (the game installs ~2 GB to its
   virtual HDD, as on the PS3, and shaders compile); afterwards it starts in about 20 seconds.

Alternatives, if you would rather not use the picker:
- Leave the ISO in `/sdcard/PS3/` and tap **Dar permiso** ("Grant permission") in the dialog: it opens the app's
  own "All files access" switch. Same folder and same file the ARMSX3 emulator uses, in case you have both.
- Or extract the disc on a PC (`python tools/iso_extract.py <your.iso> all disc`) and copy `disc/PS3_GAME` to
  `/sdcard/PS3/vfs/PS3_GAME` or to `Android/data/com.hackversus.port/files/PS3/vfs/PS3_GAME` (the latter needs no
  permission).

Where the app keeps things:
```
Android/data/com.hackversus.port/files/
  PS3/dev_hdd0/game/BLJS93008/           the game's HDD install (~2 GB, made by the game itself)
  PS3/dev_hdd0/home/00000001/savedata/   saves (with /sdcard/PS3 they go to /sdcard/PS3/dev_hdd0/...)
  PS3/hv_env.txt                          options (see below)
  EBOOT.ELF, spirv/, pipeline.cache       the unpacked executable and the shader caches
  log.txt                                 the session log (truncated at every launch)
```
Uninstalling the app deletes that folder, saves included: copy them first if they matter to you.

## How to play (Windows)

1. Download `hackversus-windows.zip` from **Releases** and unzip it anywhere: `hackversus.exe`, `SDL2.dll` and the
   game's translated executable `EBOOT.ELF`.
2. Extract your disc next to them, into a folder named `disc` (needs Python 3):
   `python tools/iso_extract.py <your.iso> all disc` — the result is `disc/PS3_GAME/...`.
3. Run `hackversus.exe`. The first run takes longer (the ~2 GB HDD install and the shader caches).

The PC version does not read the ISO directly yet. Saves, the HDD install and the shader caches go to `data/` next to
the executable; options go in `hv_env.txt` next to it. You can also pass the paths:
`hackversus.exe <EBOOT.ELF> <disc folder>`.

## Controls

- **Bluetooth controller**: pair a DualShock 4 (or compatible) with Android before opening the app; the port assigns
  it to port 1 with the PS3 mapping (✕○□△, L1/R1/L2/R2, L3/R3, START/SELECT, d-pad and sticks).
- **Touch pad**: always active as player 1; the **PAD** button (top centre) hides or shows it.
- **Windows**: any controller SDL recognises (DualShock 4, DualSense, Xbox…). Without one, the keyboard: arrows = d-pad,
  Z/X/A/S = ✕/○/□/△, Q/W = L1/R1, 1/2 = L2/R2, Enter = START, Tab = SELECT. F11 or Alt+Enter switches between full
  screen and a window; Esc leaves full screen (in a window it closes the game).
- The game's own controls are in the game: during a fight, START → "Basic Operation", "Command List" and
  "Battle Key Config".

## Options (`hv_env.txt`)

A file named `hv_env.txt` in the data folder (`Android/data/com.hackversus.port/files/PS3/` in ISO mode, or
`/sdcard/PS3/`; on Windows next to `hackversus.exe`), one `VARIABLE=value` per line:

| Variable | Effect |
|---|---|
| `PS3_OSK_TEXT=NAME` | What the port types when the game asks for text (Story Mode: 8 characters at most, and the game does not let you change it later). Default `PLAYER`. |
| `HV_KEEP_ASPECT=1` | 16:9 with bars instead of stretching to the whole screen. |
| `HV_INTRO=orig` | The full intro (by default only the Bandai Namco and CRIWARE logos are shown). |
| `HV_JC_PAR=n` | Threads for the SPU jobs (3 by default). |
| `HV_AUDIO_LEGACY_PACE=1` | ps3recomp's original audio mixer (in case the new one sounds worse on your device). |
| `HV_LOG=0` | No log: `log.txt` is not written. |
| `PS3RECOMP_VK_FULLSCREEN=0` | Windows: start in a window. |

## Known issues

- Fights do not hold a steady 30 fps with the battery saver on (the cores stay at 1.2–1.8 GHz); without it there is
  headroom. The bottleneck is the CPU (the game's main thread, the SPU skinning jobs, the RSX command stream), not
  the GPU.
- Story Mode asks for a character name through the on-screen keyboard: the port fills it in by itself (`PS3_OSK_TEXT`).
- No online, no real trophies, no user music (all stubs).
- After a long session the phone heats up and lowers its clocks: normal, not a port bug.

## Building

The whole environment is portable and lives in the project folder; nothing is installed system-wide.

1. **Toolchain** (not included): Android NDK r30, CMake ≥ 3.28, Ninja, JDK 17, build-tools 35.0.1 and the API 35
   `android.jar`, Python 3.10 with `pip install -r ps3recomp-0.18.0/requirements.txt`, and in `sysroot/` SDL2 and
   glslang built for `arm64-v8a`.
2. **Lifting the game** (once, on the PC; `game/EBOOT.ELF` is the decrypted executable, which an emulator with
   `PPU Debug` dumps to `cache/ppu_progs/BLJS93008-*/exec.elf`):
   ```
   P=.venv/Scripts/python; R=ps3recomp-0.18.0/tools
   $P $R/ppu_loader.py game/EBOOT.ELF -o analysis/
   $P $R/find_functions.py game/EBOOT.ELF --seed-json analysis/EBOOT.functions.json --output analysis/functions.json
   $P $R/extract_spu_images.py game/EBOOT.ELF --output spu_programs/
   $P $R/ppu_lifter.py game/EBOOT.ELF --functions analysis/functions.json --hle-stubs analysis/EBOOT.imports.json \
       --code-end 0x7bf7b0 --chunks 64 -o recomp/
   ```
   The SPURS jobs (skinning) are not in the ELF: they are dumped on the phone with `SPU_DUMP_MISS=<dir>` the first
   time the game uses them and go to `spu_jobs/*.elf`.
3. **APK**: `tools/build_apk.sh` → `out/hackversus.apk` (configures CMake with the NDK, builds the runtime, the 64
   translated PPU files and the SPUs, links `libmain.so`, packs `game/EBOOT.ELF` as an asset, compiles the Java
   activity and signs with a debug key). A full PPU rebuild takes about 3 minutes on 16 cores.

4. **Windows**: with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (ucrt, x86_64) unpacked in `toolchain/`
   (no Visual Studio needed), `tools/build_win_deps.sh` once (SDL2 and glslang into `sysroot-win/`) and then
   `tools/build_win.sh` → `out-win/hackversus.exe`. The PC build targets x86-64-v3 (AVX2) and uses the same Vulkan
   backend through SDL2. `tools/win_fight.sh [VAR=value ...]` reaches a CPU vs CPU fight with the keyboard and
   measures it (it takes over the window and keyboard for about two minutes).

The scripts in `tools/` (`goto_fight.sh`, `measure_fight.sh`, `prof_threads_fast.py`, …) reach a fight without
touching the phone, measure fps and profile; they carry the adb id of the device the port was developed on, change it
to yours.

## What changed from ps3recomp 0.18.0

The toolkit is included in `ps3recomp-0.18.0/` with this port's modifications (MIT licence, see its `LICENSE`).
Among others: a Vulkan backend for Android (merged render passes, draw batches, asynchronous present, depth read as
colour in compute), scalarised shader register files for Adreno, SPURS jobs in parallel on threads, SPU event queues
for the sound driver, VMX and scalar FP in NEON without NaN checks, NEON vertex and index fetch, a working
`cellOskDialog`, saving, the touch pad and the controller. For Windows: the Vulkan backend built with mingw (a
`LoadLibrary` loader instead of `dlopen`), SDL2 for pad and audio, a GCM callback thread, the keyboard fallback with
any pad backend, and an audio mixer that leaves the game's sound drivers one block period between blocks. The ISO reader (`hackversus/isofs.c`) hangs under the host
file API with the linker's `--wrap`, so no runtime layer changes. The technical details are in the code comments.

## Credits

- CyberConnect2 and Bandai Namco, creators of .hack//Versus.
- [sp00nznet](https://github.com/sp00nznet/ps3recomp), for ps3recomp.
- The team behind the .hack//Versus English translation patch.
- SDL2, glslang, the Android NDK and llvm-mingw.

## License

The code in this repository is under the [MIT license](LICENSE), like ps3recomp; third-party components keep their
own licenses. It grants no rights to the game: .hack//Versus belongs to CyberConnect2 and Bandai Namco. This
repository contains no game data; the APK and the Windows build contain the game's code translated to ARM64 by static recompilation and
does not work without a legitimate copy of the disc. If the rights holders ask, it comes down.
