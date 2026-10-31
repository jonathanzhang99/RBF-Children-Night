# RBF Children's Night Game

The included spaghetti code in this repository powers the Brawl Stars shooting game for RBF Children's Night. The directory is roughly structured as follows:

`arduino/`: Contains the code to be flashed to the Arduino devices (tested only on Uno). `game.ino` drives the main game loop (IR targets + LEDs), `score.cpp` drives the score display and `clock.cpp` drives the countdown clock.

`entrypoint.py`, `game_state`, `input_handler`: The python files to run the threaded server. Includes a flask webserver using socket io to communicate with a frontend. Uses an event driven architecture to interact with the Arduinos.

`frontend/`: The frontend code for the game. Built with svelte using bun.js (not node!!). Note: Used only during 2023 and superceded by arduino code

`audio/`: Brawl Stars audio sound effects

## Installation

Supported python version: `>=3.12`

1. Install [uv](https://docs.astral.sh/uv/)
2. run `uv sync`

## Local Dev

Make sure that your Arduino is connected.

### 2025 update:

Python dependencies are now managed with uv instead of poetry. Use `uv run` wherever you used to use `poetry run`.

Changes to the showdown game mode in `arduino/game.ino`:

1. Scoring is now based on how long it takes to destroy each target (100-200 points per target) instead of a flat amount per level.
2. Level ups happen at 8 and 14 targets destroyed. At 8, targets start randomly jumping. At 14, each hit counts for half as much (2 hits to destroy a target instead of 1).
3. An IR remote sending protocol `8` / command `10` toggles the game on and off. The protocol and command of every received IR signal are printed over serial, which helps find the codes for a new remote.
4. Uses the newer IRremote API (`IrReceiver.irparams`), so flash with a recent 4.x version of the IRremote library.

### 2024 update:

`entrypoint.py` supports 3 arduino modules using the given options:

1. `port-read` + `baudrate-read`: Configures the arduino that drives the main game loop.
2. `port-write` + `baudrate-write`: Configures the arduino that drives the score.
3. `port-clock` + `baudrate-clock`: Configures the arduino that drives the countdown clock.

To run the server use:

```
uv run python entrypoint.py --port-read /dev/<PORT> --baudrate-read <BAUDRATE> --port-write /dev/<PORT> --baudrate-write <BAUDRATE> --port-clock /dev/<PORT> --baudrate-clock <BAUDRATE>
```

Run `uv run python entrypoint.py --help` for the full list of options (e.g. `--game-len`, `--test`, `--debug`).

### 2023 Usage:

The frontend uses svelte packaged with bun.js. This has not been updated or used since 2023 and can be run with:

```
cd frontend
bun install
bun run dev
```
