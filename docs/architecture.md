# Architecture

The project keeps the acquisition firmware, desktop server, and browser UI
independent. The firmware owns everything about the hardware, and the two host
programs learn it at runtime.

```text
lib/jsoscope-device/                  protocol v2.1 framing and generic session
firmware/src/device_descriptor.hpp   what can be configured (JSON, stored in flash)
firmware/src/scope_config.cpp        validates settings, builds the stream layout
firmware/src/sampler.cpp             TIM3, ADC1, DMA, GPIO
firmware/src/app.cpp                 Instrument and USB adapters, main loop
```

`jsoscope::DeviceSession` owns framing and request handling. `ScopeInstrument`
adapts the configuration and DMA sampler to the library's instrument interface,
and `UsbCdcTransport` adapts the ST USB CDC stack to its byte-stream interface.
`sampler.cpp` and `app.cpp` need the STM32. `scope_config.cpp` and the shared
device library do not, and are built on the host for testing.

## Discovery

1. The Rust server opens each USB serial port and sends `HELLO_REQUEST`. A port
   that answers is a JSOScope device. The firmware does not need to be a known
   model, and the server does not check USB identifiers.
2. The server downloads the JSON descriptor in chunks. It holds the device name,
   maximum rates, the default stream, and the field definitions for the
   configuration panel.
3. When the user selects **Connect**, the server sends `STREAM_CONFIG` to read the
   settings and layout that are actually running, so reconnecting never resets
   the hardware.
4. The browser receives the stream description plus `device_controls`, and builds
   the panel from those fields.
5. **Apply** sends the complete settings object. The server forwards it unchanged
   in `CONFIGURE`. It has no idea what a sample time or a pull-up is.
6. The firmware validates the settings, restarts sampling, and returns the
   accepted layout, or a message that the UI displays.

The canonical definition of all of this is
[`ui/JSOScope/docs/device-protocol-v2.md`](../ui/JSOScope/docs/device-protocol-v2.md).

## Sampling

TIM3 counts at 1 MHz, and its update event starts every row. It does two things
at once with no CPU involved:

```text
TIM3 update --> TRGO --> ADC1 scan of the enabled inputs --> DMA1 channel 1 --> adc_buffer
           \--> TIM3_UP DMA request --> GPIOB->IDR read ---> DMA1 channel 3 --> gpio_buffer
```

ADC1 scans PA0 through PA7 followed by PB0 and PB1 when those inputs are enabled.

Because one event drives both, row N of `adc_buffer` and row N of `gpio_buffer`
come from the same tick. The two can only slip apart if a trigger reaches the
ADC during a scan, because the ADC ignores it. `parse_scope_configuration` refuses
any configuration whose scan does not fit in a row period, with two ADC clocks of
margin.

The sample rates are all exact divisors of the 1 MHz tick, so the rate reported to
the host is correct to the accuracy of the crystal. The RP2040 project has to time
its converter. This one does not.

Each buffer holds two blocks, and DMA runs in circular mode. The half-transfer and
transfer-complete interrupts count finished blocks, and the main loop reads the half
the DMA is not writing. With analog inputs enabled, the interrupt comes from the ADC
channel, because a row's conversions finish after its GPIO read. In digital-only
configurations it comes from the GPIO channel.

The ADC is left-aligned, so a 12-bit result fills the top of a 16-bit word.
Flipping the top bit gives an `int16` whose full range spans 0 V to the reference,
which is how the UI interprets `min` and `max` for that type.

## Row and block layout

```text
[int16 for each enabled analog input] [uint32 of digital inputs, if any]
```

A digital channel's `bit` is its pin number in `GPIOB->IDR`, so PB8 is bit 8 and
PB12 is bit 12. All digital channels share the word. It is little-endian, and it is only 4-byte
aligned when an even number of analog columns comes before it, which the UI
handles.

A block holds as many rows as fit in 512 bytes, limited to about 64 ms of data, so
low rates stay responsive and high rates use the USB bandwidth efficiently.

## Main loop and USB

The main loop calls `DeviceSession::service()` and updates the activity LED.
The session writes any pending response or sample record, reads and handles
requests, then asks the instrument for one finished sample block. It keeps only
one encoded record in flight, so a slow host naturally applies back pressure.

The USB receive callback copies endpoint data into a 512-byte ring consumed by
the session. On transmit, the adapter copies one complete encoded record into
storage it owns before handing it to the asynchronous ST USB stack, which splits
it into 64-byte endpoint packets. This is required by the shared transport
contract: once `write()` accepts bytes, the session is free to reuse its record
buffer even though the endpoint transfer has not completed yet.

The DMA sampler itself remains double-buffered. If the host falls behind, its
next read takes the newest safe half and older completed halves are discarded.
PC13 blinks while sample blocks are flowing.

## Reconfiguration

`ScopeInstrument::configure` builds the response first, so an oversized reply
cannot leave the hardware running a configuration the host was told had failed.
It then stops sampling, applies the accepted configuration, advances its
generation, and restarts. `DeviceSession` sends the configuration response before
it asks for another sample block.

Settings in the response are written from the parsed configuration, not echoed
from the request, so the host always sees what is running.

The ADC is recalibrated at each restart, which takes a millisecond or two.
Calibration finishes before the ADC's DMA request is enabled, because it leaves
its result in `ADC_DR`, and that value would otherwise become the first sample.

## Resources

| | Used | Available |
| --- | ---: | ---: |
| Flash | 34 KB | 64 KB |
| RAM, including 2 KB of stack and 512 B heap reserve | 18 KB | 20 KB |

Floating-point printing and parsing are deliberately unused, to save flash. Every
number in the settings is a whole number, and voltages are held in millivolts.

The server binds to `127.0.0.1`, not all network interfaces, which prevents
unintended LAN access to the measurement stream.
