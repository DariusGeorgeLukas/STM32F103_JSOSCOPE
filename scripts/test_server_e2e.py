#!/usr/bin/env python3
"""Runs the real jsoscope-server against the simulated device.

Everything the browser depends on passes through here: discovery of the input
controls from the firmware's descriptor, the stream description, binary sample
frames, reconfiguration, and rejected settings.

    python3 scripts/test_server_e2e.py --server PATH --host-test PATH
"""

import argparse
import asyncio
import json
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import websockets

ROOT = Path(__file__).resolve().parent
HTTP_PORT = 13001
WS_PORT = 13002


def wait_for_http(url, seconds=10):
    deadline = time.monotonic() + seconds
    while True:
        try:
            with urllib.request.urlopen(url, timeout=1) as response:
                return response.read()
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.1)


async def next_text(socket, seconds=5):
    """Skips sample frames until the next JSON message."""
    deadline = time.monotonic() + seconds
    while True:
        message = await asyncio.wait_for(socket.recv(), max(0.1, deadline - time.monotonic()))
        if isinstance(message, str):
            return json.loads(message)


async def next_binary(socket, seconds=5):
    deadline = time.monotonic() + seconds
    while True:
        message = await asyncio.wait_for(socket.recv(), max(0.1, deadline - time.monotonic()))
        if isinstance(message, bytes):
            return message


async def configure(socket, settings):
    await socket.send(json.dumps({"type": "configure_device", "settings": settings}))
    messages = []
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        message = await next_text(socket)
        messages.append(message)
        if message.get("type") == "device_config_status":
            status = message
            config = next((m for m in messages if "channels" in m), None)
            if not status["ok"] or config is not None:
                return status, config
        elif "channels" in message and any(m.get("type") == "device_config_status" for m in messages):
            return next(m for m in messages if m.get("type") == "device_config_status"), message
    raise TimeoutError("no configuration status")


async def exercise(url):
    async with websockets.connect(url, max_size=None) as socket:
        config = await next_text(socket)

        # The server and UI learn all of this from the firmware.
        assert config["source_name"] == "STM32F103 Blue Pill"
        controls = config["device_controls"]
        assert controls["title"] == "Blue Pill inputs"
        fields = {field["key"]: field for field in controls["fields"]}
        assert fields["sample_rate"]["suffix"] == "Hz"
        assert fields["analog_pa7"]["type"] == "checkbox"
        assert fields["analog_pb0"]["type"] == "checkbox"
        assert fields["analog_pb1"]["type"] == "checkbox"
        assert [o["value"] for o in fields["digital_pb8"]["options"]][:2] == ["off", "floating"]
        assert [o["value"] for o in fields["digital_pb15"]["options"]][:2] == ["off", "floating"]
        assert fields["adc_sample_time"]["advanced"] is True
        settings = dict(controls["settings"])
        assert settings["sample_rate"] == 1000

        assert [c["name"] for c in config["channels"]] == ["PA0", "PB12"]
        block = await next_binary(socket)
        assert len(block) % config["frame"]["row_stride_bytes"] == 0
        assert len(block) == 6 * 64

        # Every input at once.
        wanted = dict(settings)
        wanted.update(
            sample_rate=2000,
            analog_pa1=True, analog_pa2=True, analog_pa3=True, analog_pa4=True,
            analog_pa5=True, analog_pa6=True, analog_pa7=True,
            analog_pb0=True, analog_pb1=True,
            digital_pb8="floating", digital_pb9="pull-down",
            digital_pb10="floating", digital_pb11="pull-up",
            digital_pb13="pull-down", digital_pb14="floating", digital_pb15="pull-up",
            reference_mv=3250,
        )
        status, config = await configure(socket, wanted)
        assert status["ok"], status
        assert [c["name"] for c in config["channels"]] == [
            "PA0", "PA1", "PA2", "PA3", "PA4", "PA5", "PA6", "PA7", "PB0", "PB1",
            "PB8", "PB9", "PB10", "PB11", "PB12", "PB13", "PB14", "PB15",
        ]
        assert config["sample_rate_hz"] == 2000
        assert config["channels"][0]["max"] == 3.25
        assert config["device_controls"]["settings"]["digital_pb13"] == "pull-down"
        stride = config["frame"]["row_stride_bytes"]
        assert stride == 24

        # No block from the old layout may follow the new description.
        for _ in range(10):
            block = await next_binary(socket)
            assert len(block) == stride * config["frame"]["samples_per_frame"]
        first_row = struct.unpack_from("<10hI", block)
        assert all(value != 0 for value in first_row[2:10])

        # A rejected configuration reports the firmware's reason and keeps
        # the running stream.
        impossible = dict(wanted, sample_rate=50000, adc_sample_time="1.5")
        status, _ = await configure(socket, impossible)
        assert not status["ok"] and "USB" in status["message"], status
        block = await next_binary(socket)
        assert len(block) % stride == 0

        # Digital only, which leaves no analog column at all.
        analog_keys = [f"analog_pa{pin}" for pin in range(8)] + ["analog_pb0", "analog_pb1"]
        digital = dict(wanted, **{key: False for key in analog_keys})
        status, config = await configure(socket, digital)
        assert status["ok"], status
        assert all(c["kind"] == "digital" for c in config["channels"])
        assert (await next_binary(socket))[:0] == b""
        block = await next_binary(socket)
        assert len(block) == 4 * config["frame"]["samples_per_frame"]

        # Back to the defaults, as a browser would after a change of mind.
        status, config = await configure(socket, settings)
        assert status["ok"], status
        assert [c["name"] for c in config["channels"]] == ["PA0", "PB12"]

    # A new client starts from what is running.
    async with websockets.connect(url, max_size=None) as socket:
        config = await next_text(socket)
        assert config["device_controls"]["settings"] == settings


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--server", required=True)
    parser.add_argument("--host-test", required=True)
    parser.add_argument("--leave-running", action="store_true", help="keep serving for a browser")
    options = parser.parse_args()

    simulator = subprocess.Popen(
        [sys.executable, str(ROOT / "simulate_device.py"), "--host-test", options.host_test],
        stdout=subprocess.PIPE, text=True,
    )
    server = None
    try:
        path = simulator.stdout.readline().strip()
        server = subprocess.Popen(
            [options.server, "--headless", "--device", path,
             "--http-port", str(HTTP_PORT), "--ws-port", str(WS_PORT)],
            stdout=subprocess.DEVNULL,
        )
        source = json.loads(wait_for_http(f"http://127.0.0.1:{HTTP_PORT}/ws_config.json"))
        ws_config = source["ws_config"]
        assert ws_config["port"] == WS_PORT
        assert ws_config["path"].startswith("/device/stm32f103-bluepill-"), ws_config
        page = wait_for_http(f"http://127.0.0.1:{HTTP_PORT}/")
        assert b"<html" in page.lower()

        asyncio.run(exercise(f"ws://127.0.0.1:{WS_PORT}{ws_config['path']}"))
        print("server end-to-end test passed")
        if options.leave_running:
            print(f"http://127.0.0.1:{HTTP_PORT}/", flush=True)
            server.wait()
    finally:
        for process in (server, simulator):
            if process is not None:
                process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
