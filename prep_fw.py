#!/usr/bin/env python3
import argparse
from base64 import b64decode
from pathlib import Path


KEY_LENGTH = 28
MAX_KEYS = 32
GOOGLE_EID_LENGTH = 20
KEY_MARKER = b"OFFLINEFINDINGPUBLICKEYHERE!"
GOOGLE_EID_MARKER = b"GOOGLEFINDMYEIDHERE!"
CONFIG_MARKER = b"FINDMY_CONFIG_V1"
NETWORK_APPLE = 1
NETWORK_GOOGLE = 2
NETWORK_DUAL = 3


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Configure main.bin for Apple Find My, Google Find Hub, or both"
        )
    )
    parser.add_argument(
        "--keyfile",
        help="Path(s) to text .keys files or binary _keyfile bundles, in rotation order",
        type=Path,
        nargs="+",
    )
    parser.add_argument(
        "--google-eid",
        help="40-character GoogleFindMyTools advertisement key (20-byte EID)",
    )
    parser.add_argument(
        "--adv-interval",
        help="Advertisement interval (seconds)",
        type=int,
        choices=[2, 3, 5, 10, 20, 30],
        default=3,
    )
    parser.add_argument(
        "--firmware",
        help="Unpatched firmware image",
        type=Path,
        default=Path("main.bin"),
    )
    parser.add_argument("--output", help="Output firmware path", type=Path)
    args = parser.parse_args()

    keyfiles = args.keyfile or []
    if not keyfiles and args.google_eid is None:
        parser.error("provide --keyfile, --google-eid, or both")

    output = args.output
    if output is None:
        if keyfiles:
            suffix = "_rotating" if len(keyfiles) > 1 else ""
            if args.google_eid is not None:
                suffix += "_dual"
            output = Path(f"FindMy_{keyfiles[0].stem}{suffix}.bin")
        else:
            output = Path("FindMy_Google.bin")

    patch_fw(
        args.firmware,
        keyfiles,
        output,
        args.adv_interval,
        args.google_eid,
    )


def read_keys(keyfile):
    key_data = keyfile.read_bytes()

    # generate_keys.py writes a one-byte key count followed by packed 28-byte
    # advertisement keys to files named *_keyfile.
    if key_data:
        key_count = key_data[0]
        if key_count and len(key_data) == 1 + key_count * KEY_LENGTH:
            return [
                key_data[1 + index * KEY_LENGTH:1 + (index + 1) * KEY_LENGTH]
                for index in range(key_count)
            ]

    try:
        text = key_data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(
            f"{keyfile}: unrecognized key format; expected a text .keys file "
            "or binary _keyfile bundle"
        ) from error

    keys = []
    for line in text.splitlines():
        name, separator, encoded_key = line.partition(": ")
        if not separator or name != "Advertisement key":
            continue

        try:
            key = b64decode(encoded_key, validate=True)
        except ValueError as error:
            raise ValueError(f"{keyfile}: invalid base64 advertisement key") from error

        if len(key) != KEY_LENGTH:
            raise ValueError(
                f"{keyfile}: advertisement key is {len(key)} bytes; "
                f"expected {KEY_LENGTH}"
            )
        keys.append(key)

    if not keys:
        raise ValueError(f"{keyfile}: no 'Advertisement key' entry found")

    return keys


def find_unique(firmware, marker, description):
    offset = firmware.find(marker)
    if offset < 0:
        raise ValueError(f"{description} marker not found; rebuild main.bin first")
    if firmware.find(marker, offset + 1) >= 0:
        raise ValueError(f"multiple {description} markers found in firmware")
    return offset


def parse_google_eid(value):
    if value is None:
        return None
    if isinstance(value, bytes):
        eid = value
    else:
        text = value.strip()
        if text.lower().startswith("0x"):
            text = text[2:]
        try:
            eid = bytes.fromhex(text)
        except ValueError as error:
            raise ValueError(
                "Google advertisement key must be hexadecimal"
            ) from error

    if len(eid) != GOOGLE_EID_LENGTH:
        raise ValueError(
            f"Google advertisement key is {len(eid)} bytes; "
            f"expected {GOOGLE_EID_LENGTH}"
        )
    return eid


def apple_advertising_address(key):
    """Return the key-derived address in the order shown by BLE scanners."""
    return bytes([key[0] | 0xC0]) + key[1:6]


def patch_fw(
    firmware_path,
    keyfiles,
    output_path,
    adv_interval=3,
    google_eid=None,
):
    keyfiles = keyfiles or []
    keys = []
    for keyfile in keyfiles:
        keys.extend(read_keys(keyfile))
    if len(keys) > MAX_KEYS:
        raise ValueError(f"at most {MAX_KEYS} keys are supported; got {len(keys)}")
    google_eid = parse_google_eid(google_eid)
    if not keys and google_eid is None:
        raise ValueError("at least one Apple key or a Google EID is required")

    firmware = bytearray(firmware_path.read_bytes())

    if keys:
        key_start = find_unique(firmware, KEY_MARKER, "Apple public-key")
        key_region_end = key_start + MAX_KEYS * KEY_LENGTH
        if key_region_end > len(firmware):
            raise ValueError("public-key slots extend past the end of the firmware")

        firmware[key_start:key_region_end] = bytes(MAX_KEYS * KEY_LENGTH)
        for index, key in enumerate(keys):
            start = key_start + index * KEY_LENGTH
            firmware[start:start + KEY_LENGTH] = key

    if google_eid is not None:
        google_start = find_unique(firmware, GOOGLE_EID_MARKER, "Google EID")
        firmware[google_start:google_start + GOOGLE_EID_LENGTH] = google_eid

    config_start = find_unique(firmware, CONFIG_MARKER, "configuration")
    interval = adv_interval * 1600
    interval_start = config_start + len(CONFIG_MARKER)
    firmware[interval_start:interval_start + 2] = interval.to_bytes(2, "little")

    if keys and google_eid is not None:
        network_mode = NETWORK_DUAL
        network_description = "Apple + Google (30s time-sharing)"
    elif google_eid is not None:
        network_mode = NETWORK_GOOGLE
        network_description = "Google"
    else:
        network_mode = NETWORK_APPLE
        network_description = "Apple"
    firmware[interval_start + 2] = network_mode

    output_path.write_bytes(firmware)
    key_description = f", {len(keys)} Apple key(s) rotating hourly" if keys else ""
    print(
        f"Wrote {output_path} for {network_description}{key_description}, "
        f"with a {adv_interval}s advertising interval"
    )
    if keys:
        print(
            "Expected initial Apple address: "
            + apple_advertising_address(keys[0]).hex(":")
        )
    if google_eid is not None:
        print("Google advertisement EID: " + google_eid.hex())


if __name__ == "__main__":
    main()
