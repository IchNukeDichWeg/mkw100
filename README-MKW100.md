# MKW100

Experimental WiiCompiled fork for configurable 1–100 racer offline single-player VS races. Normal menus, controller driving, track changes and next race have been play-tested. Numeric position HUD, expanded lightning/POW, instant standard item boxes, unlocks and optional alternate-frame rendering are included. Other mechanics, special item boxes and performance still need work. Online and Retro Rewind integration are not supported by this fork.

## Windows installation

1. Install and successfully run WiiCompiled first. Keep its **Toolkit**, **BuildWorkspace** (including Aurora, Dependencies and clean PAL Assets), **Base**, and **GameAssets/DATA** folders. An executable-only installation is insufficient.
2. Install a .NET SDK (8 or newer). The installer builds this fork's translator; the old bundled translator is insufficient.
3. Clone this repository, then double-click **Install-MKW100.cmd** and enter your WiiCompiled installation directory. Or run:

```powershell
./Install-MKW100.ps1 -InstallRoot 'D:/Games/WiiCompiled'
```

4. Launch **Play-MKW100.cmd**. Select single-player VS normally. F10 → Local VS configures racers and rendering options.

The first build can take a while. Logs go to `build-logs`; incremental build output stays in `native-build-win`. Matching cached native dependencies are reused. If unavailable, the script stops with an explanation; `-AllowSourceDependencies` permits building them, which requires additional disk space and potentially network access.

The separate `play` directory has its own settings and saves. The installer does not replace the original WiiCompiled executable or copy your original save data. Updates preserve this fork's settings. Game data is referenced from the installation path; keep that installation available. Use the game menus to create a fresh license on first launch.

## Development and sharing

Run the installer again after pulling source changes. Never commit `Assets`, `generated`, native build output, `play`, dependencies, game files, saves, caches, or generated executables. Translation and compilation happen on each user's machine using their own game files.

Primary changes live under `runtime/src/hle`, `runtime/include/guest_abi`, `projects/mkwii/mkw100`, and the translator. The current gameplay integration is in `local_vs.cpp`; POW, player-slot storage and starting-grid support have separate modules. Registering a new native wrapper requires regenerating translations and dispatch, not just rebuilding C++—the installer does this.

Before submitting changes, describe the racer count, track, item conditions, controller behavior and how you tested them. Test normal menu entry, race completion and next race. CPU oracle commands are documented under `docs/mkw100`; expanded gameplay changes may intentionally change old hashes. Keep <=12 regression behavior unless explicitly changing it.

The renderer option preserves simulation ticks while skipping alternate render iterations. It is not a guarantee of full speed under every workload. The HUD's game-speed indicator measures simulation speed independently of displayed FPS.

## Attribution

Fork of WiiCompiled; original documentation is in `README.md`. Preserve `LICENSE`, upstream attribution and dependency licenses. This project uses the upstream GPLv3 license. Instant standard-box behavior follows Anarion's inactive-store approach, also documented by Unnamed at https://mariokartwii.com/showthread.php?tid=2206. No proprietary game assets or generated game executable should be published with this source repository.
