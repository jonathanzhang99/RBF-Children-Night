import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Callable, Dict, List, Optional, Set, Tuple, Union

import serial
import serial.tools.list_ports

# Opening a port resets the Uno, and every sketch prints "HELLO <ROLE>" at this
# baud rate as it boots. That's how ports are matched to boards.
HELLO_BAUDRATE = 115200
HELLO_TIMEOUT_SECS = 4

GAME_BAUDRATE = 115200
DISPLAY_BAUDRATE = 9600

READ_TIMEOUT_SECS = 0.5
RESCAN_SECS = 2

# Why a plugged-in port isn't being used
PORT_BUSY = "couldn't be opened. Is the Arduino IDE's Serial Monitor open on it?"
NO_HELLO = (
    "didn't identify itself. Flash it with the game or display sketch, then replug it."
)


class Board:
    """One Arduino, found by the HELLO line it prints on boot and reconnected if unplugged."""

    def __init__(
        self,
        role: str,
        baudrate: int,
        port: Optional[str] = None,
        text: Optional[str] = None,
        on_line: Optional[Callable[[str], None]] = None,
        on_connect: Optional[Callable[[], None]] = None,
    ):
        self.role = role
        self.baudrate = baudrate
        # Fixed port to use instead of searching all of them
        self.port = port
        # What a display board should show, resent whenever it reconnects
        self.text = text
        self.on_line = on_line
        self.on_connect = on_connect
        self._serial: Optional[serial.Serial] = None
        self._sent_text: Optional[str] = None
        self._lock = threading.RLock()

    @property
    def connected(self) -> bool:
        return self._serial is not None

    @property
    def device(self) -> Optional[str]:
        ser = self._serial
        return ser.port if ser else None

    def attach(self, ser: serial.Serial) -> None:
        ser.baudrate = self.baudrate
        ser.timeout = READ_TIMEOUT_SECS
        with self._lock:
            self._serial = ser
            self._sent_text = None
        print(f"{self.role} board connected on {ser.port}")
        if self.on_connect:
            self.on_connect()
        if self.text is not None:
            self.send_text(self.text)

    def _detach(self, ser: serial.Serial, error: Exception) -> None:
        with self._lock:
            if self._serial is not ser:
                return
            self._serial = None
        try:
            ser.close()
        except (serial.SerialException, OSError):
            pass
        print(f"{self.role} board disconnected ({error}), waiting for it to come back")

    def send_text(self, text: str) -> None:
        """Show a line of text on a display board."""
        with self._lock:
            self.text = text
            ser = self._serial
            if ser is None or text == self._sent_text:
                return
            try:
                ser.write(f"{text}\n".encode())
                self._sent_text = text
            except (serial.SerialException, OSError) as e:
                self._detach(ser, e)

    def send_command(self, command: str) -> bool:
        """Send a command to the game board. Returns whether it was sent."""
        with self._lock:
            ser = self._serial
            if ser is None:
                return False
            try:
                ser.write(command.encode())
                return True
            except (serial.SerialException, OSError) as e:
                self._detach(ser, e)
                return False

    def read_line(self) -> str:
        ser = self._serial
        if ser is None:
            time.sleep(READ_TIMEOUT_SECS)
            return ""
        try:
            return ser.readline().decode("utf-8", errors="replace").strip()
        except (serial.SerialException, OSError) as e:
            self._detach(ser, e)
            return ""

    def run_reader(self) -> None:
        while True:
            line = self.read_line()
            if line and self.on_line:
                self.on_line(line)


def probe(device: str) -> Union[Tuple[str, serial.Serial], str]:
    """Open a port, which resets the board on it, and wait for its HELLO line.

    Returns the board's role and the open port, or why it couldn't."""
    try:
        ser = serial.Serial(device, HELLO_BAUDRATE, timeout=READ_TIMEOUT_SECS)
    except (serial.SerialException, OSError):
        return PORT_BUSY

    deadline = time.monotonic() + HELLO_TIMEOUT_SECS
    try:
        while time.monotonic() < deadline:
            parts = ser.readline().decode("utf-8", errors="replace").split()
            if len(parts) == 2 and parts[0] == "HELLO":
                return parts[1], ser
    except (serial.SerialException, OSError):
        pass
    ser.close()
    return NO_HELLO


class BoardManager:
    """Connects each board to its port, reconnects any that drop and runs their readers."""

    def __init__(self, boards: List[Board]):
        self.boards = boards
        # Ports with no HELLO or a board that isn't needed, and why. They're
        # skipped until unplugged so they aren't reset over and over.
        self._ignored_ports: Dict[str, str] = {}
        # Ports that couldn't be opened, which are tried again on every scan
        self._busy_ports: Set[str] = set()
        self._last_missing: List[str] = []

    @property
    def unused_ports(self) -> Dict[str, str]:
        """Plugged-in ports that aren't being used, and why."""
        unused = {device: PORT_BUSY for device in self._busy_ports}
        unused.update(self._ignored_ports)
        return unused

    def start(self) -> None:
        for board in self.boards:
            threading.Thread(target=board.run_reader, daemon=True).start()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        while True:
            # A port's device file disappears when it's unplugged. The admin page
            # reads these from another thread, so they're replaced, not changed.
            self._ignored_ports = {
                d: why for d, why in self._ignored_ports.items() if os.path.exists(d)
            }
            missing = [b for b in self.boards if not b.connected]
            # Busy ports only matter while a board is still missing
            self._busy_ports = (
                {d for d in self._busy_ports if os.path.exists(d)} if missing else set()
            )
            if missing:
                self._connect(missing)

            missing_roles = [b.role for b in self.boards if not b.connected]
            if missing_roles and missing_roles != self._last_missing:
                print(f"Waiting for boards: {', '.join(missing_roles)}")
            self._last_missing = missing_roles
            time.sleep(RESCAN_SECS)

    def _connect(self, missing: List[Board]) -> None:
        usb_ports = {
            p.device for p in serial.tools.list_ports.comports() if p.vid is not None
        }
        fixed_ports = {b.port for b in self.boards if b.port}
        in_use = {b.device for b in self.boards if b.device}

        to_probe = {b.port for b in missing if b.port}
        if any(b.port is None for b in missing):
            to_probe |= usb_ports - fixed_ports
        to_probe = {
            d
            for d in to_probe - in_use - self._ignored_ports.keys()
            if d and os.path.exists(d)
        }
        if not to_probe:
            return

        devices = sorted(to_probe)
        with ThreadPoolExecutor(max_workers=len(devices)) as pool:
            results = list(pool.map(probe, devices))

        ignored, busy = dict(self._ignored_ports), set(self._busy_ports)
        for device, result in zip(devices, results):
            if result == PORT_BUSY:
                if device not in busy:
                    print(f"{device} {PORT_BUSY}")
                busy.add(device)
                continue
            busy.discard(device)
            if isinstance(result, str):
                print(f"{device} {result}")
                ignored[device] = result
                continue

            role, ser = result
            board = next(
                (
                    b
                    for b in missing
                    if b.role == role and not b.connected and b.port in (None, device)
                ),
                None,
            )
            if board:
                board.attach(ser)
            else:
                # e.g. both displays flashed as SCORE
                why = (
                    f"is a second {role.lower()} board, which isn't needed. Reflash it as "
                    "the board it should be (make flash-game, flash-score or flash-clock), "
                    "then replug it."
                )
                print(f"{device} {why}")
                ser.close()
                ignored[device] = why
        self._ignored_ports, self._busy_ports = ignored, busy
