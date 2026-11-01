from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import Callable, Dict, List, Literal, Set

import pygame.mixer

F = Callable[["GameState", List[str]], None]

# The game Arduino pauses for a second after a game starts before targets go
# live, and ends the game a second late to make up for it (MS_OFFSET in game.ino)
START_DELAY_SECS = 1


@dataclass
class GameState:
    start_time: datetime = field(default_factory=datetime.now)
    game_length_secs: float = 60
    current_countdown: int = 5
    game_active: bool = False
    game_mode: Literal["Showdown", "Versus"] = "Showdown"
    score: int = 0
    # Only used in versus mode, where score is orange's and this is blue's
    player2_score: int = 0
    # How many targets each player has destroyed this game
    targets_destroyed: List[int] = field(default_factory=lambda: [0, 0])
    # Each player's live target, numbered from 1, or 0 while their last one shows
    # it was destroyed. Showdown has one player and versus has two.
    live_targets: List[int] = field(default_factory=list)
    # Targets turned off from the admin page, numbered from 1
    disabled_targets: Set[int] = field(default_factory=set)
    # How much of the last second each target's IR receiver read LOW, 0-100, and
    # when that arrived (time.monotonic())
    ir_low_percent: List[int] = field(default_factory=list)
    ir_updated: float = 0.0
    # Until when each receiver counts as broken after a bad reading
    ir_broken_until: List[float] = field(default_factory=list)
    listeners: Dict[str, F] = field(default_factory=dict)
    # Signals that arrive too often to log
    quiet_signals: Set[str] = field(default_factory=set)

    @property
    def end_time(self) -> datetime:
        return self.start_time + timedelta(
            seconds=START_DELAY_SECS + self.game_length_secs
        )

    def reset_game(self) -> None:
        self.game_active = False
        self.current_countdown = 5

    def start_game(self, game_length_secs: float) -> None:
        self.start_time = datetime.now()
        self.game_length_secs = game_length_secs
        self.game_active = True
        self.current_countdown = 5
        self.score = 0
        self.player2_score = 0
        self.targets_destroyed = [0, 0]

    def is_active(self) -> bool:
        return self.game_active and datetime.now() < self.end_time

    def seconds_remaining(self) -> float:
        """Play time left, holding at the full length during the start pause."""
        if not self.game_active:
            return 0.0
        remaining = (self.end_time - datetime.now()).total_seconds()
        return max(0.0, min(self.game_length_secs, remaining))

    def countdown(self, countdown_sound: pygame.mixer.Sound) -> None:
        if self.seconds_remaining() <= self.current_countdown:
            self.current_countdown -= 1
            countdown_sound.play()

    def register_listener(self, signal: str, log: bool = True) -> Callable[[F], F]:
        def wrapper(f: F) -> F:
            self.listeners[signal] = f
            if not log:
                self.quiet_signals.add(signal)
            return f

        return wrapper

    def consume_signal(self, signal: str, data: List[str]) -> None:
        handler = self.listeners.get(signal)
        if not handler:
            print(f"ERROR: No handler for signal {signal}")
            return
        elif signal not in self.quiet_signals:
            print(f"CONSUME: {signal} {data}")
        try:
            handler(self, data)
        except Exception as e:
            print(f"ERROR: Handling {signal} failed: {e}")

    def report(self):
        return {
            "score": self.score,
            "gameMode": self.game_mode,
            "gameActive": self.game_active,
        }
