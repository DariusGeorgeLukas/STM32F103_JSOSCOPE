#!/usr/bin/env python3
"""A simulated Blue Pill on a pseudo-terminal, for testing without hardware.

It speaks the real serial protocol, and it asks a host build of the firmware's
parser whether to accept each configuration and what stream to report. Samples
are synthetic. It proves the tools, server, and UI work with this firmware's
protocol. It says nothing about the ADC, DMA, timing, or USB throughput.

    python3 scripts/simulate_device.py --host-test /path/to/host_test
"""

import argparse
import json
import math
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_stream as bench  # noqa: E402

SIGNAL_HZ = 5
MAX_PAYLOAD = 2014
# Known DC inputs, for checking calibration. The first two are moving signals
# that average to mid-scale.
DC_VOLTS = {
    "PA2": 1.0,
    "PA3": 0.5,
    "PA4": 0.25,
    "PA5": 0.75,
    "PA6": 1.25,
    "PA7": 2.0,
    "PB0": 1.5,
    "PB1": 2.5,
}


class Simulator:
    def __init__(self, descriptor, host_test, fault=None):
        self.descriptor = descriptor
        self.descriptor_bytes = json.dumps(descriptor).encode()
        self.host_test = host_test
        self.fault = fault
        self.generation = 0
        self.sequence = 0
        status, output = self.configure_with_firmware(json.dumps(descriptor["configuration"]["defaults"]))
        assert status == 0, output
        self.apply(output)

    def configure_with_firmware(self, settings):
        result = subprocess.run(
            [self.host_test, "--configure"], input=settings.encode(), capture_output=True, check=False
        )
        return result.returncode, result.stdout.decode().strip()

    def apply(self, runtime_json):
        self.runtime = json.loads(runtime_json)
        self.build_waveform()
        self.position = 0
        self.next_block = time.monotonic() + 0.01

    def analog_value(self, channel, phase):
        name = channel["name"]
        scale = channel["max"]
        if name == "PA0":
            value = scale * (0.5 + 0.45 * math.sin(2 * math.pi * phase))
        elif name == "PA1":
            value = scale * (0.05 + 0.9 * abs(2 * phase - 1))
        else:
            value = DC_VOLTS[name]
        per_count, offset = bench.calibration(channel)
        return max(-32768, min(32767, round((value - offset) / per_count)))

    def digital_word(self, phase):
        word = 0
        square = phase < 0.5
        levels = {
            8: square,
            9: not square,
            10: True,
            11: False,
            12: square,
            13: not square,
            14: True,
            15: False,
        }
        for channel in self.runtime["stream"]["channels"]:
            if channel["kind"] == "digital" and levels[channel["bit"]]:
                word |= 1 << channel["bit"]
        return word

    def build_waveform(self):
        """Precomputes one signal period, so that high rates cost no per-row work."""
        stream = self.runtime["stream"]
        rate = stream["sample_rate_hz"]
        stride = stream["frame"]["row_stride_bytes"]
        block_rows = stream["frame"]["samples_per_frame"]
        rows = max(1, rate // SIGNAL_HZ)
        analog = [c for c in stream["channels"] if c["kind"] == "analog"]
        has_digital = any(c["kind"] == "digital" for c in stream["channels"])
        data = bytearray()
        for row in range(rows):
            phase = row / rows
            values = [self.analog_value(channel, phase) for channel in analog]
            if self.fault == "swapped":
                positions = {channel["name"]: index for index, channel in enumerate(analog)}
                if "PA2" in positions and "PA3" in positions:
                    pa2 = positions["PA2"]
                    pa3 = positions["PA3"]
                    values[pa2], values[pa3] = values[pa3], values[pa2]
            data += struct.pack(f"<{len(values)}h", *values)
            if has_digital:
                data += struct.pack("<I", self.digital_word(phase))
        self.waveform = bytes(data)
        self.waveform_rows = rows
        self.stride = stride
        self.block_rows = block_rows
        self.block_seconds = block_rows / rate

    def reply(self, kind, sequence, body):
        if kind == bench.HELLO_REQUEST:
            hello = struct.pack("<HII", MAX_PAYLOAD, len(self.descriptor_bytes), self.generation)
            return bench.encode_message(bench.HELLO_RESPONSE, sequence, hello)
        if kind == bench.DESCRIPTOR_REQUEST:
            offset, length = struct.unpack("<IH", body)
            chunk = self.descriptor_bytes[offset : offset + length]
            header = struct.pack("<II", len(self.descriptor_bytes), offset)
            return bench.encode_message(bench.DESCRIPTOR_CHUNK, sequence, header + chunk)
        if kind == bench.STREAM_CONFIG:
            data = struct.pack("<I", self.generation) + json.dumps(self.runtime).encode()
            return bench.encode_message(bench.STREAM_CONFIG, sequence, data)
        if kind == bench.CONFIGURE:
            status, output = self.configure_with_firmware(body.decode())
            if status != 0:
                data = bytes([1]) + struct.pack("<I", self.generation) + output.encode()
                return bench.encode_message(bench.CONFIGURE_RESULT, sequence, data)
            self.generation += 1
            self.apply(output)
            data = bytes([0]) + struct.pack("<I", self.generation) + output.encode()
            return bench.encode_message(bench.CONFIGURE_RESULT, sequence, data)
        return b""

    def sample_block(self, now):
        """Returns one encoded block when one is due."""
        if now < self.next_block:
            return b""
        seconds = self.block_seconds / 0.8 if self.fault == "slow-rate" else self.block_seconds
        self.next_block += seconds
        rows = self.block_rows
        start = self.position
        end = start + rows
        if end <= self.waveform_rows:
            body = self.waveform[start * self.stride : end * self.stride]
        else:
            body = (
                self.waveform[start * self.stride :]
                + self.waveform * (rows // self.waveform_rows + 1)
            )[: rows * self.stride]
        self.position = end % self.waveform_rows
        self.sequence += 1
        if self.fault == "lost-blocks" and self.sequence % 50 == 0:
            self.sequence += 1
        return bench.encode_message(bench.SAMPLE_DATA, self.sequence & 0xFFFFFFFF, body)


def open_terminal():
    primary, secondary = pty.openpty()
    tty.setraw(primary)
    tty.setraw(secondary)
    attributes = termios.tcgetattr(secondary)
    attributes[3] &= ~termios.ECHO
    termios.tcsetattr(secondary, termios.TCSANOW, attributes)
    return primary, os.ttyname(secondary), secondary


def serve(primary, simulator, stop=lambda: False):
    buffer = bytearray()
    pending = bytearray()
    while not stop():
        readable, writable, _ = select.select([primary], [primary] if pending else [], [], 0.001)
        if primary in readable:
            try:
                buffer += os.read(primary, 4096)
            except OSError:
                return
            while True:
                end = buffer.find(0)
                if end < 0:
                    break
                frame = bytes(buffer[:end])
                del buffer[: end + 1]
                message = bench.decode_message(frame) if frame else None
                if message is not None:
                    # Everything queued before a reply is sent ahead of it, as on the wire.
                    pending += simulator.reply(*message)
        if primary in writable:
            try:
                written = os.write(primary, pending[:4096])
            except OSError:
                return
            del pending[:written]
        if len(pending) < 65536:
            pending += simulator.sample_block(time.monotonic())


def load_descriptor(host_test):
    output = subprocess.run([host_test, "--dump"], capture_output=True, check=True).stdout
    return json.loads(output.decode().splitlines()[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host-test", required=True, help="host_test built from firmware/tests")
    parser.add_argument("--fault", choices=["swapped", "slow-rate", "lost-blocks"])
    options = parser.parse_args()
    simulator = Simulator(load_descriptor(options.host_test), options.host_test, options.fault)
    primary, path, _secondary = open_terminal()
    print(path, flush=True)
    serve(primary, simulator)


if __name__ == "__main__":
    main()
