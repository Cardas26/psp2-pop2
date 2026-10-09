# Prince of Persia 2: The Shadow and the Flame for PS Vita

Vibecoded. Based on the recompilation of the IR release by Supermedo. Adjusted for support of v1.1. 

## What works

- All levels and cutscenes.
- FM music and sound effects (Sound Blaster Pro driver).
- Save and resume.
- 4:3 and widescreen views, four filters (Sharp, Pixel, Soft, LCD).
- all controls remappable.

## Requirements

- Optional: [PSVshell](https://github.com/Electry/PSVshell) to change the CPU and GPU clocks.
- 12 MB free space on `ux0:`.
- The DOS game, **version 1.1 only**: its `PRINCE.EXE` is 259,583 bytes.

## Setup

1. Install `pop2.vpk` from the [latest release](../../releases/latest).
2. Create the `ux0:data/pop2-recomp/` folder.
3. Copy the game files into it, including the `SNDDRVRS` folder.
4. The first start checks every game file. Missing or changed files are named on-screen.

### Updating

- Install the new VPK over the old one. Saves and settings are kept.

## Controls

| Button | In play |
|---|---|
| D-pad, left stick | movement |
| Cross | jump |
| Circle | grab, hang, step |
| Square | draw sword, strike |
| Start | pause |
| Select | timer |
| L | save screen |
| R | resume screen |
| Select+L | restart the level |
| Triangle+L | view: 4:3, Wide |
| Triangle+R | filter: Sharp, Pixel, Soft, LCD |

In the save and resume screens Down moves to the next slot, Cross confirms. Any button skips a cutscene.

### Changing the controls

The game writes `ux0:data/pop2-recomp/controls.ini` the first time it starts: every action on its own line, at its default, with what it does.

```ini
; Prince of Persia 2 - the controls. Read when the game starts: edit, save, start the game again.
; Buttons: Up Down Left Right Cross Circle Square Triangle L R Start Select
;          LStickUp LStickDown LStickLeft LStickRight RStickUp RStickDown RStickLeft RStickRight None
; Several buttons: Up, Cross    A chord, the first held and the second pressed: Triangle+L
; A button held for a chord still does its own action: keep Jump, Crouch, Left, Right, Action and Strike off it.
; A mistake falls back to that action's default and is named on the screen at start.
; Delete a line for its default; delete this file for every default.
; The Screen actions work in the save and resume screens only, the others outside them.
; Fixed: any button skips a cutscene.
[controls]
Jump=Up, LStickUp, Cross  ; jump, climb, block
Crouch=Down, LStickDown   ; crouch, put the sword away
Left=Left, LStickLeft     ; run left
Right=Right, LStickRight  ; run right
Action=Circle             ; grab, hang on, step carefully
Strike=Square             ; draw the sword, strike
Pause=Start               ; pause
Time=Select               ; show the time left
SaveScreen=L              ; the game's save screen
ResumeScreen=R            ; the game's resume screen
Restart=Select+L          ; restart the level
View=Triangle+L           ; the next view: 4:3, Wide
Filter=Triangle+R         ; the next filter: Sharp, Pixel, Soft, LCD
ScreenNext=Down, LStickDown ; save and resume screens: the next slot
ScreenConfirm=Cross       ; save and resume screens: save (name it) or resume
ScreenCancel=Circle, Start ; save and resume screens: leave
```

- Change the value after `=`. Names are not case-sensitive; spaces around `=` and a `;` comment after the value are fine.
- Several buttons for one action: `Jump=Up, LStickUp, Cross` (at most four). A chord: `Restart=Select+L`. No button: `None`.
- Each stick counts as four buttons: `LStickUp` to `LStickRight` and `RStickUp` to `RStickRight`. 
- The Screen actions (`ScreenNext`, `ScreenConfirm`, `ScreenCancel`) work only in the save and resume screens and the others only outside them.
- The game reads the file when it starts.
- A name the game does not know falls back to that action's default.
- Delete a line to get its default back; delete the file to get every default back.
- A button you hold for a chord still does its own action, so keep Jump, Crouch, Left, Right, Action and Strike off the first button of a chord.
- If the game can read no action from the file, it keeps it as `controls-unreadable.ini` and writes a fresh one.

## Known issues

- None known. Please report new bugs.

## Reporting a bug

1. Install the **diagnostics** build, `pop2-diagnostics.vpk` from the [latest release](../../releases/latest).
2. Reproduce the problem.
3. Attach to a [bug report](../../issues/new/choose):
   - `ux0:data/pop2-recomp/debug.log`;
   - `ux0:data/pop2-recomp/watchdog.log`, if present;
   - after a crash, the newest `ux0:data/*.psp2dmp`.

## Credits

- The static recompiler and its runtime by Supermedo:
  [prince-of-persia-2-windows](https://github.com/Supermedo/prince-of-persia-2-windows).
- [SDL2](https://github.com/libsdl-org/SDL) and [VitaSDK](https://vitasdk.org/).
- The LCD filter: lcd3x by Gigaherz.
- LiveArea art: the prince by Shiios42 and the title lettering by Kutsune (SteamGridDB).
- Built with [Claude Code](https://claude.com/claude-code)
- Full list and licences: [THIRD_PARTY.md](THIRD_PARTY.md).

## Legal

- This project is not affiliated with or endorsed by Ubisoft.
- No game data is included.
- No donations are accepted.
- The port's code is MIT licensed ([LICENSE](LICENSE)). The recompiler states no licence and is credited above;
  other libraries keep their own licences ([THIRD_PARTY.md](THIRD_PARTY.md)).
