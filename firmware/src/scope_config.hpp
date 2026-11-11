#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The inputs, and everything about a configuration that can be decided without
// touching the hardware. firmware/src/sampler.cpp applies the result, and
// firmware/tests/host_test.cpp tests this file on the host.

inline constexpr std::size_t analog_input_count = 10;
inline constexpr std::size_t digital_input_count = 8;

// PA0 to PA7 and PB0 to PB1 are ADC1 channels 0 to 9.
inline constexpr std::array<const char*, analog_input_count> analog_input_names{{
    "PA0", "PA1", "PA2", "PA3", "PA4", "PA5", "PA6", "PA7", "PB0", "PB1",
}};
inline constexpr std::array<std::uint8_t, analog_input_count> analog_adc_channels{{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
}};

// Digital inputs share one port, because a single DMA channel reads the whole
// of GPIOB->IDR. A pin's bit in the sample word is its pin number.
inline constexpr std::array<const char*, digital_input_count> digital_input_names{{
    "PB8", "PB9", "PB10", "PB11", "PB12", "PB13", "PB14", "PB15",
}};
inline constexpr std::array<std::uint8_t, digital_input_count> digital_pins{{
    8, 9, 10, 11, 12, 13, 14, 15,
}};

enum class DigitalMode : std::uint8_t { off, floating, pull_up, pull_down };

// TIM3 counts at 1 MHz, so these are the rates it can produce exactly and the
// rate sent to the host needs no measurement.
inline constexpr std::uint32_t timer_tick_hz = 1'000'000;
inline constexpr std::array<std::uint32_t, 11> sample_rates_hz{{
    100, 200, 500, 1'000, 2'000, 5'000, 10'000, 20'000, 50'000, 100'000, 200'000,
}};

// ADCCLK is 72 MHz / 6. ADC_SMPRx codes run in the same order as this table.
inline constexpr std::uint32_t adc_clock_hz = 12'000'000;
inline constexpr std::array<const char*, 8> adc_sample_time_names{{
    "1.5", "7.5", "13.5", "28.5", "41.5", "55.5", "71.5", "239.5",
}};
inline constexpr std::array<std::uint16_t, 8> adc_sample_time_tenths{{
    15, 75, 135, 285, 415, 555, 715, 2395,
}};
inline constexpr std::uint32_t adc_conversion_tenths = 125;

// The most the firmware will stream, including framing. USB full speed bulk
// can carry more, but this leaves room for host scheduling and has not yet
// been measured on hardware. See docs/bench-test.md.
inline constexpr std::uint32_t max_bytes_per_second = 600'000;

// Samples reach the host within about this time at low rates.
inline constexpr std::uint32_t target_block_ms = 64;

inline constexpr std::uint16_t min_reference_mv = 2400;
inline constexpr std::uint16_t max_reference_mv = 3600;

struct ScopeConfiguration {
    std::uint32_t sample_rate_hz{};
    std::uint16_t reference_mv{};
    std::uint16_t rows_per_block{};
    std::uint8_t sample_time_index{};
    std::array<bool, analog_input_count> analog{};
    std::array<DigitalMode, digital_input_count> digital{};
    std::uint8_t analog_count{};
    std::uint8_t digital_count{};
    // Enabled pins, as bits of GPIOB->IDR.
    std::uint16_t digital_mask{};

    // One int16 per analog input, then one uint32 holding every digital input.
    constexpr std::size_t row_bytes() const {
        return analog_count * sizeof(std::int16_t) +
               (digital_count != 0 ? sizeof(std::uint32_t) : 0u);
    }
};

// Worst-case bytes per second for this configuration, with record framing.
std::uint32_t wire_bytes_per_second(const ScopeConfiguration& configuration);

bool parse_scope_configuration(const std::uint8_t* json, std::size_t length,
                               ScopeConfiguration& output,
                               char* error, std::size_t error_capacity);

// Both return the JSON length, or 0 when it does not fit. Settings are written
// from the parsed configuration, so the host always sees what is actually
// running rather than an echo of what it sent.
std::size_t build_stream_config(const ScopeConfiguration& configuration,
                                char* output, std::size_t capacity);
std::size_t build_settings_json(const ScopeConfiguration& configuration,
                                char* output, std::size_t capacity);

// {"stream":...,"settings":...}, the body of CONFIGURE_RESULT and STREAM_CONFIG.
std::size_t build_runtime_config(const ScopeConfiguration& configuration,
                                 char* output, std::size_t capacity);

// Packs rows for SAMPLE_DATA. `adc` holds analog_count left-aligned ADC results
// per row and `gpio` one GPIOB->IDR read per row. Returns the bytes written.
std::size_t pack_sample_rows(const ScopeConfiguration& configuration,
                             const std::uint16_t* adc, const std::uint16_t* gpio,
                             std::uint8_t* output);
