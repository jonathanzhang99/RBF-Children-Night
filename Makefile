# Uses arduino-cli from PATH, falling back to the copy bundled with the Arduino IDE
ARDUINO_CLI ?= $(shell command -v arduino-cli 2>/dev/null || echo "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli")

.PHONY: install run test ports compile flash-game flash-score flash-clock flash-hardware-test monitor require-port

install:
	uv sync

run:
	uv run python entrypoint.py $(ARGS)

# Run without the game Arduino, typing EMIT lines into stdin
test:
	uv run python entrypoint.py --test $(ARGS)

ports:
	"$(ARDUINO_CLI)" board list

compile:
	"$(ARDUINO_CLI)" compile arduino/game
	"$(ARDUINO_CLI)" compile arduino/display
	"$(ARDUINO_CLI)" compile --build-property "compiler.cpp.extra_flags=-DDISPLAY_CLOCK" arduino/display
	"$(ARDUINO_CLI)" compile arduino/hardware_test

flash-game: require-port
	"$(ARDUINO_CLI)" compile --upload -p $(PORT) arduino/game

flash-score: require-port
	"$(ARDUINO_CLI)" compile --upload -p $(PORT) arduino/display

flash-clock: require-port
	"$(ARDUINO_CLI)" compile --upload -p $(PORT) --build-property "compiler.cpp.extra_flags=-DDISPLAY_CLOCK" arduino/display

# Checks the game board's LED strips and IR receivers; watch it with `make monitor PORT=...`
flash-hardware-test: require-port
	"$(ARDUINO_CLI)" compile --upload -p $(PORT) arduino/hardware_test

monitor: require-port
	"$(ARDUINO_CLI)" monitor -p $(PORT) --config baudrate=115200

require-port:
	@test -n "$(PORT)" || { echo "Set PORT, e.g. make $(MAKECMDGOALS) PORT=/dev/cu.usbmodem1101 (run 'make ports' to list them)"; exit 1; }
