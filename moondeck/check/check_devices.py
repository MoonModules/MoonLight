#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["jsonschema>=4"]
# ///
"""Validate the installer device catalog (mooninstaller/deviceModels.json).

The catalog is hand-maintained data three clients read alike, the web installer, the device UI's ?deviceModel= inject and MoonDeck, so a typo drifts silently: a broken image path, a name that no longer matches its System.deviceModel, a driver pin list on an entry with no driver. This is the catalog's counterpart to check_specs.py: a fast gate pinning what the clients assume.

The catalog's shape, its fields, their types and their vocabularies, is mooninstaller/deviceModels.schema.json, validated first. What a schema cannot know is checked here, per entry:
  - the name is unique
  - chip is a family of the firmware registry, and every firmware is a registry firmware for that family
  - image (if set) resolves on disk
  - every state root is a top-level module the device boots with
  - the state's System.deviceModel value equals the entry `name`
  - every child module `type` in the state is factory-registered
  - a driver's `pins` control only appears on a light driver (a *LedDriver or PwmLightDriver)

Exit 1 on any error, mirroring check_specs.py.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from firmwares import FIRMWARES, TARGET_TO_FAMILY, chip_family  # noqa: E402
CATALOG = ROOT / "mooninstaller" / "deviceModels.json"
SCHEMA = ROOT / "mooninstaller" / "deviceModels.schema.json"
MODULE_TYPES_CPP = ROOT / "src" / "module_types.cpp"
MAIN_CPP = ROOT / "src" / "main.cpp"
ETHERNET_MODULE = ROOT / "src" / "core" / "system" / "EthernetModule.h"
SD_CARD_MODULE = ROOT / "src" / "core" / "services" / "SdCardService.h"
PLATFORM_CONFIG = ROOT / "src" / "platform" / "esp32" / "platform_config.h"
DOCS = ROOT / "docs"


def preset_labels(header, table):
    """The preset labels of a module header's board-preset table, such as EthernetModule.h's kEthPresets.

    Read from the firmware rather than restated here, so a preset renamed in one place and not the
    other fails this check instead of silently leaving a catalog entry pointing at nothing.
    """
    text = header.read_text(encoding="utf-8")
    block = re.search(table + r"\[\]\s*=\s*\{(.*?)\n    \};", text, re.S)
    if not block:
        return set()
    return set(re.findall(r'^\s*\{"([^"]+)"', block.group(1), re.M))


def eth_preset_drift():
    """Preset rows that no longer match the per-chip default they restate, as (label, field, was, now).

    Three presets ARE a chip's `ethConfigDefault`, so the same pin map has two homes in two layers.
    They cannot simply reference each other: the header's value is a compile-time `constexpr` chosen
    per chip, while the table must carry every chip's row on every build. So the duplication stands
    and this check binds it, because the drift is silent where it matters most: `seedEthPresetFromPins`
    matches on exact equality, so one corrected pin would reseed every provisioned board to Custom.
    """
    table = ETHERNET_MODULE.read_text(encoding="utf-8")
    header = PLATFORM_CONFIG.read_text(encoding="utf-8")
    block = re.search(r"kEthPresets\[\]\s*=\s*\{(.*?)\n    \};", table, re.S)
    if not block:
        return [("kEthPresets", "table", "present", "unreadable")]

    rows = {}
    for m in re.finditer(r'\{"([^"]+)",\s*([^}]+)\}', block.group(1)):
        rows[m.group(1)] = [f.strip() for f in m.group(2).split(",")]

    # label -> the PHY constant naming that chip's arm of the ethConfigDefault ternary.
    PAIRS = {"Classic RMII": "ethLan8720", "P4 RMII": "ethIp101", "S31 RGMII": "ethYt8531"}
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


def top_level_names():
    """The names of the top-level modules the device boots with, which are the only roots a state document reaches.

    main.cpp creates each by type through `top("Type")`, and `kBootModules` names the ones whose name is not the type's default, which drops a trailing `Module`.
    """
    named = dict(re.findall(r'\{nullptr, "([^"]+)", "([^"]+)", [^}]*\}', MODULE_TYPES_CPP.read_text(encoding="utf-8")))
    types = re.findall(r'\btop\("([^"]+)"\)', MAIN_CPP.read_text(encoding="utf-8"))
    return {named.get(t, t[:-len("Module")] if t.endswith("Module") else t) for t in types}


def main():
    errors = []
    eth_presets = preset_labels(ETHERNET_MODULE, "kEthPresets")
    sd_presets = preset_labels(SD_CARD_MODULE, "kSdPresets")
    for label, field, was, now in eth_preset_drift():
        errors.append(f"EthernetModule.h kEthPresets {label!r}: {field} is {now}, but "
                      f"platform_config.h's ethConfigDefault says {was}: the preset restates the "
                      f"chip default, so the two must agree or a provisioned board reseeds to Custom")
    if not eth_presets:
        errors.append("EthernetModule.h: could not read the kEthPresets table, so the ethBoard check cannot run")
    if not sd_presets:
        errors.append("SdCardService.h: could not read the kSdPresets table, so the SD board check cannot run")

    try:
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        print(f"Device check: cannot read {CATALOG.relative_to(ROOT)}: {e}")
        sys.exit(1)

    # Imported here rather than at the top, so check_code, which imports this module's readers, needs no schema package.
    import jsonschema
    # An integer is an int, as esptool reads one: JSON Schema counts 921600.0 as an integer, and it would reach the flasher as a float.
    strict = jsonschema.Draft202012Validator.TYPE_CHECKER.redefine("integer", lambda _, v: isinstance(v, int) and not isinstance(v, bool))
    validator = jsonschema.validators.extend(jsonschema.Draft202012Validator, type_checker=strict)(json.loads(SCHEMA.read_text(encoding="utf-8")))
    for err in validator.iter_errors(catalog):
        path = list(err.absolute_path)
        entry = catalog[path[0]] if path and isinstance(catalog, list) else None
        who = f'"{entry["name"]}"' if isinstance(entry, dict) and isinstance(entry.get("name"), str) else f"entry {path[0]}" if path else "the catalog"
        errors.append(f"{who}: {'/'.join(map(str, path[1:])) or 'entry'}: {err.message}")
    if not isinstance(catalog, list):
        print("\n".join(errors))
        sys.exit(1)

    factory_types = registered_types()
    top_names = top_level_names()
    if not top_names:
        errors.append("main.cpp: could not read the top-level modules, so the state roots cannot be checked")
    names_seen = set()

    for i, e in enumerate(catalog):
        where = f"entry {i}"
        if isinstance(e, dict) and "name" in e:
            where = f'"{e["name"]}"'

        if not isinstance(e, dict):
            continue   # the schema reported it

        name = e.get("name")
        # The schema reported a name that is no string; the checks below key on one, so it stays out of them.
        if isinstance(name, str):
            if name in names_seen:
                errors.append(f"{where}: duplicate device name")
            names_seen.add(name)

        # --- chip: the family the installer detects and the manifests name, and the one every firmware installs onto ---
        chip = e.get("chip")
        fws = e.get("firmwares")
        families = set(TARGET_TO_FAMILY.values())
        # A string first: a list or an object is unhashable, and the set membership test would raise rather than report.
        if "chip" in e and (not isinstance(chip, str) or chip not in families):
            errors.append(f"{where}: chip {chip!r} is not a known family {sorted(families)}")
        elif isinstance(fws, list):
            for fw in fws:
                if isinstance(fw, str) and fw not in FIRMWARES:
                    errors.append(f"{where}: firmware {fw!r} is not in the firmware registry (moondeck/build/firmwares.py)")
                elif isinstance(fw, str) and chip_family(fw) != chip:
                    errors.append(f"{where}: firmware {fw!r} installs onto {chip_family(fw)}, not {chip}")

        img = e.get("image")
        if isinstance(img, str) and img.startswith("assets/deviceModels/") and not (DOCS / img).exists():
            errors.append(f"{where}: image '{img}' does not exist on disk")

        # --- state: the document PATCH /api/state takes; its shape is the schema's, its modules are checked against the firmware here ---
        state = e.get("state")
        if not isinstance(state, dict):
            continue   # the schema reported it
        mods = []                        # [{type, id, controls}] in document order, roots first
        def _walk(path, node, mtype):
            controls = {}
            kids = []
            for k, v in node.items():
                if k in ("type", "$patch"):
                    continue
                if isinstance(v, dict) and isinstance(v.get("type"), str) and v["type"]:
                    kids.append((k, v))
                elif not isinstance(v, dict):
                    controls[k] = v
            mods.append({"type": mtype, "id": path, "controls": controls})
            for k, v in kids:
                _walk(f"{path}.{k}", v, v["type"])
        for root, node in state.items():
            if not isinstance(node, dict):
                continue   # the schema reported it
            if top_names and root not in top_names:
                errors.append(f"{where}: state root '{root}' is not a top-level module (known: {sorted(top_names)})")
                continue
            _walk(root, node, None)
        board_control_seen = False
        for m in mods:
            mtype = m["type"]
            mid = m["id"]
            if mtype is not None and mtype not in factory_types:
                errors.append(f"{where}: state member '{mid}' has type '{mtype}', which is not factory-registered")

            controls = m["controls"]
            # System.deviceModel control must equal the entry name (the identity key).
            if mid == "System" and "deviceModel" in controls:
                board_control_seen = True
                if controls["deviceModel"] != name:
                    errors.append(f"{where}: System.deviceModel control '{controls['deviceModel']}' "
                                  f"!= entry name '{name}'")
            # A `pins` control only makes sense on a light driver: an LED driver, or PWM Light, one pin per channel.
            if "pins" in controls and not (str(mtype).endswith("LedDriver") or mtype == "PwmLightDriver"):
                errors.append(f"{where}: state member '{mid}' has a 'pins' control but is not a light driver")

            # 74HCT595 pin expander (pinExpander = is the board fitted?). The wiring
            # invariants are what a bad catalog entry gets wrong, and they fail on a bench with dark
            # LEDs rather than loudly, so pin them here.
            if controls.get("pinExpander"):
                if "latchPin" not in controls:
                    errors.append(f"{where}: pinExpander (74HCT595) needs a 'latchPin'")
                # The data-pin count is a property of the BOARD (how many '595 sockets are
                # populated), not of the bus: the driver pads the bus width itself. The ceiling is the
                # runtime's, not an arbitrary one: every pin fans out to 8 strands through its register,
                # and ParallelLedDriver refuses more than kMaxStrands (64), so 8 pins is the most that can
                # ever be driven ("too many strands (pins x 8 through the expander)").
                pins = [p.strip() for p in str(controls.get("pins", "")).split(",") if p.strip()]
                if not 1 <= len(pins) <= 8:
                    errors.append(f"{where}: pinExpander (74HCT595) needs 1..8 data pins "
                                  f"(one per populated register; 8 x 8 taps = the 64-strand ceiling), "
                                  f"got {len(pins)}")
                # The latch rides a DATA LANE (the peripheral gives only one clock), so it must not share a
                # GPIO with anything the bus drives: the bus controls OR a data pin. A data pin carrying
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
                                errors.append(f"{where}: latchPin ({latch_n}) collides with {other}: "
                                              f"the latch needs its own GPIO")
                        if latch_n in [_gpio(pn) for pn in pins]:
                            errors.append(f"{where}: latchPin ({latch_n}) is also a data pin: "
                                          f"the latch needs its own GPIO")

            # Ethernet is explicit, not defaulted: a board that turns Ethernet ON (EthernetModule with
            # a non-None ethType) must declare its board-wiring GPIOs, so the firmware never falls
            # back to a per-chip default that is really one specific board's pins. (The Dig-Octa is
            # why: GPIO5 is the classic-ESP32 default reset but that board uses it as an LED output;
            # inheriting the default would drive an LED pin as an Ethernet reset.) Only the pins that
            # are genuine BOARD WIRING are required: MDC/MDIO may stay at the IDF default (omit or
            # -1) on RMII, since that's a real standard, not a board-specific value.
            # The same rule for the SD slot's wiring, whose map lives in SdCardService.h.
            if mtype == "SdCardService" and "board" in controls:
                board = controls["board"]
                if not isinstance(board, str):
                    errors.append(f"{where}: SdCardService board must be the preset LABEL as a string, got {board!r}")
                elif board not in sd_presets:
                    errors.append(f"{where}: SdCardService board {board!r} is not a preset in SdCardService.h (known: {sorted(sd_presets)})")
            if mtype == "EthernetModule":
                # PRESENCE, not truthiness: a JSON `null` reads as None like a missing key, and the
                # installer treats a present-but-null value as a named preset (null !== "Custom").
                board = controls["ethBoard"] if "ethBoard" in controls else None
                if "ethBoard" in controls:
                    if not isinstance(board, str):
                        errors.append(f"{where}: EthernetModule ethBoard must be the preset LABEL as a string, got {board!r}")
                    elif board not in eth_presets:
                        errors.append(f"{where}: EthernetModule ethBoard {board!r} is not a preset in EthernetModule.h (known: {sorted(eth_presets)})")
                et = controls.get("ethType")
                # ethType must be an int (a JSON string like "2" would silently skip the rule below and
                # also isn't what the device deserializes into the Select), so reject a stringified value.
                # `bool` is an int subclass, so exact-type-check (as flashBaud does) or a JSON `true`
                # would pass as ethType 1 (LAN8720).
                if et is not None and type(et) is not int:
                    errors.append(f"{where}: EthernetModule ethType must be an integer, got {et!r}")
                # A named preset IS the explicit pinning: the map lives in EthernetModule.h, under
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
                        errors.append(f"{where}: EthernetModule sets ethType={et} but omits board-wiring "
                                      f"pin(s) {missing}: Ethernet must be pinned explicitly, not "
                                      f"inherited from a per-chip default (use -1 for a genuinely "
                                      f"unused pin)")

        # Every entry must carry the deviceModel identity (System.deviceModel). An empty `state`
        # is also a failure, since it has no identity at all.
        if not board_control_seen:
            errors.append(f"{where}: state sets no System.deviceModel identity")

    # Report (mirrors check_specs.py's shape).
    print(f"Device check: {len(catalog)} devices, {len(errors)} issue(s)")
    if errors:
        print()
        for err in errors:
            print(f"  {err}")
        print()
        sys.exit(1)
    print("Catalog valid.")


if __name__ == "__main__":
    main()
