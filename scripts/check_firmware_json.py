#!/usr/bin/env python3
"""Checks the JSON printed by the firmware host test's --dump mode.

The first line is the device descriptor. Each later line is a runtime
configuration. The firmware builds these by hand, so this confirms they parse
and that they follow the layout rules in the JSOScope protocol document.
"""

import json
import sys

ANALOG_BYTES = 2
DIGITAL_BYTES = 4
MAX_BLOCK_BYTES = 512


def check_descriptor(descriptor):
    assert descriptor["protocol"] == {"name": "jsoscope-device", "major": 2, "minor": 1}
    assert descriptor["device"]["id"].startswith("stm32f103-bluepill-")
    assert len(descriptor["device"]["id"].removeprefix("stm32f103-bluepill-")) == 12
    assert descriptor["capabilities"] == {"configuration": True, "streaming": True}
    assert descriptor["limits"]["max_bytes_per_second"] > 0

    configuration = descriptor["configuration"]
    defaults = configuration["defaults"]
    keys = set()
    for field in configuration["fields"]:
        key = field["key"]
        assert key not in keys, f"duplicate field {key}"
        keys.add(key)
        assert key in defaults, f"{key} has no default"
        assert field["type"] in ("select", "number", "checkbox"), key
        if field["type"] == "select":
            values = [
                option["value"] if isinstance(option, dict) else option
                for option in field["options"]
            ]
            assert defaults[key] in values, f"default for {key} is not an option"
            # The UI returns numbers as numbers only when the default is one.
            assert all(type(value) is type(defaults[key]) for value in values), key
        if field["type"] == "number":
            assert field["min"] <= defaults[key] <= field["max"], key
    assert set(defaults) == keys, f"defaults without fields: {set(defaults) - keys}"


def check_runtime(runtime, descriptor):
    stream = runtime["stream"]
    settings = runtime["settings"]
    assert set(settings) == set(descriptor["configuration"]["defaults"])

    channels = stream["channels"]
    assert channels, "a stream needs at least one channel"
    names = [channel["name"] for channel in channels]
    assert len(set(names)) == len(names), "channel names must be unique"

    analog = [channel for channel in channels if channel["kind"] == "analog"]
    digital = [channel for channel in channels if channel["kind"] == "digital"]
    assert len(analog) + len(digital) == len(channels)
    for index, channel in enumerate(analog):
        assert channel["offset"] == index * ANALOG_BYTES
        assert channel["type"] == "int16"
        # JSOScope defaults analog channels without an explicit unit to volts.
        assert channel.get("units", "V") == "V"
        assert channel["min"] == 0
        assert channel["max"] == settings["reference_mv"] / 1000
    word_offset = len(analog) * ANALOG_BYTES
    for channel in digital:
        assert channel["offset"] == word_offset, "digital channels share one word"
        assert channel["type"] == "uint32"
        assert 0 <= channel["bit"] < 32
        assert channel["name"] == f"PB{channel['bit']}", "a bit is its pin number"

    stride = stream["frame"]["row_stride_bytes"]
    assert stride == word_offset + (DIGITAL_BYTES if digital else 0)
    assert 1 <= stream["frame"]["samples_per_frame"]
    assert stream["frame"]["samples_per_frame"] * stride <= MAX_BLOCK_BYTES
    assert stream["sample_rate_hz"] == settings["sample_rate"]

    # What is enabled in the settings is exactly what is in the stream.
    enabled = {
        key.split("_")[1].upper()
        for key, value in settings.items()
        if (key.startswith("analog_") and value is True)
        or (key.startswith("digital_") and value != "off")
    }
    assert enabled == set(names), (enabled, names)


def main():
    lines = sys.stdin.read().splitlines()
    assert len(lines) >= 2, "expected a descriptor and runtime configurations"
    descriptor = json.loads(lines[0])
    check_descriptor(descriptor)
    runtimes = [json.loads(line) for line in lines[1:]]
    for runtime in runtimes:
        check_runtime(runtime, descriptor)

    default = runtimes[0]
    described = descriptor["stream"]
    assert default["settings"] == descriptor["configuration"]["defaults"]
    assert default["stream"] == described, "descriptor stream differs from the default runtime"
    assert described["sample_rate_hz"] <= descriptor["limits"]["max_sample_rate_hz"]
    print(f"firmware JSON checks passed ({len(lines[0])}-byte descriptor)")


if __name__ == "__main__":
    main()
