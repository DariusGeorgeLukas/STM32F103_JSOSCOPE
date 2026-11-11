# STM32F103 Blue Pill JSOScope appliance

This project turns an STM32F103 "Blue Pill" into a small analog and digital
input streamer for [JSOScope](https://github.com/DariusGeorgeLukas/JSOScope):

```text
PA0-PA7, PB0-PB1 -> ADC1 + DMA --\
                                  +-> STM32F103 -> USB CDC -> Rust desktop companion -> HTTP -> JSOScope UI
PB8-PB15 -> GPIO + DMA ------------/                                        `-> WebSocket -> live samples
```

The firmware describes itself. The Rust server and the browser have no
knowledge of this board: they read its inputs, sample rates, and limits from a
descriptor stored in the firmware, then build the configuration panel from it.
Choosing settings in the browser sends them back to the firmware, which
validates them and replies with the stream layout that is now running.

> **Status:** the firmware builds, and everything above the hardware is tested
> (see [Testing](#testing)). It has **not yet been run on a real Blue Pill**. The
> ADC and DMA setup and the USB byte-rate limit come from the reference manual
> and from estimates. Run the [bench test](docs/bench-test.md) on the first board.

The shared pieces are git submodules, following the current JSOScope device
architecture:

- `ui/JSOScope` contains the browser UI and Rust desktop companion. Release
  builds embed the UI, so end users do not need Node.js, Python, or a separate
  web server.
- `lib/jsoscope-device` contains protocol v2.1 framing and the generic device
  session. This firmware supplies only the STM32 instrument and USB adapters.

## Hardware

The firmware targets an STM32F103C8 Blue Pill with an 8 MHz crystal. It runs at
72 MHz and needs about 34 KB of flash and 18 KB of RAM, including the stack, so the
smaller C6 variant does not have enough RAM.

| Pin | Use | Notes |
| --- | --- | --- |
| PA0 to PA7 | Analog inputs | 0 V to 3.3 V. **Not 5 V tolerant.** |
| PB0 to PB1 | Analog inputs | 0 V to 3.3 V. **Not 5 V tolerant.** |
| PB8 to PB15 | Digital inputs | 5 V tolerant only in **Floating** mode. |
| PA11, PA12 | USB | Connected on the board. |
| PC13 | Status LED | Blinks while samples are streaming. |
| PA13, PA14 | SWD | ST-Link for flashing. |

Connect signal grounds to the Blue Pill's GND. Analog readings use the board's
3.3 V supply as their reference, because VREF+ is tied to VDDA on this package.
Measure it for accurate voltages and enter the result as **Analog reference**.
Do not drive a digital input above 3.3 V while its pull-up or pull-down is on.

Some Blue Pills have the wrong resistor on D+ (R10 should be 1.5 kΩ). If the
board does not enumerate, or enumerates only on some computers, check this first.

## Clone

```sh
git clone --recursive <this-repository-url>
cd STM32F103_JSOSCOPE
```

For an existing clone:

```sh
git submodule update --init --recursive
```

## Build and flash the firmware

Install GNU Make and an Arm embedded GCC toolchain. Debian and Ubuntu need
`gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`, and
`libstdc++-arm-none-eabi-newlib`. Then:

```sh
make -C firmware -j
make -C firmware flash        # with st-flash and an ST-Link
make -C firmware flash_ocd    # or with OpenOCD and an ST-Link
```

To use another toolchain, add `GCC_PATH=/path/to/toolchain/bin`.

The output is in `firmware/build/`. The ST HAL, CMSIS, and USB device library are
vendored under `firmware/`, reduced to the files this build uses. The board
support comes from a CubeMX project, then edited by hand, so there is no `.ioc`
file to regenerate it from.

## Build and run the desktop companion

Install Rust 1.88 or newer, then:

```sh
cargo build --release --locked --manifest-path ui/JSOScope/server/Cargo.toml
./ui/JSOScope/target/release/jsoscope-server
```

Or download it from the
[JSOScope server releases](https://github.com/DariusGeorgeLukas/JSOScope/releases).

Its window lists compatible devices. Select **Connect**, then **Open browser**.
On Linux, membership in the `dialout` or `uucp` group normally provides access
to the serial port. `packaging/60-stm32f103-jsoscope.rules` is supplied for
systems that prefer a per-device `uaccess` rule.

Each board reports `stm32f103-bluepill-` followed by the MCU-derived USB serial
number as its device ID. The companion uses that stable ID to recognize the
same board on another port and restore that board's saved settings after it
reconnects. The firmware itself still starts from its defaults after power-up.

## Browser controls

The **Blue Pill inputs** panel is generated from the firmware's descriptor.
Change a setting and select **Apply**. The firmware validates the whole
selection, restarts sampling, and returns the accepted layout, and the UI
rebuilds its panels to match. It boots with PA0 and a pulled-up PB12 at 1 kHz.

| Control | Options |
| --- | --- |
| Sample rate | 100 Hz to 200 kHz |
| Analog PA0 to PA7, PB0 to PB1 | On or off; only PA0 defaults on |
| Digital PB8 to PB15 | Off, floating, pull-up, or pull-down; only PB12 defaults on |
| ADC sample time (advanced) | 1.5 to 239.5 ADC cycles, default 28.5 |
| Analog reference (advanced) | 2400 to 3600 mV, default 3300 |

Every enabled input is sampled once per row at the sample rate. Digital inputs
can drive the JSOScope trigger.

A selection is refused, with the reason shown in the panel, if the ADC cannot
finish a scan within one sample period or the stream would exceed the USB limit
of 600 kB/s. These are the fastest accepted rates:

| Analog inputs | No digital inputs | With digital inputs |
| --- | ---: | ---: |
| 0 | - | 100 kHz |
| 1 | 200 kHz | 50 kHz |
| 2 | 100 kHz | 50 kHz |
| 3 | 50 kHz | 50 kHz |
| 4 | 50 kHz | 20 kHz |
| 8 | 20 kHz | 20 kHz |
| 10 | 20 kHz | 20 kHz |

Analog inputs are converted one after another, so within a row they are one ADC
conversion apart. That is 3.4 µs at the default sample time. A longer sample
time suits a high-impedance source. The STM32F103 datasheet gives the maximum
source resistance for each sample time. The digital word is read at the start of the row, at the same timer tick
that starts the scan.

Analog resolution is 12 bits. PA4 to PA7 share pins with SPI1 and other
alternate functions, while PB0 and PB1 can also serve timer functions. This
firmware leaves those peripherals disabled and uses the pins only as analog
inputs. The UI shows readings in volts using the **Analog
reference** value. Because these are the ADC's raw results, expect a few counts
of noise and the accuracy given in the STM32F103 datasheet, not that of a
precision converter.

## Adding inputs

An input is two table entries. Add it to `firmware/src/scope_config.hpp` and to
the settings and field lists in `firmware/src/device_descriptor.hpp`. Nothing
changes in the server or UI. `scripts/test-firmware-host.sh` fails if the two
disagree. Analog inputs can use any ADC-capable pin supported by the sampler's
scan sequence. Digital inputs must be on GPIOB, because one DMA channel reads
the whole port. PB0 and PB1 are reserved for ADC1, PB2 is the BOOT1 pin, and
PB3 and PB4 belong to JTAG, so they are deliberately not offered as digital inputs.

## Testing

```sh
./scripts/test-firmware-host.sh
```

This needs no hardware. It covers:

- The protocol encoder and parser, and every setting and limit, with sanitizers.
  Every combination of inputs, rates, and sample times is checked against the
  DMA buffer sizes and the USB limit.
- The descriptor and runtime JSON against the JSOScope protocol's rules.
- The bench script against a simulated device, including injected faults the
  bench has to catch.
- If the server has been built, the real `jsoscope-server` against the simulated
  device, covering discovery, streaming, reconfiguration, and rejected settings.

The command first runs the independent `lib/jsoscope-device` protocol and
session tests, then the board-specific configuration and simulator tests.

The simulator shares only the parser with the firmware. These tests say nothing
about `firmware/src/sampler.cpp`, the ADC, or real USB timing. Those need the
[bench test](docs/bench-test.md).

See [architecture](docs/architecture.md) for how the firmware works.

## Differences from the RP2040 ADS1220 project

This project follows
[RP2040_ADS1220_JSOSCOPE](https://github.com/DariusGeorgeLukas/RP2040_ADS1220_JSOSCOPE)
and uses the same `jsoscope-device` library, server, and UI. The differences
come from the hardware:

- It samples with the chip's own 12-bit ADC and GPIO instead of an ADS1220, and
  sends `int16` analog columns plus a `uint32` word of digital inputs.
- A hardware timer sets the row rate exactly, so nothing is timed at runtime.
- It uses the ST HAL and USB stack rather than the Pico SDK and TinyUSB.
- There is no USB documentation drive. The drive's 1 MiB image does not fit in
  64 KB of flash, so the device is CDC only.

The firmware uses TinyUSB's `0xCAFE` development vendor ID, with product ID
`0x4014`. Obtain an assigned VID/PID before distributing hardware, and change
`firmware/USB_DEVICE/App/usbd_desc.c`. Device discovery uses the protocol
handshake, not USB identifiers.
