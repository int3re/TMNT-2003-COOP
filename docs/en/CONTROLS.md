# Controls

[← Home](../../README.en.md) · [Русский](../CONTROLS.md)

In a network game **everybody plays on their own PC with the keys set up there for player 1** —
with the keyboard, a gamepad or the mouse. The keys are shown and changed in the game itself:
**ПАРАМЕТРЫ → ПАРАМЕТРЫ УПРАВЛЕНИЯ** (Options → Control options). Keys and mouse buttons can also
be set without starting the game — on the **УПРАВЛЕНИЕ** (Controls) tab in the settings of the
[launcher](LAUNCHER.md#controls).

> The game sees only the gamepads connected before it was started.

## Default keys

The actions are named the same way as in the game's key table.

| Action | Player 1 | Player 2 |
|---|---|---|
| up, down, right, left | arrow keys | Home, End, Page Down, Delete |
| jump | Space | Num 0 |
| weak attack / «решить» ("decide": select in a menu) | F | Num 4 |
| strong attack / «отмена» ("cancel") in a menu | D | Num 5 |
| shuriken | S | Num 7 |
| dash | E | Num + |
| special attack | Left Ctrl | Num 9 |
| air juggling | R | Num 6 |
| shuriken change / **revive a teammate** | Left Shift | Num 8 |

Start is Enter, pause is Esc. This is the game's own layout; any key can be changed in the table
**ПАРАМЕТРЫ → ПАРАМЕТРЫ УПРАВЛЕНИЯ → КЛАВИАТУРА** (Options → Control options → Keyboard):

![The game's key table](../images/menu-keys.png)

## Mouse

Mouse buttons are keys just like the others. **ПАРАМЕТРЫ → ПАРАМЕТРЫ УПРАВЛЕНИЯ → КЛАВИАТУРА**
(Options → Control options → Keyboard): choose an action ("select", the cell starts blinking) and
click a mouse button. The cell will show **ЛКМ** (left button), **ПКМ** (right button), **СКМ**
(middle button) or **М4 / М5** (side buttons). In the original, mouse buttons cannot be put into
this table.

- **OK** and «ДА» (Yes) save; «Сбросить» (Reset), «Базовые установки» (Defaults) and cancel work
  as they do with keys.
- «Слабая атака» (Weak attack) is also "select" in menus, and «Сильная атака» (Strong attack) is
  "back". If you put mouse buttons on them, selecting and cancelling in menus is done with a
  click too.
- The mouse works while the game window is active and the pointer is over it. The click with
  which you switched to the game does not count.
- If you want an action to be done **both** by a key **and** by a mouse button, this is set in
  `tmntnet.ini`, section `[mouse]` (for example `left=1:attack`): the table will show «F +ЛКМ»
  (F + left button).

## Two players at one keyboard

The **РАСКЛАДКА** (Layout) row on the control options page was added by the mod:

![Control options: the "Layout" row](../images/menu-controls.png)

**ПАРАМЕТРЫ → ПАРАМЕТРЫ УПРАВЛЕНИЯ → РАСКЛАДКА: ДВА ИГРОКА** (Options → Control options → Layout:
Two players). Player 1 sits on the left, player 2 on the right:

| Action | Player 1 | Player 2 |
|---|---|---|
| walking | W A S D | arrow keys |
| attack / select | J | Num 4 |
| strong attack / cancel | K | Num 5 |
| jump | Space | Num 0 |
| shuriken | L | Num 6 |
| shuriken change | H | Num 3 |
| dash | U | Num 1 |
| air juggling | I | Num 2 |
| special attack | O | Num 8 |
| start | Enter | Num Enter |
| pause | Esc | Esc |

**РАСКЛАДКА: КАК В ИГРЕ** (Layout: As in the game) brings back the original's layout. If you
change individual keys, the row will say «СВОЯ» (Own) — it is remembered: switch to a ready-made
layout and back, and your keys return.

Some keyboards do not recognize many keys pressed at the same time. If somebody's movement
"sticks", try other keys.

## Players 3 and 4 at one PC

For the story with three and four players without a network:

- **Gamepads.** With four gamepads, player 3 has the third one and player 4 the fourth. If there
  are fewer gamepads than players, they go to players 3 and 4 first, and players 1 and 2 play on
  the keyboard. The buttons are the same as for player 1's gamepad in ПАРАМЕТРЫ УПРАВЛЕНИЯ
  (Control options). Manually: `p3_pad`, `p4_pad` in the `[mod]` section (`auto`, a gamepad
  number or `0`).
- **Keyboard.** `p3_keys` and `p4_keys` in the `[mod]` section: 16 DirectInput key codes in the
  order of the game's buttons (dash, air juggling, special attack, shuriken change, shuriken,
  jump, attack, strong attack, pause, −, −, start, up, right, down, left); `-1` means none.

In a network game these settings do not apply: there each PC has one player.

## Teammate revive button

By default it is "shuriken change". Another button: `revive_button` in the `[mod]` section
(`dash`, `juggle`, `special`, `shuriken_change`, `shuriken`, `jump`, `attack`, `strong`). Next to
a fallen teammate this button does not change shurikens. In a network game the host's setting
applies.

## Utility keys

| Key | What it does |
|---|---|
| Esc | pause (in a network game — for everybody at once) |
| Alt+Enter | switch "borderless" ↔ "windowed" |
| F12 | a screenshot into the `screenshots` folder |

## Client of the "host-only game" mode

For the [streaming mode](PLAY.md#host-only-game-mode), where the second player controls the game
through `tmntnet-client.exe`:

| Action | Gamepad (Xbox layout) | Keyboard |
|---|---|---|
| walking | D-pad or left stick | I J K L |
| jump | A (cross) | Z |
| weak attack / select | X (square) | A |
| strong attack / cancel | Y (triangle) | X |
| shuriken | B (circle) | Q |
| dash | RB | W |
| shuriken change | LB | B |
| special attack | RT | C |
| air juggling | LT | V |
| start | START | T |
| pause | BACK | P |

Xbox, DualShock 4, DualSense and Switch Pro gamepads and most USB gamepads are supported. The
gamepad is read even when the client's window is minimized. The layout is changed in the `[pad]`
and `[keys]` sections of the `tmntnet.ini` file next to the client.

**Two players at one PC, the second one with a modern gamepad.** Run `tmntnet-client.exe 127.0.0.1`
on the same PC: player 2 will be controlled by any gamepad that the game itself does not
understand.
