"""AgentsDeckBLE: the Bluetooth bridge (ble_bridge.py) with a menu bar icon.
Click it for the boards around, pick one to connect to; the pick is kept for
next time. "Ngắt kết nối" lets go of the board, for another Mac to take it.

On a Mac without the deck it sets it up: the agentctl inside the app goes to
~/.local/bin and starts the daemon, and the Bluetooth port is registered.
From the menu: approve the connected screen, put open Claude sessions on it."""
import asyncio
import filecmp
import shutil
import subprocess
import sys
import threading
from pathlib import Path

import AppKit
import objc
from PyObjCTools import AppHelper

import ble_bridge as bridge

CTL = bridge.Control()
LINK = Path.home() / ".agents-deck-ble"
AGENTCTL = Path.home() / ".local/bin/agentctl"
BUNDLED = Path(getattr(sys, "_MEIPASS", Path(__file__).parent)) / "bin/agentctl"


def agentctl(*args):
    """Runs agentctl; its output, "" if it fails."""
    try:
        return subprocess.run([str(AGENTCTL), *args], capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.TimeoutExpired) as error:
        print("agentctl", *args, "failed:", error, flush=True)
        return ""


def set_up():
    """Installs the bundled agentctl (and its login item) unless this one is
    already there, and registers the Bluetooth port with the daemon."""
    if BUNDLED.exists() and not (AGENTCTL.exists() and filecmp.cmp(BUNDLED, AGENTCTL, shallow=False)):
        AGENTCTL.parent.mkdir(parents=True, exist_ok=True)
        tmp = AGENTCTL.with_suffix(".new")
        shutil.copy2(BUNDLED, tmp)
        tmp.replace(AGENTCTL)
        print("installed agentctl:", agentctl("install").strip(), flush=True)
    if str(LINK) not in agentctl("link"):
        print(agentctl("link", "add", str(LINK)).strip(), flush=True)


def deck_device():
    """The deck's name for the connected board, and whether it's approved
    with act scope."""
    if not CTL.connected:
        return None, False
    suffix = "-" + CTL.connected.split("-")[-1].lower()  # AgentsDeck-e8df48 -> -e8df48
    for row in agentctl("devices").splitlines():
        cols = row.split()
        if cols and cols[0].endswith(suffix):
            return cols[0], len(cols) > 3 and cols[2] == "act" and "approval" not in row
    return None, False


def open_sessions():
    """Claude sessions started by hand that aren't on the deck yet."""
    rows = agentctl("live").splitlines()[1:]
    return [r.split()[0] for r in rows if r.split() and r.split()[-1] == "-"]


def signal(rssi):
    if rssi is None:
        return ""
    return "  ▂▄▆█"[:2 + (rssi > -90) + (rssi > -75) + (rssi > -60)]


class App(AppKit.NSObject):
    def applicationDidFinishLaunching_(self, _):
        self.item = AppKit.NSStatusBar.systemStatusBar().statusItemWithLength_(AppKit.NSVariableStatusItemLength)
        icon = AppKit.NSImage.imageWithSystemSymbolName_accessibilityDescription_(
            "antenna.radiowaves.left.and.right", "Agents Deck")
        self.item.button().setImage_(icon)
        self.menu = AppKit.NSMenu.alloc().init()
        self.menu.setAutoenablesItems_(False)
        self.menu.setDelegate_(self)
        self.item.setMenu_(self.menu)
        AppKit.NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(1, self, "tick:", None, True)
        self.tick_(None)
        threading.Thread(target=self.start, daemon=True).start()

    @objc.python_method
    def start(self):
        set_up()
        asyncio.run(bridge.run(CTL, LINK))

    def tick_(self, _):  # dim icon while not connected
        self.item.button().setAppearsDisabled_(CTL.connected is None)

    @objc.python_method
    def add(self, title, action=None, obj=None, on=False):
        item = self.menu.addItemWithTitle_action_keyEquivalent_(title, action, "")
        item.setTarget_(self)
        item.setEnabled_(action is not None)
        item.setRepresentedObject_(obj)
        item.setState_(AppKit.NSControlStateValueOn if on else AppKit.NSControlStateValueOff)

    def menuNeedsUpdate_(self, menu):
        menu.removeAllItems()
        if CTL.paused:
            self.add("Đã ngắt kết nối")
        elif CTL.connected:
            self.add(f"Đã nối: {CTL.connected}")
            name, ok = deck_device()
            if name and not ok:
                self.add("Duyệt màn hình này", "approve:", name)
        else:
            self.add(f"Đang tìm {CTL.target or 'màn hình'}…")
        sessions = open_sessions()
        if sessions:
            self.add(f"Đưa {len(sessions)} phiên Claude lên màn", "addSessions:", sessions)
        menu.addItem_(AppKit.NSMenuItem.separatorItem())
        self.add("Màn hình quanh đây:" if CTL.found else "Chưa thấy màn hình nào")
        for name, rssi in sorted(CTL.found.items()):
            self.add(f"{name}   {signal(rssi)}", "choose:", name, on=name == CTL.target)
        self.add("Quét lại", "rescan:")
        menu.addItem_(AppKit.NSMenuItem.separatorItem())
        self.add("Kết nối lại" if CTL.paused else "Ngắt kết nối", "toggle:")
        self.add("Thoát", "quit:")

    def choose_(self, sender):
        CTL.choose(sender.representedObject())

    def approve_(self, sender):
        name = sender.representedObject()
        agentctl("devices", "approve", name)
        agentctl("devices", "scope", name, "act")

    def addSessions_(self, sender):
        for session in sender.representedObject():
            agentctl("add", session)

    def rescan_(self, _):
        CTL.rescan = True

    def toggle_(self, _):
        CTL.paused = not CTL.paused

    def quit_(self, _):
        AppKit.NSApp.terminate_(None)


def main():
    bridge.log_to_file()
    app = AppKit.NSApplication.sharedApplication()
    app.setActivationPolicy_(AppKit.NSApplicationActivationPolicyAccessory)  # menu bar only
    delegate = App.alloc().init()
    app.setDelegate_(delegate)
    AppHelper.runEventLoop()


if __name__ == "__main__":
    main()
