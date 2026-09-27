#!/usr/bin/env python3
"""Validate the installer board catalog (mooninstaller/deviceModels.json).

The catalog is hand-maintained data consumed identically by three clients (the
web installer, the device UI's ?deviceModel= inject, and MoonDeck), so a typo drifts
silently — a broken image path, a board name that no longer matches its
System.deviceModel control, a driver pin list on an entry that has no driver. This is the
catalog's counterpart to check_specs.py for module docs: a fast, dependency-free
gate that pins the invariants the clients assume.

Invariants checked per entry:
  - required fields present (name, chip, firmwares, modules)
  - firmwares is a non-empty list of non-empty strings (entry[0] is the default)
  - image (if set) is a local assets/deviceModels/ path that resolves on disk
  - url (if set) is an absolute http(s) link
  - the System module's `deviceModel` control value equals the entry `name`
  - every module `type` is factory-registered (or a known boot-wired singleton)
  - a driver's `pins` control only appears on an actual *LedDriver module
  - supported/planned (if set) are string arrays drawn from the known vocabulary

Exit 1 on any error, mirroring check_specs.py.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CATALOG = ROOT / "mooninstaller" / "deviceModels.json"
MODULE_TYPES_CPP = ROOT / "src" / "module_types.cpp"
NETWORK_MODULE = ROOT / "src" / "core" / "system" / "NetworkModule.h"
PLATFORM_CONFIG = ROOT / "src" / "platform" / "esp32" / "platform_config.h"
DOCS = ROOT / "docs"

# The device stores the injected deviceModel in SystemModule's deviceModel_[32] buffer (31 usable
# chars + NUL); a longer catalog name is truncated on-device and stops matching. Keep in step with
# that buffer in src/core/system/SystemModule.h.
DEVICE_MODEL_MAX = 31

# Capability vocabulary. supported = what a module drives today; keep this list
# in lockstep with the modules that actually exist. planned = peripherals with no
# module yet (the backlog seed) — open-ended by design, so it is NOT whitelisted,
# only type-checked. Adding a new supported capability means a module backs it.
SUPPORTED_VOCAB = {"LEDs", "WiFi", "Ethernet", "Audio", "IR", "MQTT", "Hue"}

# Flash bauds a board may pin via `flashBaud` — the standard esptool rates. The default
# differs by audience: the CLI / MoonDeck path defaults FAST (921600 — DIY bench, modern
# bridge), the web installer defaults SAFE (460800 — unknown walk-up hardware). A board
# sets `flashBaud` to override its resolved default in either direction — down for a flaky
# bridge (the LOLIN's CH340), up where a slow default needs raising. Keep in step with
# flash_esp32.py (_catalog_flash_baud) and install-orchestrator.js.
FLASH_BAUDS = {115200, 230400, 460800, 921600}

# Boot-wired singletons: present on every device, added by code, so the catalog
# references them by id without the factory creating them. Their catalog `type`
# is the short id, not the factory class name (e.g. "System", not "SystemModule").
BOOT_WIRED_TYPES = {"System", "Network", "Drivers"}


def eth_preset_labels():
    """The Ethernet preset labels from NetworkModule.h's kEthPresets table.

    Read from the firmware rather than restated here, so a preset renamed in one place and not the
    other fails this check instead of silently leaving a catalog entry pointing at nothing.
    """
    text = NETWORK_MODULE.read_text(encoding="utf-8")
    table = re.search(r"kEthPresets\[\]\s*=\s*\{(.*?)\n    \};", text, re.S)
    if not table:
        return set()
    return set(re.findall(r'^\s*\{"([^"]+)"', table.group(1), re.M))


def eth_preset_drift():
    """Preset rows that no longer match the per-chip default they restate, as (label, field, was, now).

    Three presets ARE a chip's `ethConfigDefault`, so the same pin map has two homes in two layers.
    They cannot simply reference each other: the header's value is a compile-time `constexpr` chosen
    per chip, while the table must carry every chip's row on every build. So the duplication stands
    and this check binds it, because the drift is silent where it matters most: `seedEthPresetFromPins`
    matches on exact equality, so one corrected pin would reseed every provisioned board to Custom.
    """
    table = NETWORK_MODULE.read_text(encoding="utf-8")
    header = PLATFORM_CONFIG.read_text(encoding="utf-8")
    block = re.search(r"kEthPresets\[\]\s*=\s*\{(.*?)\n    \};", table, re.S)
    if not block:
        return [("kEthPresets", "table", "present", "unreadable")]

    rows = {}
    for m in re.finditer(r'\{"([^"]+)",\s*([^}]+)\}', block.group(1)):
        rows[m.group(1)] = [f.strip() for f in m.group(2).split(",")]

    # label -> the PHY constant naming that chip's arm of the ethConfigDefault ternary.
    PAIRS = {"Classic RMII": "ethLan8720", "P4-NANO": "ethIp101", "S31 CoreBoard": "ethYt8531"}
    # Every field `seedEthPresetFromPins` compares, or a drift it ignores reseeds a board to Custom.
    FIELDS = ["phyAddr", "mdc", "mdio", "rst", "rmiiClk", "rmiiClockExtIn"]
    # The preset table writes the PHY as its enum VALUE and the header names the constant.
    PHY_VALUE = {"ethLan8720": "1", "ethIp101": "2", "ethW5500": "3", "ethYt8531": "4"}
    drift = []
    for label, phy in PAIRS.items():
        row = rows.get(label)
        if not row:
            drift.append((label, "row", "present", "missing"))
            continue
        m = re.search(phy + r", /\*addr\*/ (-?\d+), /\*mdc\*/ (-?\d+), /\*mdio\*/ (-?\d+),"
                      r"\s*/\*rst\*/ (-?\d+), /\*rmiiClk\*/ (-?\d+), /\*extIn\*/ (\w+)", header)
        if not m:
            drift.append((label, phy, "an ethConfigDefault arm", "not found"))
            continue
        # The PHY the preset selects must be the one whose default arm it restates.
        if row[0] != PHY_VALUE.get(phy, row[0]):
            drift.append((label, "phyType", PHY_VALUE[phy], row[0]))
        # row is label-less here: [type, phyAddr, mdc, mdio, rst, rmiiClk, extIn, ...]
        for i, field in enumerate(FIELDS):
            want, got = m.group(i + 1), row[i + 1]
            if want != got:
                drift.append((label, field, want, got))
    return drift


def registered_types():
    """The set of factory type names, read from the one file that registers them.

    `module_types.cpp` rather than main.cpp: the registry moved there so the firmware, the
    scenario runner and the ESP32 build share one list instead of drifting apart, and a checker
    reading the old home would report every type as missing.
    """
    text = MODULE_TYPES_CPP.read_text(encoding="utf-8")
    return set(re.findall(r'registerType<[^>]+>\("([^"]+)"', text))


def main():
    errors = []
    eth_presets = eth_preset_labels()
    for label, field, was, now in eth_preset_drift():
        errors.append(f"NetworkModule.h kEthPresets {label!r}: {field} is {now}, but "
                      f"platform_config.h's ethConfigDefault says {was}: the preset restates the "
                      f"chip default, so the two must agree or a provisioned board reseeds to Custom")
    if not eth_presets:
        errors.append("NetworkModule.h: could not read the kEthPresets table, so the ethBoard check cannot run")

    try:
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        print(f"Board check: cannot read {CATALOG.relative_to(ROOT)}: {e}")
        sys.exit(1)

    if not isinstance(catalog, list):
        print("Board check: deviceModels.json must be a JSON array")
        sys.exit(1)

    factory_types = registered_types()
    valid_types = factory_types | BOOT_WIRED_TYPES
    names_seen = set()

    for i, e in enumerate(catalog):
        where = f"entry {i}"
        if isinstance(e, dict) and "name" in e:
            where = f'"{e["name"]}"'

        if not isinstance(e, dict):
            errors.append(f"{where}: not a JSON object")
            continue

        # --- required fields ---
        for field in ("name", "chip", "firmwares", "modules"):
            if field not in e:
                errors.append(f"{where}: missing required field '{field}'")

        name = e.get("name")
        if name in names_seen:
            errors.append(f"{where}: duplicate board name")
        names_seen.add(name)
        # The name is injected into the device's SystemModule.deviceModel control, whose buffer is
        # deviceModel_[32] (31 chars + NUL). A longer name is silently truncated on-device, so it no
        # longer matches the catalog — MoonDeck then can't map it back and shows a duplicate
        # "(unknown)" entry. Cap the source data so it always round-trips whole.
        if isinstance(name, str) and len(name) > DEVICE_MODEL_MAX:
            errors.append(f"{where}: name is {len(name)} chars; max {DEVICE_MODEL_MAX} "
                          f"(SystemModule.deviceModel_[32] truncates longer names on-device)")

        # --- firmwares (firmwares[0] is the default the picker pre-selects) ---
        fws = e.get("firmwares")
        if not isinstance(fws, list):
            errors.append(f"{where}: firmwares must be a list, got {type(fws).__name__}")
        elif not fws:
            errors.append(f"{where}: firmwares must be a non-empty list (entry[0] is the default)")
        else:
            non_str = [f for f in fws if not isinstance(f, str)]
            if non_str:
                errors.append(f"{where}: firmwares entries must be strings, got "
                              f"{[type(f).__name__ for f in non_str]}")
            elif any(not f for f in fws):
                errors.append(f"{where}: firmwares entries must be non-empty strings")
            elif any(f != f.strip() or not f.strip() for f in fws):
                # Whitespace-only or padded keys (" ", "esp32 ") silently miss the
                # exact firmware-key match downstream (picker, dedup, manifest names).
                errors.append(f"{where}: firmwares entries must not be whitespace-only "
                              f"or have leading/trailing whitespace")

        # --- flashBaud (optional) — a board opts into a faster flash baud only when
        #     its USB bridge is verified to sustain it (flash_esp32.py reads this). ---
        baud = e.get("flashBaud")
        # `bool` is an int subclass and `1.0 in {int}` is True, so guard the type
        # explicitly — a float/bool flashBaud would stringify wrong for esptool.
        if baud is not None and (type(baud) is not int or baud not in FLASH_BAUDS):
            errors.append(f"{where}: flashBaud must be one of {sorted(FLASH_BAUDS)}, got {baud!r}")

        # --- image resolves on disk + is a local path ---
        img = e.get("image")
        if img is not None:
            if not isinstance(img, str) or not img.startswith("assets/deviceModels/"):
                errors.append(f"{where}: image must be a local 'assets/deviceModels/...' path, got {img!r}")
            elif not (DOCS / img).exists():
                errors.append(f"{where}: image '{img}' does not exist on disk")

        # --- url is an absolute http(s) link ---
        url = e.get("url")
        if url is not None and not (isinstance(url, str) and url.startswith(("http://", "https://"))):
            errors.append(f"{where}: url must be an absolute http(s) link, got {url!r}")

        # --- capability fields ---
        for cap_field, whitelist in (("supported", SUPPORTED_VOCAB), ("planned", None)):
            caps = e.get(cap_field)
            if caps is None:
                continue
            if not isinstance(caps, list) or not all(isinstance(c, str) for c in caps):
                errors.append(f"{where}: {cap_field} must be a list of strings")
                continue
            if whitelist is not None:
                for c in caps:
                    if c not in whitelist:
                        errors.append(f"{where}: supported capability '{c}' is not in the "
                                      f"known vocabulary {sorted(whitelist)} — add a module first")

        # --- modules ---
        mods = e.get("modules")
        if not isinstance(mods, list):
            # `modules` is required (presence checked above); a wrong *type* is a
            # schema violation, not something to skip silently.
            errors.append(f"{where}: modules must be a list, got {type(mods).__name__}")
            continue
        board_control_seen = False
        for m in mods:
            if not isinstance(m, dict):
                errors.append(f"{where}: a modules entry is not an object")
                continue
            mtype = m.get("type")
            mid = m.get("id")
            if not mtype:
                errors.append(f"{where}: a modules entry has no 'type'")
            elif mtype not in valid_types:
                errors.append(f"{where}: module type '{mtype}' is not factory-registered "
                              f"(and not a boot-wired singleton)")
            if not isinstance(mid, str) or not mid:
                errors.append(f"{where}: module '{mtype}' has no non-empty 'id'")

            # replaceChildren (optional) — a container unit sets it true to clear its
            # existing children before the entry's children are added (the installer
            # inject path). Must be a bool when present so a typo'd value is caught here.
            if "replaceChildren" in m and not isinstance(m["replaceChildren"], bool):
                errors.append(f"{where}: module '{mid}' replaceChildren must be true/false")

            controls = m.get("controls") or {}
            # System.deviceModel control must equal the entry name (the identity key).
            if mtype == "System" and isinstance(controls, dict) and "deviceModel" in controls:
                board_control_seen = True
                if controls["deviceModel"] != name:
                    errors.append(f"{where}: System.deviceModel control '{controls['deviceModel']}' "
                                  f"!= entry name '{name}'")
            # A `pins` control only makes sense on an LED driver module.
            if isinstance(controls, dict) and "pins" in controls and not str(mtype).endswith("LedDriver"):
                errors.append(f"{where}: module '{mtype}' has a 'pins' control but is not a *LedDriver")

            # 74HCT595 pin expander (pinExpander = is the board fitted?). The wiring
            # invariants are what a bad catalog entry gets wrong, and they fail on a bench with dark
            # LEDs rather than loudly, so pin them here.
            if isinstance(controls, dict) and controls.get("pinExpander"):
                if "latchPin" not in controls:
                    errors.append(f"{where}: pinExpander (74HCT595) needs a 'latchPin'")
                # The data-pin count is a property of the BOARD (how many '595 sockets are
                # populated), not of the bus: the driver pads the bus width itself. The ceiling is the
                # runtime's, not an arbitrary one — every pin fans out to 8 strands through its register,
                # and ParallelLedDriver refuses more than kMaxStrands (64), so 8 pins is the most that can
                # ever be driven ("too many strands (pins x 8 through the expander)").
                pins = [p.strip() for p in str(controls.get("pins", "")).split(",") if p.strip()]
                if not 1 <= len(pins) <= 8:
                    errors.append(f"{where}: pinExpander (74HCT595) needs 1..8 data pins "
                                  f"(one per populated register; 8 x 8 taps = the 64-strand ceiling), "
                                  f"got {len(pins)}")
                # The latch rides a DATA LANE (the peripheral gives only one clock), so it must not share a
                # GPIO with anything the bus drives — the bus controls OR a data pin. A data pin carrying
                # the latch waveform emits garbage on that strand.
                latch = controls.get("latchPin")
                if latch is not None:
                    # Normalize to int before every collision test: a control value may be a JSON number
                    # (20) or a string ("20"), and a raw == would let 20 and "20" slip past as "different"
                    # GPIOs when they are the same pad. `pins` are already strings from the split above.
                    def _gpio(v):
                        try:
                            return int(str(v).strip())
                        except (TypeError, ValueError):
                            return None
                    latch_n = _gpio(latch)
                    if latch_n is None:
                        errors.append(f"{where}: latchPin ({latch!r}) is not a valid GPIO number")
                    else:
                        for other in ("clockPin", "dcPin"):
                            if _gpio(controls.get(other)) == latch_n:
                                errors.append(f"{where}: latchPin ({latch_n}) collides with {other} — "
                                              f"the latch needs its own GPIO")
                        if latch_n in [_gpio(pn) for pn in pins]:
                            errors.append(f"{where}: latchPin ({latch_n}) is also a data pin — "
                                          f"the latch needs its own GPIO")

            # Ethernet is explicit, not defaulted: a board that turns Ethernet ON (NetworkModule with
            # a non-None ethType) must declare its board-wiring GPIOs, so the firmware never falls
            # back to a per-chip default that is really one specific board's pins. (The Dig-Octa is
            # why: GPIO5 is the classic-ESP32 default reset but that board uses it as an LED output;
            # inheriting the default would drive an LED pin as an Ethernet reset.) Only the pins that
            # are genuine BOARD WIRING are required — MDC/MDIO may stay at the IDF default (omit or
            # -1) on RMII, since that's a real standard, not a board-specific value.
            if mtype == "NetworkModule" and isinstance(controls, dict):
                # PRESENCE, not truthiness: a JSON `null` reads as None like a missing key, and the
                # installer treats a present-but-null value as a named preset (null !== "Custom").
                board = controls["ethBoard"] if "ethBoard" in controls else None
                if "ethBoard" in controls:
                    if not isinstance(board, str):
                        errors.append(f"{where}: NetworkModule ethBoard must be the preset LABEL as a string, got {board!r}")
                    elif board not in eth_presets:
                        errors.append(f"{where}: NetworkModule ethBoard {board!r} is not a preset in NetworkModule.h (known: {sorted(eth_presets)})")
                et = controls.get("ethType")
                # ethType must be an int (a JSON string like "2" would silently skip the rule below and
                # also isn't what the device deserializes into the Select) — reject a stringified value.
                # `bool` is an int subclass, so exact-type-check (as flashBaud does) or a JSON `true`
                # would pass as ethType 1 (LAN8720).
                if et is not None and type(et) is not int:
                    errors.append(f"{where}: NetworkModule ethType must be an integer, got {et!r}")
                # A named preset IS the explicit pinning: the map lives in NetworkModule.h, under
                # the same review as any other firmware constant, rather than repeated per board.
                if type(et) is int and et != 0 and board in (None, "Custom"):
                    # RMII LAN8720(1)/IP101(2): rst + clock. RGMII YT8531(4): mdc/mdio/rst.
                    # W5500 SPI(3): the four SPI bus pins. -1 is an allowed explicit value ("unused /
                    # IDF default"); what the rule forbids is OMITTING a board-wiring pin.
                    required = {
                        1: ["ethRstGpio", "ethClockGpio"],
                        2: ["ethRstGpio", "ethClockGpio"],
                        4: ["ethMdcGpio", "ethMdioGpio", "ethRstGpio"],
                        3: ["ethSpiMiso", "ethSpiMosi", "ethSpiSck", "ethSpiCs"],
                    }.get(et, [])
                    missing = [p for p in required if p not in controls]
                    if missing:
                        errors.append(f"{where}: NetworkModule sets ethType={et} but omits board-wiring "
                                      f"pin(s) {missing} — Ethernet must be pinned explicitly, not "
                                      f"inherited from a per-chip default (use -1 for a genuinely "
                                      f"unused pin)")

        # Every entry must carry the deviceModel identity (a System unit with the
        # `deviceModel` control). An empty `modules` list is also a failure — it has no
        # identity at all — so this is gated only on board_control_seen, not on `mods`.
        if not board_control_seen:
            errors.append(f"{where}: no System module sets the 'deviceModel' identity control")

    # Report (mirrors check_specs.py's shape).
    print(f"Board check: {len(catalog)} boards, {len(errors)} issue(s)")
    if errors:
        print()
        for err in errors:
            print(f"  {err}")
        print()
        sys.exit(1)
    print("Catalog valid.")


if __name__ == "__main__":
    main()
