# RBF Children's Night Game

The included spaghetti code in this repository powers the Brawl Stars shooting game for RBF Children's Night. The directory is roughly structured as follows:

`arduino/game/`: The sketch for the Arduino that runs the game (IR targets, LEDs, scoring). Tested only on Uno.

`arduino/display/`: The sketch for the score and countdown clock P10 panels. Both boards run the same sketch and just show whatever text the server sends them, so changing what they display never needs a reflash.

`entrypoint.py`, `game_state.py`, `boards.py`: The python files to run the threaded server. It reads game events from the game Arduino, plays the sound effects, and sends the score and the countdown to the display boards.

`admin.py`, `admin.html`: The admin page for starting and stopping games and changing the mode.

`audio/`: Brawl Stars audio sound effects

## Installation

Supported python version: `>=3.12`

1. Install [uv](https://docs.astral.sh/uv/)
2. run `make install` (or `uv sync`)

Flashing uses `arduino-cli`. The Makefile uses the copy bundled with the Arduino IDE, or `arduino-cli` from your PATH (`brew install arduino-cli`) if you have one.

## Flashing the Arduinos

Plug in one board at a time and find its port with `make ports`, then:

```
make flash-game PORT=/dev/<PORT>
make flash-score PORT=/dev/<PORT>
make flash-clock PORT=/dev/<PORT>
```

The board and library versions are pinned in each sketch's `sketch.yaml`, and arduino-cli installs them on the first build. `make compile` builds all three without flashing anything.

The display boards only need flashing again if `display.ino` itself changes.

To use the Arduino IDE instead, open `arduino/game/game.ino` or `arduino/display/display.ino` and install the library versions listed in that sketch's `sketch.yaml`. For the clock board, uncomment `#define DISPLAY_CLOCK` at the top of `display.ino` before flashing.

## Running the server

Plug in the boards and run:

```
make run
```

The server finds each board by the `HELLO <ROLE>` line it prints when it boots, and reconnects to any board that gets unplugged.

Control the game from the admin page at http://localhost:8080, which shows the time left, the score (orange's and blue's in versus mode) and which boards are connected on which port, with buttons to start and stop games and change the mode. It shows each target's IR sensor as working or stuck low (no power, or the sensor is broken), which target is live (in versus, which is orange's and which is blue's), and "seeing IR" while a sensor picks up shots. A sensor seeing IR when nobody is shooting usually has a loose signal wire. Each target has a Disable button for when its sensor is playing up: a disabled target is never made a live target, but its lights keep showing red and rainbow like the others. It also lists any board that is plugged in but not being used, such as a board that still needs flashing or a clock board flashed as a second score board, and says how to fix it. It only opens on the computer running the server. If port 8080 is taken, pick another with `make run ARGS="--admin-port 8081"`.

Or type into the server's terminal and press Enter:

| Command | What it does |
| --- | --- |
| `start` | Start a game |
| `stop` | End the game early |
| `showdown` | Switch to showdown mode (between games) |
| `versus` | Switch to versus mode (between games) |
| `disable 2` / `enable 2` | Take a target out of the game, or put it back |

The IR remote still works too.

- Opening a port resets an Uno, so restarting the server or replugging the game board ends any game in progress.
- To skip the port search, pass ports explicitly: `make run ARGS="--port-game /dev/<PORT> --port-score /dev/<PORT> --port-clock /dev/<PORT>"`
- `make test` runs without the game board and reads game events (e.g. `EMIT GAME_ACTIVE 60000`) from stdin.
- Run `uv run python entrypoint.py --help` for all options.

## Gameplay notes

### 2025 update:

Changes to the showdown game mode in `arduino/game/game.ino`:

1. Scoring is now based on how fast each target is destroyed instead of a flat amount per level: 200 points within 0.1s, then dropping linearly to 100 points at 5s and after.
2. Level ups happen at 8 and 14 targets destroyed. At 8, targets start randomly jumping. At 14, each hit counts for half as much (2 hits to destroy a target instead of 1).
3. Each game starts on a random target, and targets whose IR sensor reads stuck low are never made a live target, so one broken sensor doesn't stall the game.
4. An IR remote sending protocol `8` / command `10` toggles the game on and off. It's heard by whichever target is live (in versus, the orange one). To find the codes for a new remote, uncomment `#define PRINT_IR_CODES` and the protocol and command of every IR signal are printed over serial.

### 2026 update:

1. Versus lights two targets at once, one orange and one blue, and each player scores on their own color. New targets appear within a second, and the score board shows each player's targets destroyed (`03 05`).
2. Games are run from the admin page instead of the IR remote. It also shows which boards are connected and each IR sensor's health, and can take a broken target out of the game.
3. The server finds each board automatically, and `make flash-*` flashes them with pinned library versions.
4. The scoreboard web page is gone (it's in the git history); the score and clock boards show everything.

### History

- 2024: Added the score and countdown clock boards.
- 2023: The scoreboard was a svelte frontend built with bun.js. It was removed in the 2026 cleanup and is still in the git history.
