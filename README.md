## Controls

| Button | In play |
|---|---|
| D-pad, left stick | run left and right, jump and climb (Up), crouch (Down) |
| Cross | jump |
| Circle | grab, hang on, step carefully |
| Square | draw the sword, strike |
| Start | pause |
| Select | show the time left |
| L | the game's save screen |
| R | the game's resume screen |
| Select+L | restart the level |
| Triangle+L | the next view: 4:3, Wide |
| Triangle+R | the next filter: Sharp, Pixel, Soft, LCD (an LCD panel's look) |

A chord such as Select+L means: hold the first button, press the second.

In the save and resume screens Down moves to the next slot, Cross confirms (on the save screen it opens the keyboard for the slot's name), and Circle or Start leaves; the mapping below changes these too. Any button skips a cutscene.

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
- Each stick counts as four buttons: `LStickUp` to `LStickRight` and `RStickUp` to `RStickRight`. By default the left stick does what the d-pad does and the right stick does nothing.
- The Screen actions (`ScreenNext`, `ScreenConfirm`, `ScreenCancel`) work only in the save and resume screens and the others only outside them, so one button can do one of each.
- The game reads the file when it starts: save your edit, then start the game again.
- A mistake never leaves you without a control. A name the game does not know falls back to that action's default, and so does a movement, sword or pause action left without a button. Each problem is named at the top left of the screen when the game starts.
- Delete a line to get its default back; delete the file to get every default back.
- A button you hold for a chord still does its own action, so keep Jump, Crouch, Left, Right, Action and Strike off the first button of a chord.
- If the game can read no action from the file, it keeps it as `controls-unreadable.ini` and writes a fresh one.

#### Examples

Cross and Circle swapped, so Circle jumps and confirms, and Cross grabs and leaves a screen:

```ini
Jump=Up, LStickUp, Circle
Action=Cross
ScreenConfirm=Circle
ScreenCancel=Cross, Start
```

Grab and sword on the shoulder buttons, the game's screens on Triangle chords:

```ini
Action=L
Strike=R
SaveScreen=Triangle+Square
ResumeScreen=Triangle+Circle
Restart=Triangle+Select
```
