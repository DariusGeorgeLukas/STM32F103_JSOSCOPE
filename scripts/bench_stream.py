#!/usr/bin/env python3
"""Bench test for the Blue Pill firmware. Needs real hardware.

Talks to the firmware directly over USB serial, runs a set of input
combinations, and reports whether each one behaved. See docs/bench-test.md for
the wiring and how to act on the results.

Stop the JSOScope server first, because only one program can open the port.

    python3 scripts/bench_stream.py --port /dev/ttyACM0 \\
        --expect pa0=1.65,pa1=0.0 --expect-digital pb12=1,pb13=0
"""

import argparse
import json
import statistics
import struct
import sys
import time
from dataclasses import dataclass, field

HEADER = struct.Struct("<4sBBBBIH")
MAGIC = b"JSO2"
PROTOCOL_MAJOR = 2
PROTOCOL_MINOR = 1
HELLO_REQUEST, HELLO_RESPONSE = 1, 2
DESCRIPTOR_REQUEST, DESCRIPTOR_CHUNK = 3, 4
CONFIGURE, CONFIGURE_RESULT = 5, 6
STREAM_CONFIG = 7
SAMPLE_DATA = 8

RATE_TOLERANCE = 0.01
VOLTAGE_TOLERANCE = 0.05
FAST_ADC = "1.5"


def make_crc_table():
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ 0x8408 if value & 1 else value >> 1
        table.append(value)
    return table


CRC_TABLE = make_crc_table()


# These run on every byte at up to 600 kB/s, and a bitwise version would make
# this script the slowest part of the test.
def crc16_kermit(data):
    crc = 0
    table = CRC_TABLE
    for byte in data:
        crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF]
    return crc


def cobs_encode(raw):
    output = bytearray()
    for part in raw.split(b"\0"):
        while len(part) >= 254:
            output.append(0xFF)
            output += part[:254]
            part = part[254:]
        output.append(len(part) + 1)
        output += part
    output.append(0)
    return bytes(output)


def cobs_decode(encoded):
    output = bytearray()
    position = 0
    while position < len(encoded):
        code = encoded[position]
        if code == 0:
            return None
        position += 1
        end = position + code - 1
        if end > len(encoded):
            return None
        output += encoded[position:end]
        position = end
        if code != 0xFF and position < len(encoded):
            output.append(0)
    return bytes(output)


def encode_message(kind, sequence, body=b""):
    payload = HEADER.pack(MAGIC, PROTOCOL_MAJOR, PROTOCOL_MINOR, kind, 0, sequence, len(body)) + body
    record = bytes([1]) + struct.pack("<H", len(payload)) + payload
    return cobs_encode(record + struct.pack("<H", crc16_kermit(record)))


def decode_message(frame):
    """Returns (kind, sequence, body), or None for a damaged frame."""
    raw = cobs_decode(frame)
    if raw is None or len(raw) < 5 or raw[0] != 1:
        return None
    length = struct.unpack_from("<H", raw, 1)[0]
    if len(raw) != 5 + length:
        return None
    if crc16_kermit(raw[:-2]) != struct.unpack_from("<H", raw, len(raw) - 2)[0]:
        return None
    if length < HEADER.size:
        return None
    magic, major, _minor, kind, _flags, sequence, body_length = HEADER.unpack_from(raw, 3)
    if magic != MAGIC or major != PROTOCOL_MAJOR or body_length != length - HEADER.size:
        return None
    return kind, sequence, raw[3 + HEADER.size : 3 + length]


class Device:
    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()
        self.sequence = 0x10
        self.bad_frames = 0

    def frames(self):
        """Reads what is available and yields decoded messages."""
        data = self.port.read(max(1, self.port.in_waiting))
        self.buffer += data
        while True:
            end = self.buffer.find(0)
            if end < 0:
                return
            frame = bytes(self.buffer[:end])
            del self.buffer[: end + 1]
            if not frame:
                continue
            message = decode_message(frame)
            if message is None:
                self.bad_frames += 1
            else:
                yield message

    def request(self, kind, reply, body=b"", timeout=2.0):
        self.sequence += 1
        self.port.write(encode_message(kind, self.sequence, body))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for message_kind, sequence, message_body in self.frames():
                if message_kind == reply and sequence == self.sequence:
                    return message_body
        raise TimeoutError(f"no reply to message type {kind}")

    def descriptor(self):
        hello = self.request(HELLO_REQUEST, HELLO_RESPONSE)
        _, length, _ = struct.unpack("<HII", hello)
        data = b""
        while len(data) < length:
            body = struct.pack("<IH", len(data), min(480, length - len(data)))
            data += self.request(DESCRIPTOR_REQUEST, DESCRIPTOR_CHUNK, body)[8:]
        return json.loads(data)

    def runtime(self):
        return json.loads(self.request(STREAM_CONFIG, STREAM_CONFIG)[4:])

    def configure(self, settings):
        """Returns (accepted, runtime or error text)."""
        body = self.request(CONFIGURE, CONFIGURE_RESULT, json.dumps(settings).encode())
        if body[0] != 0:
            return False, body[5:].decode()
        return True, json.loads(body[5:])

    def capture(self, seconds, stream, settle=0.2):
        """Collects sample blocks after discarding the first `settle` seconds."""
        capture = Capture()
        started = time.monotonic()
        while time.monotonic() - started < seconds + settle:
            for kind, sequence, body in self.frames():
                if kind != SAMPLE_DATA or time.monotonic() - started < settle:
                    continue
                capture.add(sequence, body, stream, time.monotonic())
        capture.bad_frames = self.bad_frames
        capture.decode(stream)
        return capture


@dataclass
class Capture:
    rows: int = 0
    blocks: int = 0
    lost_blocks: int = 0
    short_blocks: int = 0
    bad_frames: int = 0
    first_time: float = 0.0
    last_time: float = 0.0
    rows_after_first: int = 0
    last_sequence: int = None
    bodies: list = field(default_factory=list)
    analog: dict = field(default_factory=dict)
    digital: dict = field(default_factory=dict)

    def add(self, sequence, body, stream, now):
        stride = stream["frame"]["row_stride_bytes"]
        if len(body) != stride * stream["frame"]["samples_per_frame"]:
            self.short_blocks += 1
            return
        if self.last_sequence is not None:
            self.lost_blocks += (sequence - self.last_sequence - 1) & 0xFFFFFFFF
        self.last_sequence = sequence
        rows = len(body) // stride
        if self.blocks == 0:
            self.first_time = now
        else:
            self.rows_after_first += rows
        self.last_time = now
        self.blocks += 1
        self.rows += rows
        self.bodies.append(body)

    def decode(self, stream):
        """Unpacks the columns. This waits until after the capture, because at
        the highest rates it takes longer than the capture does."""
        data = b"".join(self.bodies)
        analog = [c for c in stream["channels"] if c["kind"] == "analog"]
        digital = [c for c in stream["channels"] if c["kind"] == "digital"]
        layout = "<" + "h" * len(analog) + ("I" if digital else "")
        rows = list(struct.iter_unpack(layout, data)) if data else []
        for column, channel in enumerate(analog):
            self.analog[channel["name"]] = [row[column] for row in rows]
        for channel in digital:
            bit = channel["bit"]
            self.digital[channel["name"]] = [(row[-1] >> bit) & 1 for row in rows]

    def measured_rate(self):
        elapsed = self.last_time - self.first_time
        return self.rows_after_first / elapsed if elapsed > 0 else 0.0


def calibration(channel):
    """Volts per count and the offset, as the JSOScope UI computes them for int16."""
    per_count = (channel["max"] - channel["min"]) / 65535
    return per_count, (channel["min"] + channel["max"]) / 2 + 0.5 * per_count


def volts(channel, counts):
    per_count, offset = calibration(channel)
    return counts * per_count + offset


def settings_for(defaults, rate=1000, analog=("pa0",), digital=("pb12",), adc=None):
    settings = dict(defaults)
    settings["sample_rate"] = rate
    for key in settings:
        if key.startswith("analog_"):
            settings[key] = key.split("_")[1] in analog
        elif key.startswith("digital_"):
            settings[key] = "floating" if key.split("_")[1] in digital else "off"
    if adc is not None:
        settings["adc_sample_time"] = adc
    return settings


ALL_ANALOG = tuple(f"pa{pin}" for pin in range(8)) + ("pb0", "pb1")
ALL_DIGITAL = tuple(f"pb{pin}" for pin in range(8, 16))

# name, keyword arguments for settings_for. The last few sit at the edge of
# what the firmware accepts and are what tests its USB limit.
CASES = [
    ("default", {}),
    ("analog-only", {"digital": ()}),
    ("digital-only", {"analog": ()}),
    ("all-inputs-1k", {"analog": ALL_ANALOG, "digital": ALL_DIGITAL}),
    ("all-inputs-10k", {"rate": 10000, "analog": ALL_ANALOG, "digital": ALL_DIGITAL}),
    ("slow-100hz", {"rate": 100}),
    ("limit-one-analog", {"rate": 200000, "digital": (), "adc": FAST_ADC}),
    ("limit-digital-only", {"rate": 100000, "analog": ()}),
    ("limit-all-inputs", {"rate": 20000, "analog": ALL_ANALOG, "digital": ALL_DIGITAL, "adc": FAST_ADC}),
]

MUST_REJECT = [
    ("reject-no-inputs", {"analog": (), "digital": ()}, "at least one input"),
    ("reject-usb-budget", {"rate": 50000, "analog": ALL_ANALOG, "digital": ALL_DIGITAL, "adc": FAST_ADC}, "USB"),
    ("reject-adc-budget", {"rate": 200000, "digital": (), "adc": "239.5"}, "ADC sample time"),
]


def check_case(capture, stream, seconds, options):
    failures = []
    if capture.blocks < 2:
        return [f"only {capture.blocks} sample blocks arrived"]
    if capture.short_blocks:
        failures.append(f"{capture.short_blocks} blocks had the wrong size")
    if capture.lost_blocks:
        failures.append(f"{capture.lost_blocks} blocks were lost")
    if capture.bad_frames:
        failures.append(f"{capture.bad_frames} damaged frames")

    reported = stream["sample_rate_hz"]
    block_seconds = stream["frame"]["samples_per_frame"] / reported
    # At low rates the host clock only sees a few blocks.
    tolerance = max(RATE_TOLERANCE, 2 * block_seconds / seconds)
    measured = capture.measured_rate()
    if abs(measured - reported) > reported * tolerance:
        failures.append(f"measured {measured:.1f} rows/s but reported {reported}")

    for channel in stream["channels"]:
        name = channel["name"].lower()
        if channel["kind"] == "analog" and name in options.expect:
            mean = volts(channel, statistics.fmean(capture.analog[channel["name"]]))
            if abs(mean - options.expect[name]) > VOLTAGE_TOLERANCE:
                failures.append(f"{name} read {mean:.3f} V, expected {options.expect[name]:.3f} V")
        if channel["kind"] == "digital" and name in options.expect_digital:
            values = set(capture.digital[channel["name"]])
            if values != {options.expect_digital[name]}:
                failures.append(f"{name} read {sorted(values)}, expected {options.expect_digital[name]}")
    return failures


def summarize(capture, stream):
    line = [f"{capture.measured_rate():.1f} rows/s"]
    for channel in stream["channels"]:
        if channel["kind"] == "analog":
            line.append(f"{channel['name']} {volts(channel, statistics.fmean(capture.analog[channel['name']])):.3f} V")
        else:
            values = capture.digital[channel["name"]]
            line.append(f"{channel['name']} {statistics.fmean(values) * 100:.0f}% high")
    return ", ".join(line)


def run_cases(device, options):
    descriptor = device.descriptor()
    defaults = descriptor["configuration"]["defaults"]
    results = []

    def record(name, failures, detail=""):
        results.append({"name": name, "passed": not failures, "failures": failures, "detail": detail})
        status = "pass" if not failures else "FAIL"
        print(f"{status}  {name}  {detail}")
        for failure in failures:
            print(f"      {failure}")

    def selected(name):
        return not options.case or name in options.case

    for name, arguments in CASES:
        if not selected(name):
            continue
        accepted, runtime = device.configure(settings_for(defaults, **arguments))
        if not accepted:
            record(name, [f"configuration was rejected: {runtime}"])
            continue
        stream = runtime["stream"]
        capture = device.capture(options.seconds, stream)
        failures = check_case(capture, stream, options.seconds, options)
        record(name, failures, summarize(capture, stream) if capture.blocks else "")

    for name, arguments, expected in MUST_REJECT:
        if not selected(name):
            continue
        accepted, runtime = device.configure(settings_for(defaults, **arguments))
        if accepted:
            record(name, ["configuration was accepted"])
        elif expected not in runtime:
            record(name, [f"wrong error: {runtime}"])
        else:
            record(name, [], runtime)

    if options.soak:
        arguments = dict(CASES[-1][1])
        accepted, runtime = device.configure(settings_for(defaults, **arguments))
        capture = device.capture(options.soak, runtime["stream"])
        failures = check_case(capture, runtime["stream"], options.soak, options) if accepted else [runtime]
        record(f"soak-{options.soak}s", failures, f"{capture.rows} rows")

    device.configure(defaults)
    return results


def parse_pairs(text, convert):
    pairs = {}
    for item in filter(None, (text or "").split(",")):
        key, value = item.split("=")
        pairs[key.strip().lower()] = convert(value)
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=3.0, help="capture time per case")
    parser.add_argument("--expect", help="known voltages, such as pa0=1.65,pa1=0")
    parser.add_argument("--expect-digital", help="known levels, such as pb12=1,pb13=0")
    parser.add_argument("--case", action="append", help="run only this case; may be repeated")
    parser.add_argument("--soak", type=int, default=0, help="also run the busiest case for this many seconds")
    parser.add_argument("--report", help="write the results to this JSON file")
    options = parser.parse_args()
    options.expect = parse_pairs(options.expect, float)
    options.expect_digital = parse_pairs(options.expect_digital, int)

    import serial  # Imported here so that the simulator does not need pyserial.

    with serial.Serial(options.port, timeout=0.05) as port:
        port.dtr = True
        results = run_cases(Device(port), options)

    if options.report:
        with open(options.report, "w", encoding="utf-8") as output:
            json.dump(results, output, indent=2)
    failed = [result["name"] for result in results if not result["passed"]]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
