"""Deck Link over Bluetooth LE: hands the board to the agents deck daemon as a
serial port.

The board advertises "AgentsDeck-<id>" with a Nordic-UART-style service. This
opens a pseudo-terminal, points a fixed path at it, and copies bytes between
the pty and the board. Register the path once:

    agentctl link add ~/.agents-deck-ble

Run it from a terminal (python3 tools/ble_bridge.py) or as the menu bar app
(ble_menu.py, see README); macOS asks once for Bluetooth access. Needs bleak (pip install bleak).
"""
import argparse
import asyncio
import json
import os
import tty
from pathlib import Path

SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # we write the deck's bytes here
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # the board's bytes come as notifications
PREFIX = "AgentsDeck-"


def open_port(link):
    """A pty whose slave the daemon opens through link. Only the daemon may
    hold the slave (it refuses a port someone else has open)."""
    master, slave = os.openpty()
    tty.setraw(slave)  # no echo before the daemon sets it up
    name = os.ttyname(slave)
    os.close(slave)
    os.set_blocking(master, False)
    tmp = link.with_suffix(".tmp")
    tmp.unlink(missing_ok=True)
    tmp.symlink_to(name)
    tmp.replace(link)
    return master, name


class Control:
    """What the menu bar shows and asks for (ble_menu.py); the bridge reads and
    updates it. The board picked last is kept in CHOICE."""
    CHOICE = Path.home() / ".agents-deck-ble.json"

    def __init__(self, name=None):
        if not name and self.CHOICE.exists():
            name = json.loads(self.CHOICE.read_text()).get("name")
        self.target = name     # the board to connect to; None: the only one around
        self.found = {}        # name -> RSSI, from the last scan
        self.connected = None  # the board we're connected to
        self.paused = False    # "disconnect": leave the board to another Mac
        self.rescan = False

    def choose(self, name):
        self.target, self.paused = name, False
        self.CHOICE.write_text(json.dumps({"name": name}))


async def scan(ctl):
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=5, return_adv=True)
    boards = {adv.local_name: (d, adv.rssi) for d, adv in found.values()
              if (adv.local_name or "").startswith(PREFIX)}
    ctl.found = {n: rssi for n, (_, rssi) in boards.items()}
    if ctl.connected:  # it doesn't advertise while connected
        ctl.found.setdefault(ctl.connected, None)
    ctl.rescan = False
    return {n: d for n, (d, _) in boards.items()}


async def run(ctl, link):
    from bleak import BleakClient
    master, name = open_port(link)
    print(f"port {link} -> {name}", flush=True)
    while True:
        if ctl.paused:
            await asyncio.sleep(0.5)
            continue
        try:
            boards = await scan(ctl)
            pick = ctl.target if ctl.target in boards else None
            if not ctl.target and len(boards) == 1:
                pick = next(iter(boards))
                ctl.choose(pick)
            if not pick:
                print(f"looking for {ctl.target or 'a board'}: found {len(boards)}", flush=True)
                await asyncio.sleep(2)
                continue
            async with BleakClient(boards[pick], timeout=20) as client:
                rx = client.services.get_characteristic(RX)
                chunk = max(20, rx.max_write_without_response_size)
                print(f"connected to {pick} ({chunk}-byte writes)", flush=True)

                def from_board(_, data):
                    try:
                        os.write(master, data)
                    except BlockingIOError:
                        pass  # nobody reading the port: drop, Deck Link resyncs

                await client.start_notify(TX, from_board)
                ctl.connected = pick
                # ponytail: polls the pty every 20 ms (EIO while the daemon has
                # it closed rules out add_reader); a select loop if latency matters
                while client.is_connected and not ctl.paused and ctl.target == pick:
                    if ctl.rescan:
                        asyncio.ensure_future(scan(ctl))
                        ctl.rescan = False
                    try:
                        data = os.read(master, 4096)
                    except OSError:  # nothing yet, or no daemon on the port
                        await asyncio.sleep(0.02)
                        continue
                    for i in range(0, len(data), chunk):
                        await client.write_gatt_char(rx, data[i:i + chunk], response=False)
            print("disconnected", flush=True)
        except asyncio.CancelledError:
            raise
        except Exception as error:
            print("reconnecting:", type(error).__name__, error, flush=True)
            await asyncio.sleep(3)
        finally:
            ctl.connected = None


def log_to_file():
    """Started as an app: no terminal, so keep a log."""
    if not os.isatty(1):
        log = open(Path.home() / "Library/Logs/AgentsDeckBLE.log", "a", buffering=1)
        os.dup2(log.fileno(), 1)
        os.dup2(log.fileno(), 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", help="the board's BLE name, e.g. AgentsDeck-b28e1c (needed if several are near)")
    parser.add_argument("--link", type=Path, default=Path.home() / ".agents-deck-ble",
                        help="the path to register with agentctl link add")
    args = parser.parse_args()
    log_to_file()
    try:
        asyncio.run(run(Control(args.name), args.link))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
