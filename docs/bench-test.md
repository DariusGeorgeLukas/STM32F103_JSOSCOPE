# Bench test

This checks the firmware on real hardware. Everything that can be tested without
hardware runs in `scripts/test-firmware-host.sh`. This test covers what cannot:
the timer, the ADC, the DMA, and real USB throughput.

**Status: not yet run.** No results have been recorded on a real board. The first
run matters most for these:

- `max_bytes_per_second` in `firmware/src/scope_config.hpp`. 600 kB/s is an
  estimate and has never been measured. The `limit-*` cases test it.
- `firmware/src/sampler.cpp`, which no other test exercises.

## What you need

- A Blue Pill with this firmware, and a USB cable.
- For the voltage checks, known DC voltages between 0 and 3.3 V on any enabled
  PA0 to PA7 or PB0 to PB1 inputs.
  The board's own 3V3 and GND pins work, and so does a divider between them. Make
  them different from each other, so that swapped channels are obvious.
- For the digital checks, enabled PB8 to PB15 inputs tied to 3V3 or GND. Use
  different levels.
- A multimeter to measure the voltages and the 3V3 rail.
- Python with `pip install pyserial`.

Leave no selected input floating. A floating analog input reads noise and fails
the voltage check, and a floating digital input can read either level.

## Run it

1. Build and flash the firmware from this revision.
2. Quit the JSOScope server, because only one program can open the serial port.
3. Find the port, which is usually `/dev/ttyACM0` on Linux.
4. Run the test with the values you measured:

```sh
python3 scripts/bench_stream.py --port /dev/ttyACM0 \
    --expect pa0=3.30,pa1=0,pa2=1.65,pa3=0.82 \
    --expect-digital pb12=1,pb13=0,pb14=1,pb15=0 \
    --report bench-results.json
```

A full run takes about 30 seconds. It restores the default configuration when it
finishes, and it exits with status 1 if anything failed.

The voltages are calculated with the default 3300 mV reference and allowed to
differ by 50 mV. If your 3V3 rail is far from 3.3 V, scale your expected values
to match.

To repeat one case, add `--case NAME`, for example `--case limit-one-analog`.
Add `--soak 600` for a ten-minute run of the busiest multi-input case.

## What it checks

| Check | What a failure means |
| --- | --- |
| Every block has the size the stream reports | The firmware's layout and its DMA setup disagree |
| Row rate within 1% of the reported rate | The timer is misconfigured, or the crystal is not 8 MHz |
| No gaps in the sample sequence | Blocks were lost, because USB could not keep up |
| Readings match `--expect` | Channels are swapped, or the scaling is wrong |
| Levels match `--expect-digital` | The pins, their mode, or the bit mapping are wrong |
| No damaged frames | The serial link or the framing has a problem |
| Impossible selections are refused | Validation is broken |

## After the run

**If the `limit-*` cases lose blocks,** USB is not delivering 600 kB/s on your
host. Lower `max_bytes_per_second`, and the matching value in
`firmware/src/device_descriptor.hpp`, until they pass. `scripts/test-firmware-host.sh`
fails if the two disagree. If they pass easily, try raising it.

**If only `limit-all-inputs` fails on a voltage,** the input has probably not
settled in 1.5 ADC cycles, which that case uses to reach its rate. That is a limit
of the source impedance, not a fault.

**If analog readings are good but digital rows look shifted against them,**
check whether a configuration's scan really fits in its row period. Look at
`adc_trigger_margin_clocks` in `firmware/src/scope_config.cpp`.

**If nothing arrives at all,** check that the LED on PC13 is blinking. If it is,
the sampler is running and the fault is on the USB side. If it stays still, the
sampler is not producing blocks, so start with the DMA channels in
`firmware/src/sampler.cpp`.

## Testing the script itself

This needs no hardware. It runs the bench script against a simulated device, and
then against three injected faults that it has to catch:

```sh
./scripts/test-firmware-host.sh
```

A simulated device is also available for trying the server and UI with no board:

```sh
g++ -std=c++20 -Ifirmware/src -Ilib/jsoscope-device/include \
    firmware/tests/host_test.cpp firmware/src/scope_config.cpp \
    lib/jsoscope-device/src/protocol.cpp -o /tmp/host_test
python3 scripts/simulate_device.py --host-test /tmp/host_test
```

It prints a terminal path. Pass that to `jsoscope-server --device`.
