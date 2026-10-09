# Third-party code and art

| What | Where | Origin | Licence |
|---|---|---|---|
| The static recompiler and its runtime, by Supermedo | `port/upstream/` (patched for the Vita) | [Supermedo/prince-of-persia-2-windows](https://github.com/Supermedo/prince-of-persia-2-windows) `e2c407f` | none stated upstream; credited here and in the README |
| LCD filter (lcd3x, by Gigaherz): its formula, drawn as a mask texture | `port/upstream/src/plat_sdl.c` | [frangarcj/vita-shader-collection](https://github.com/frangarcj/vita-shader-collection) `shaders/lcd3x_f.cg` | public domain |
| SDL2 2.32.8, statically linked | the eboot (`port/CMakeLists.txt`) | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) `release-2.32.8` | Zlib |
| newlib (libc, libm), statically linked | the eboot | [vitasdk/newlib](https://github.com/vitasdk/newlib), vitasdk `sdk-snapshot-20260926.790.1` | BSD-style, per file (`COPYING.NEWLIB`) |
| LiveArea: the prince | `port/livearea/` (`icon0.png`, `pic0-clouds.png`, `bg0-clouds.png`) | SteamGridDB hero 66505 by Shiios42 | fan art, no licence stated |
| LiveArea: the title lettering | `port/livearea/startup.png` | SteamGridDB logo 111904 by Kutsune (from LaunchBox) | fan art, no licence stated |
| LiveArea: the sky | `port/livearea/` (`icon0.png`, `pic0-clouds.png`, `bg0-clouds.png`) | the game's title screen (LaunchBox's screenshot), upscaled by Real-ESRGAN | the game's art, its rights holder's |
