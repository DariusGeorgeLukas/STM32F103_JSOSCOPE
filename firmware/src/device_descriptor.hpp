#pragma once

#include <cstddef>

// What this device is and what can be configured. The Rust server and the
// JSOScope UI know none of this in advance: they read it from here, build the
// controls from it, and send the chosen settings back as an opaque object for
// parse_scope_configuration() to validate.
//
// To add an input, add it to the tables in scope_config.hpp and to the lists
// below. host_test.cpp fails if the two disagree.
namespace device_descriptor {

#define SCOPE_ANALOG_FIELD(pin, name)                                                   \
    R"json({"key":"analog_)json" pin R"json(","label":"Analog )json" name               \
    R"json(","type":"checkbox"},)json"

#define SCOPE_DIGITAL_FIELD(pin, name)                                                  \
    R"json({"key":"digital_)json" pin R"json(","label":"Digital )json" name             \
    R"json(","type":"select","options":[)json"                                          \
    R"json({"value":"off","label":"Off"},)json"                                         \
    R"json({"value":"floating","label":"Floating"},)json"                               \
    R"json({"value":"pull-up","label":"Pull-up"},)json"                                 \
    R"json({"value":"pull-down","label":"Pull-down"}]},)json"

// PA0 and a pulled-up PB12 at 1 kHz, so a board shows one trace of each kind as
// soon as it is connected.
#define SCOPE_DEFAULT_SETTINGS \
    R"json({"sample_rate":1000,)json" \
    R"json("analog_pa0":true,"analog_pa1":false,)json" \
    R"json("analog_pa2":false,"analog_pa3":false,)json" \
    R"json("analog_pa4":false,"analog_pa5":false,)json" \
    R"json("analog_pa6":false,"analog_pa7":false,)json" \
    R"json("analog_pb0":false,"analog_pb1":false,)json" \
    R"json("digital_pb8":"off","digital_pb9":"off",)json" \
    R"json("digital_pb10":"off","digital_pb11":"off",)json" \
    R"json("digital_pb12":"pull-up","digital_pb13":"off",)json" \
    R"json("digital_pb14":"off","digital_pb15":"off",)json" \
    R"json("adc_sample_time":"28.5","reference_mv":3300})json"

inline constexpr char default_settings[] = SCOPE_DEFAULT_SETTINGS;

// The MCU-derived serial is inserted between these pieces by ScopeInstrument.
// Keeping it separate avoids holding a complete descriptor copy in scarce RAM.
inline constexpr char before_serial[] = R"json({
  "protocol":{"name":"jsoscope-device","major":2,"minor":1},
  "device":{"id":"stm32f103-bluepill-)json";

inline constexpr char after_serial[] = R"json(","name":"STM32F103 Blue Pill"},
  "capabilities":{"configuration":true,"streaming":true},
  "limits":{"max_sample_rate_hz":200000.0,"max_bytes_per_second":600000},
  "stream":{
    "source_name":"STM32F103 Blue Pill",
    "sample_rate_hz":1000,
    "channels":[
      {"name":"PA0","kind":"analog","type":"int16","offset":0,
       "min":0,"max":3.300},
      {"name":"PB12","kind":"digital","type":"uint32","offset":2,"bit":12}
    ],
    "frame":{"row_stride_bytes":6,"samples_per_frame":64}
  },
  "configuration":{
    "title":"Blue Pill inputs",
    "defaults":)json" SCOPE_DEFAULT_SETTINGS R"json(,
    "fields":[
      {"key":"sample_rate","label":"Sample rate","type":"select","suffix":"Hz",
       "options":[100,200,500,1000,2000,5000,10000,20000,50000,100000,200000]},
      )json"
    SCOPE_ANALOG_FIELD("pa0", "PA0") SCOPE_ANALOG_FIELD("pa1", "PA1")
    SCOPE_ANALOG_FIELD("pa2", "PA2") SCOPE_ANALOG_FIELD("pa3", "PA3")
    SCOPE_ANALOG_FIELD("pa4", "PA4") SCOPE_ANALOG_FIELD("pa5", "PA5")
    SCOPE_ANALOG_FIELD("pa6", "PA6") SCOPE_ANALOG_FIELD("pa7", "PA7")
    SCOPE_ANALOG_FIELD("pb0", "PB0") SCOPE_ANALOG_FIELD("pb1", "PB1")
    SCOPE_DIGITAL_FIELD("pb8", "PB8") SCOPE_DIGITAL_FIELD("pb9", "PB9")
    SCOPE_DIGITAL_FIELD("pb10", "PB10") SCOPE_DIGITAL_FIELD("pb11", "PB11")
    SCOPE_DIGITAL_FIELD("pb12", "PB12") SCOPE_DIGITAL_FIELD("pb13", "PB13")
    SCOPE_DIGITAL_FIELD("pb14", "PB14") SCOPE_DIGITAL_FIELD("pb15", "PB15")
    R"json(
      {"key":"adc_sample_time","label":"ADC sample time","type":"select","suffix":"cycles",
       "advanced":true,"options":["1.5","7.5","13.5","28.5","41.5","55.5","71.5","239.5"]},
      {"key":"reference_mv","label":"Analog reference","type":"number","suffix":"mV",
       "advanced":true,"min":2400,"max":3600,"step":1}
    ]
  }
})json";

#undef SCOPE_ANALOG_FIELD
#undef SCOPE_DIGITAL_FIELD
#undef SCOPE_DEFAULT_SETTINGS

inline constexpr std::size_t serial_length = 12;
inline constexpr std::size_t length =
    sizeof(before_serial) - 1 + serial_length + sizeof(after_serial) - 1;

}  // namespace device_descriptor
