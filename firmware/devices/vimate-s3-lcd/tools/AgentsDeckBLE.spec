# AgentsDeckBLE.app: the menu bar bridge (ble_menu.py) with the Info.plist macOS needs for
# Bluetooth. From this folder: pyinstaller AgentsDeckBLE.spec (macOS arm64).
from pathlib import Path
here = Path(SPECPATH)
# bin/agentctl: built from backend/ (go build -o tools/bin/agentctl ./cmd/agentctl),
# installed by the app on a Mac that doesn't have it.
a = Analysis([str(here / "ble_menu.py")], pathex=[str(here)],
             binaries=[(str(here / "bin/agentctl"), "bin")],
             hiddenimports=["bleak.backends.corebluetooth"], noarchive=False)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="AgentsDeckBLE",
          console=False, target_arch="arm64", upx=False)
coll = COLLECT(exe, a.binaries, a.datas, upx=False, name="AgentsDeckBLE")
app = BUNDLE(coll, name="AgentsDeckBLE.app", bundle_identifier="dev.agents-deck.ble-bridge",
             info_plist={"NSBluetoothAlwaysUsageDescription": "Agents Deck nối màn hình agent qua Bluetooth.",
                         "LSUIElement": True, "CFBundleShortVersionString": "0.1.0"})
