import argparse
import threading
import time
from typing import Any, Dict, List, Optional

import pygame.mixer

from admin import make_admin_server
from boards import DISPLAY_BAUDRATE, GAME_BAUDRATE, Board, BoardManager
from game_state import GameState

parser = argparse.ArgumentParser(description="Game configuration")
parser.add_argument(
    "--test",
    action="store_true",
    default=False,
    help="Run without the game Arduino, reading game events (EMIT lines) from stdin",
)
parser.add_argument(
    "--debug", action="store_true", default=False, help="Enable debug mode"
)
parser.add_argument(
    "--port-game",
    type=str,
    default=None,
    help="Serial port of the game Arduino (found automatically if not set)",
)
parser.add_argument(
    "--port-score",
    type=str,
    default=None,
    help="Serial port of the score display Arduino (found automatically if not set)",
)
parser.add_argument(
    "--port-clock",
    type=str,
    default=None,
    help="Serial port of the clock display Arduino (found automatically if not set)",
)
parser.add_argument(
    "--admin-port",
    type=int,
    default=8080,
    help="Port for the admin page, at http://localhost:<port>",
)

args = parser.parse_args()

TEST = args.test
DEBUG = args.debug

# How often the clock display is updated
CLOCK_TICK_SECS = 0.05

# A receiver that reads LOW this much of the time is stuck (unpowered or broken)
# and can't be the live target. Shots alone read up to ~60%. Matches
# IR_STUCK_LOW_PERCENT in game.ino.
IR_STUCK_LOW_PERCENT = 98
# How long a receiver counts as broken after a bad reading, so one that only
# works on and off doesn't flicker. Matches IR_RECOVERY_MS in game.ino.
IR_RECOVERY_SECS = 10
# The game board reports receiver health every second, but its animations can
# hold that up for a few seconds
IR_HEALTH_STALE_SECS = 6
NUM_TARGETS = 4
# The color game.ino lights each player's targets in versus mode
PLAYER_COLORS = ["orange", "blue"]

# Commands typed into the server, and the byte game.ino reads for each
COMMANDS = {"start": "s", "stop": "x", "showdown": "0", "versus": "1"}
COMMANDS_HELP = (
    "Type start, stop, showdown, versus, disable <target> or enable <target> "
    "and press Enter"
)
# game.ino reads this plus a bit mask of the enabled targets (bit 0 is target 1)
ENABLED_TARGETS_COMMAND = ord("@")

pygame.mixer.init()
background_game_music = pygame.mixer.Sound("audio/draw_screen_loop.ogg")
game_start = pygame.mixer.Sound("audio/game_start.ogg")
game_won = pygame.mixer.Sound("audio/game_won.ogg")
hit = pygame.mixer.Sound("audio/hit.ogg")
countdown = pygame.mixer.Sound("audio/countdown.ogg")


game = GameState()


def format_score(score: int) -> str:
    return f"{min(max(score, 0), 99999):05d}"


def format_targets_destroyed(counts: List[int]) -> str:
    """Each player's targets destroyed, e.g. "03 05" for orange 3, blue 5."""
    return " ".join(f"{min(count, 99):02d}" for count in counts)


def format_clock(secs: float) -> str:
    return f"{secs:05.2f}"


def handle_game_line(line: str) -> None:
    if DEBUG:
        print(f"Read: {line}")
    value_arr = line.split()
    if len(value_arr) > 1 and value_arr[0] == "EMIT":
        _, signal, *data = value_arr
        game.consume_signal(signal, data)


def log_display_line(line: str) -> None:
    if DEBUG:
        print(f"Display read: {line}")


def game_board_connected() -> None:
    # Opening the port resets the game Arduino, so any game in progress is over
    if game.game_active:
        game.consume_signal("GAME_INACTIVE", [])
    game.live_targets = []
    # The board forgets which targets were disabled when it resets
    send_enabled_targets()
    game.ir_low_percent, game.ir_broken_until = [], []


game_board = Board(
    "GAME",
    GAME_BAUDRATE,
    port=args.port_game,
    on_line=handle_game_line,
    on_connect=game_board_connected,
)
score_board = Board(
    "SCORE",
    DISPLAY_BAUDRATE,
    port=args.port_score,
    text=format_score(0),
    on_line=log_display_line,
)
clock_board = Board(
    "CLOCK",
    DISPLAY_BAUDRATE,
    port=args.port_clock,
    text=format_clock(0),
    on_line=log_display_line,
)
board_manager = BoardManager(
    [score_board, clock_board] if TEST else [game_board, score_board, clock_board]
)


def show_score(gs: GameState) -> None:
    # The score board has no room for both players' scores, so in versus mode it
    # shows how many targets each has destroyed. The admin page shows the scores.
    if gs.game_mode == "Versus":
        score_board.send_text(format_targets_destroyed(gs.targets_destroyed))
    else:
        score_board.send_text(format_score(gs.score))


@game.register_listener("SCORE")
def update_score(gs: GameState, data: List[str]) -> None:
    # In versus mode the numbers are orange's and blue's scores
    gs.score = int(data[0])
    gs.player2_score = int(data[1]) if len(data) > 1 else 0
    show_score(gs)


@game.register_listener("GAME_ACTIVE")
def activate_game(gs: GameState, data: List[str]) -> None:
    game_length = int(data[0])
    gs.start_game(game_length / 1000)
    show_score(gs)

    game_start.play()
    time.sleep(1)
    background_game_music.play(loops=-1)

    # logger.info(f"starting game with ms: {game_length}")


@game.register_listener("GAME_INACTIVE")
def end_game(gs: GameState, data: List[str]) -> None:
    gs.reset_game()
    background_game_music.stop()
    game_won.play()


def describe_target(gs: GameState, data: List[str]) -> str:
    """Name the target in a TARGET_* signal, whose data is its number and the player's."""
    target, player = int(data[0]), int(data[1])
    if gs.game_mode == "Versus":
        return f"Target {target} ({PLAYER_COLORS[player - 1]})"
    return f"Target {target}"


@game.register_listener("TARGET_HIT")
def target_hit(gs: GameState, data: List[str]) -> None:
    hit.play()
    print(f"{describe_target(gs, data)} hit")


@game.register_listener("TARGET_DESTROYED")
def target_destroyed(gs: GameState, data: List[str]) -> None:
    # The board sends the new SCORE right after, which updates the score board
    gs.targets_destroyed[int(data[1]) - 1] += 1
    print(f"{describe_target(gs, data)} destroyed")


@game.register_listener("LIVE_TARGETS")
def live_targets(gs: GameState, data: List[str]) -> None:
    gs.live_targets = [int(x) for x in data]


@game.register_listener("IR_RECEIVERS", log=False)
def ir_receivers(gs: GameState, data: List[str]) -> None:
    now = time.monotonic()
    gs.ir_low_percent = [int(x) for x in data]
    gs.ir_updated = now
    if len(gs.ir_broken_until) != len(gs.ir_low_percent):
        gs.ir_broken_until = [0.0] * len(gs.ir_low_percent)
    for i, low in enumerate(gs.ir_low_percent):
        if low >= IR_STUCK_LOW_PERCENT:
            gs.ir_broken_until[i] = now + IR_RECOVERY_SECS


@game.register_listener("GAME_MODE")
def change_game_mode(gs: GameState, data: List[str]) -> None:
    game_mode = int(data[0])
    hit.play()
    gs.game_mode = "Versus" if game_mode else "Showdown"
    print(gs.report())


def run_clock() -> None:
    """Drive the clock display and the final countdown beeps from the game timer."""
    while True:
        clock_board.send_text(format_clock(game.seconds_remaining()))
        if game.is_active():
            game.countdown(countdown)
        time.sleep(CLOCK_TICK_SECS)


def send_enabled_targets() -> bool:
    mask = sum(
        1 << (t - 1)
        for t in range(1, NUM_TARGETS + 1)
        if t not in game.disabled_targets
    )
    return game_board.send_command(chr(ENABLED_TARGETS_COMMAND + mask))


def set_target_enabled(target: int, enabled: bool) -> Optional[str]:
    if not 1 <= target <= NUM_TARGETS:
        return f"There's no target {target}"
    disabled = (
        game.disabled_targets - {target}
        if enabled
        else game.disabled_targets | {target}
    )
    if len(disabled) == NUM_TARGETS:
        return "At least one target has to stay enabled"
    game.disabled_targets = disabled
    print(f"Target {target} {'enabled' if enabled else 'disabled'}")
    # If the board isn't connected, it gets this when it connects
    send_enabled_targets()
    return None


def run_command(command: str) -> Optional[str]:
    """Send a typed or clicked command to the game board. Returns why it wasn't sent."""
    words = command.split()
    if len(words) == 2 and words[0] in ("enable", "disable") and words[1].isdigit():
        return set_target_enabled(int(words[1]), words[0] == "enable")
    if command not in COMMANDS:
        return COMMANDS_HELP
    if command == "start" and game.game_active:
        return "A game is already running"
    if command == "stop" and not game.game_active:
        return "No game is running"
    if command in ("showdown", "versus") and game.game_active:
        return "Stop the game before changing the mode"
    if not game_board.send_command(COMMANDS[command]):
        return "The game board isn't connected"
    return None


def ir_receiver_health() -> List[Dict[str, Any]]:
    """Each target's IR receiver health, from the game board's IR_RECEIVERS reports."""
    now = time.monotonic()
    fresh = game_board.connected and now - game.ir_updated < IR_HEALTH_STALE_SECS
    lows = game.ir_low_percent if fresh else []
    live = game.live_targets if game_board.connected else []
    receivers = []
    for i in range(max(NUM_TARGETS, len(lows))):
        low = lows[i] if i < len(lows) else None
        if low is None:
            health = "unknown"
        elif now < game.ir_broken_until[i]:
            # Unpowered receivers pull their output LOW
            health = "stuck"
        else:
            health = "working"
        receivers.append(
            {
                "target": i + 1,
                "health": health,
                "lowPercent": low,
                # e.g. shots landing on it, or a loose signal wire when nobody's shooting
                "seeingIr": bool(low),
                "enabled": i + 1 not in game.disabled_targets,
                # The player whose live target it is, numbered from 1
                "livePlayer": live.index(i + 1) + 1 if i + 1 in live else None,
            }
        )
    return receivers


def admin_status() -> Dict[str, Any]:
    return {
        "gameActive": game.game_active,
        "gameMode": game.game_mode,
        "score": game.score,
        "player2Score": game.player2_score,
        "playerColors": PLAYER_COLORS,
        "secondsRemaining": game.seconds_remaining(),
        "boards": [
            {"role": b.role, "connected": b.connected, "port": b.device}
            for b in (game_board, score_board, clock_board)
        ],
        "unusedPorts": [
            {"port": port, "problem": problem}
            for port, problem in sorted(board_manager.unused_ports.items())
        ],
        "irReceivers": ir_receiver_health(),
    }


def read_stdin() -> None:
    print(COMMANDS_HELP)
    while True:
        try:
            line = input().strip()
        except EOFError:
            return
        if line.startswith("EMIT"):
            # Act as if the game board sent this, for testing without it
            handle_game_line(line)
        elif line:
            error = run_command(line.lower())
            if error:
                print(error)


if __name__ == "__main__":
    board_manager.start()
    threading.Thread(target=run_clock, daemon=True).start()

    try:
        # Only this computer can open it, so nobody else on the network can start games
        admin = make_admin_server(
            "127.0.0.1", args.admin_port, admin_status, run_command
        )
        threading.Thread(target=admin.serve_forever, daemon=True).start()
        print(f"Admin page: http://localhost:{args.admin_port}")
    except OSError as e:
        print(
            f"Couldn't start the admin page on port {args.admin_port} ({e}), try --admin-port"
        )

    # Read stdin on the main thread so Ctrl+C exits cleanly
    try:
        read_stdin()
        if not TEST:
            # Commands can't be typed any more, but the game can still run
            threading.Event().wait()
    except KeyboardInterrupt:
        pass
