// Host-side tests for the hardware-independent firmware code. Run them with
// scripts/test-firmware-host.sh.
//
//   host_test              runs the tests
//   host_test --dump       prints the descriptor and a set of runtime
//                          configurations, one per line, for
//                          scripts/check_firmware_json.py
//   host_test --configure  reads settings on stdin and prints the runtime
//                          configuration, or the error with exit status 1, so
//                          the simulated device uses the real parser

#include "device_descriptor.hpp"
#include "jsoscope/protocol.hpp"
#include "scope_config.hpp"

#include <array>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

std::string descriptor_json() {
    return std::string(device_descriptor::before_serial) + "000000000000" +
           device_descriptor::after_serial;
}

using Settings = std::map<std::string, std::string>;

std::string analog_key(std::size_t index) {
    std::string key = "analog_";
    for (const char value : std::string(analog_input_names[index])) {
        key += static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    }
    return key;
}

Settings default_settings() {
    Settings settings;
    settings["sample_rate"] = "1000";
    settings["analog_pa0"] = "true";
    settings["analog_pa1"] = "false";
    settings["analog_pa2"] = "false";
    settings["analog_pa3"] = "false";
    settings["analog_pa4"] = "false";
    settings["analog_pa5"] = "false";
    settings["analog_pa6"] = "false";
    settings["analog_pa7"] = "false";
    settings["analog_pb0"] = "false";
    settings["analog_pb1"] = "false";
    settings["digital_pb8"] = "\"off\"";
    settings["digital_pb9"] = "\"off\"";
    settings["digital_pb10"] = "\"off\"";
    settings["digital_pb11"] = "\"off\"";
    settings["digital_pb12"] = "\"pull-up\"";
    settings["digital_pb13"] = "\"off\"";
    settings["digital_pb14"] = "\"off\"";
    settings["digital_pb15"] = "\"off\"";
    settings["adc_sample_time"] = "\"28.5\"";
    settings["reference_mv"] = "3300";
    return settings;
}

Settings all_inputs() {
    auto settings = default_settings();
    for (unsigned index = 0; index < analog_input_count; ++index) {
        settings[analog_key(index)] = "true";
    }
    for (unsigned pin = 8; pin < 8 + digital_input_count; ++pin) {
        settings["digital_pb" + std::to_string(pin)] =
            pin % 3 == 0 ? "\"pull-down\"" : pin % 3 == 1 ? "\"pull-up\"" : "\"floating\"";
    }
    return settings;
}

std::string to_json(const Settings& settings) {
    std::string json = "{";
    for (const auto& [key, value] : settings) {
        if (json.size() > 1) json += ",";
        json += "\"" + key + "\":" + value;
    }
    return json + "}";
}

bool parse(const std::string& json, ScopeConfiguration& output, std::string* message = nullptr) {
    std::array<char, 96> error{};
    const bool accepted = parse_scope_configuration(
        reinterpret_cast<const std::uint8_t*>(json.data()), json.size(), output, error.data(),
        error.size());
    if (message != nullptr) *message = error.data();
    return accepted;
}

ScopeConfiguration accepted(const Settings& settings) {
    ScopeConfiguration configuration{};
    std::string message;
    if (!parse(to_json(settings), configuration, &message)) {
        std::fprintf(stderr, "unexpected rejection: %s\n", message.c_str());
        assert(false);
    }
    return configuration;
}

void expect_rejected(const Settings& settings, const char* expected) {
    ScopeConfiguration configuration{};
    std::string message;
    assert(!parse(to_json(settings), configuration, &message));
    if (message.find(expected) == std::string::npos) {
        std::fprintf(stderr, "wrong error: %s\n", message.c_str());
        assert(false);
    }
}

std::string runtime_json(const ScopeConfiguration& configuration) {
    std::array<char, protocol::max_control_body_bytes> output{};
    const auto length = build_runtime_config(configuration, output.data(), output.size());
    assert(length != 0);
    return std::string(output.data(), length);
}

// A host's view of the wire format, independent of the firmware's encoder.
std::vector<std::uint8_t> cobs_decode(const std::uint8_t* encoded, std::size_t length) {
    std::vector<std::uint8_t> decoded;
    std::size_t position = 0;
    while (position < length) {
        const auto code = encoded[position++];
        assert(code != 0);
        for (std::size_t i = 1; i < code; ++i) {
            assert(position < length);
            decoded.push_back(encoded[position++]);
        }
        if (code != 0xFF && position < length) decoded.push_back(0);
    }
    return decoded;
}

std::vector<std::uint8_t> host_request(protocol::DeviceMessageType type, std::uint32_t sequence,
                                       const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> wire(protocol::wire_bytes_for_body(body.size()));
    const auto size = protocol::encode_device_message(type, sequence, body.data(), body.size(),
                                                      wire.data(), wire.size());
    assert(size != 0);
    wire.resize(size);
    return wire;
}

void test_crc() {
    const char check[] = "123456789";
    assert(protocol::crc16_kermit(reinterpret_cast<const std::uint8_t*>(check), 9) == 0x2189);
}

void test_encoder_matches_the_record_format() {
    std::vector<std::uint8_t> body(600);
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<std::uint8_t>(i * 7);
    std::vector<std::uint8_t> wire(protocol::wire_bytes_for_body(body.size()));
    const auto size = protocol::encode_device_message(
        protocol::DeviceMessageType::sample_data, 0x01020304, body.data(), body.size(),
        wire.data(), wire.size());
    assert(size != 0 && size <= wire.size());
    assert(wire[size - 1] == 0);
    for (std::size_t i = 0; i + 1 < size; ++i) assert(wire[i] != 0);

    const auto raw = cobs_decode(wire.data(), size - 1);
    assert(raw.size() == 1 + 2 + protocol::device_header_size + body.size() + 2);
    assert(raw[0] == 1);
    assert(static_cast<std::size_t>(raw[1] | (raw[2] << 8)) ==
           protocol::device_header_size + body.size());
    assert(std::memcmp(raw.data() + 3, "JSO2", 4) == 0);
    assert(raw[7] == 2 && raw[8] == 1 && raw[9] == 8 && raw[10] == 0);
    assert(raw[11] == 4 && raw[12] == 3 && raw[13] == 2 && raw[14] == 1);
    assert(std::memcmp(raw.data() + 17, body.data(), body.size()) == 0);
    const auto crc = protocol::crc16_kermit(raw.data(), raw.size() - 2);
    assert(raw[raw.size() - 2] == static_cast<std::uint8_t>(crc));
    assert(raw[raw.size() - 1] == static_cast<std::uint8_t>(crc >> 8));
}

void test_encoder_handles_cobs_extremes() {
    // A body with no zero bytes reaches the 254-byte COBS block limit. A body
    // of zeros reaches the opposite case. The worst-case size must hold.
    for (const std::uint8_t fill : {std::uint8_t{0x55}, std::uint8_t{0}}) {
        const std::vector<std::uint8_t> body(protocol::max_control_body_bytes, fill);
        std::vector<std::uint8_t> wire(protocol::max_control_wire_bytes);
        const auto size = protocol::encode_device_message(
            protocol::DeviceMessageType::stream_config, 1, body.data(), body.size(), wire.data(),
            wire.size());
        assert(size != 0);
        const auto raw = cobs_decode(wire.data(), size - 1);
        assert(std::memcmp(raw.data() + 17, body.data(), body.size()) == 0);
    }
}

void test_encoder_refuses_what_does_not_fit() {
    const std::vector<std::uint8_t> body(100, 1);
    std::vector<std::uint8_t> wire(protocol::wire_bytes_for_body(100) - 1);
    assert(protocol::encode_device_message(protocol::DeviceMessageType::status, 1, body.data(),
                                           body.size(), wire.data(), wire.size()) == 0);
    assert(protocol::encode_device_message(protocol::DeviceMessageType::status, 1, nullptr, 4,
                                           wire.data(), wire.size()) == 0);
}

bool push_all(protocol::DeviceMessageParser& parser, const std::vector<std::uint8_t>& wire,
              protocol::DeviceMessage& message) {
    bool complete = false;
    for (const auto value : wire) complete = parser.push(value, message) || complete;
    return complete;
}

void test_parser_round_trip() {
    protocol::DeviceMessageParser parser;
    protocol::DeviceMessage message;
    const std::vector<std::uint8_t> body{0, 1, 0, 0, 2, 3, 0, 0};
    const auto wire = host_request(protocol::DeviceMessageType::configure, 77, body);
    assert(push_all(parser, wire, message));
    assert(message.type == protocol::DeviceMessageType::configure);
    assert(message.sequence == 77);
    assert(message.body_length == body.size());
    assert(std::memcmp(message.body, body.data(), body.size()) == 0);
}

void test_parser_rejects_damage_and_recovers() {
    protocol::DeviceMessageParser parser;
    protocol::DeviceMessage message;
    auto wire = host_request(protocol::DeviceMessageType::hello_request, 1, {});
    wire[wire.size() / 2] ^= 0x10;
    assert(!push_all(parser, wire, message));

    // Noise before a frame only costs that frame.
    assert(!push_all(parser, {0x41, 0x42, 0x43, 0}, message));
    assert(push_all(parser, host_request(protocol::DeviceMessageType::hello_request, 2, {}),
                    message));
    assert(message.sequence == 2);
}

void test_parser_survives_oversized_frames() {
    protocol::DeviceMessageParser parser;
    protocol::DeviceMessage message;
    std::vector<std::uint8_t> flood(protocol::max_request_wire_bytes * 3, 0x41);
    assert(!push_all(parser, flood, message));
    assert(!parser.push(0, message));
    assert(push_all(parser, host_request(protocol::DeviceMessageType::stream_config, 3, {}),
                    message));
}

void test_parser_accepts_the_largest_settings_object() {
    const auto configuration = accepted(all_inputs());
    std::array<char, protocol::max_request_body_bytes> json{};
    const auto length = build_settings_json(configuration, json.data(), json.size());
    assert(length != 0);
    std::vector<std::uint8_t> body(json.begin(), json.begin() + static_cast<long>(length));
    protocol::DeviceMessageParser parser;
    protocol::DeviceMessage message;
    assert(push_all(parser, host_request(protocol::DeviceMessageType::configure, 4, body), message));
    assert(message.body_length == length);

    // Pretty-printed JSON from another client must fit as well.
    std::string spaced;
    for (const char value : std::string(json.data(), length)) {
        spaced += value;
        if (value == ',' || value == ':') spaced += ' ';
    }
    assert(spaced.size() <= protocol::max_request_body_bytes);
    ScopeConfiguration reparsed{};
    assert(parse(spaced, reparsed));
    assert(reparsed.analog_count == analog_input_count);
    assert(reparsed.digital_count == digital_input_count);
}

void test_defaults() {
    ScopeConfiguration configuration{};
    assert(parse(device_descriptor::default_settings, configuration));
    assert(configuration.sample_rate_hz == 1000);
    assert(configuration.analog_count == 1 && configuration.analog[0]);
    assert(configuration.digital_count == 1);
    assert(configuration.digital[4] == DigitalMode::pull_up);
    assert(configuration.digital_mask == (1u << 12));
    assert(configuration.row_bytes() == 6);
    assert(configuration.rows_per_block == 64);
    assert(accepted(default_settings()).row_bytes() == 6);
}

void test_layouts() {
    auto settings = default_settings();
    settings["digital_pb12"] = "\"off\"";
    assert(accepted(settings).row_bytes() == 2);

    settings = default_settings();
    settings["analog_pa0"] = "false";
    const auto digital_only = accepted(settings);
    assert(digital_only.analog_count == 0);
    assert(digital_only.row_bytes() == 4);

    assert(accepted(all_inputs()).row_bytes() == 24);
    assert(accepted(all_inputs()).digital_mask == 0xFF00);
}

void test_rejections() {
    auto settings = default_settings();
    settings["sample_rate"] = "1234";
    expect_rejected(settings, "sample rate is unsupported");

    settings = default_settings();
    settings["sample_rate"] = "1000.5";
    expect_rejected(settings, "wrong type");

    settings = default_settings();
    settings["sample_rate"] = "-1000";
    expect_rejected(settings, "wrong type");

    settings = default_settings();
    settings["sample_rate"] = "\"1000\"";
    expect_rejected(settings, "wrong type");

    settings = default_settings();
    settings.erase("analog_pa2");
    expect_rejected(settings, "wrong type");

    settings = default_settings();
    settings["analog_pa1"] = "1";
    expect_rejected(settings, "wrong type");

    settings = default_settings();
    settings["digital_pb13"] = "\"pulled\"";
    expect_rejected(settings, "digital input mode");

    settings = default_settings();
    settings["adc_sample_time"] = "\"2\"";
    expect_rejected(settings, "ADC sample time");

    settings = default_settings();
    settings["reference_mv"] = "2399";
    expect_rejected(settings, "analog reference");
    settings["reference_mv"] = "3601";
    expect_rejected(settings, "analog reference");

    settings = default_settings();
    settings["analog_pa0"] = "false";
    settings["digital_pb12"] = "\"off\"";
    expect_rejected(settings, "at least one input");

    ScopeConfiguration configuration{};
    std::string message;
    assert(!parse("", configuration, &message));
    assert(!parse("not json", configuration, &message));
    assert(!parse("{\"sample_rate\":", configuration, &message));
}

void test_a_rejected_configuration_leaves_the_output_alone() {
    auto configuration = accepted(default_settings());
    const auto before = configuration.sample_rate_hz;
    auto settings = default_settings();
    settings["sample_rate"] = "7";
    assert(!parse(to_json(settings), configuration));
    assert(configuration.sample_rate_hz == before);
}

void test_adc_time_budget() {
    // A row at 200 kHz is 60 ADC clocks. 41 fit, and 54 (41.5 + 12.5) fit
    // exactly with the margin, but 68 (55.5 + 12.5) do not.
    auto settings = default_settings();
    settings["digital_pb12"] = "\"off\"";
    settings["sample_rate"] = "200000";
    settings["adc_sample_time"] = "\"41.5\"";
    accepted(settings);
    settings["adc_sample_time"] = "\"55.5\"";
    expect_rejected(settings, "ADC sample time");

    // Ten inputs at the slowest sample time take 2520 clocks, or 210 us.
    settings = all_inputs();
    settings["adc_sample_time"] = "\"239.5\"";
    settings["sample_rate"] = "20000";
    expect_rejected(settings, "ADC sample time");
    settings["sample_rate"] = "5000";
    expect_rejected(settings, "ADC sample time");
    settings["sample_rate"] = "2000";
    accepted(settings);
}

void test_usb_budget() {
    auto settings = default_settings();
    settings["digital_pb12"] = "\"off\"";
    settings["sample_rate"] = "200000";
    settings["adc_sample_time"] = "\"1.5\"";
    assert(wire_bytes_per_second(accepted(settings)) <= max_bytes_per_second);

    settings["analog_pa1"] = "true";
    expect_rejected(settings, "too fast for USB");

    settings = all_inputs();
    settings["adc_sample_time"] = "\"1.5\"";
    settings["sample_rate"] = "20000";
    accepted(settings);
    settings["sample_rate"] = "50000";
    expect_rejected(settings, "too fast for USB");

    // The descriptor promises the host this limit.
    assert(descriptor_json()
               .find("\"max_bytes_per_second\":" + std::to_string(max_bytes_per_second)) !=
           std::string::npos);
    assert(descriptor_json()
               .find("\"max_sample_rate_hz\":" + std::to_string(sample_rates_hz.back()) + ".0") !=
           std::string::npos);
}

void test_every_accepted_block_fits_the_buffers() {
    // Every input count at every rate and sample time. Pin identity does not
    // affect the size and timing limits; separate round-trip tests cover every
    // individual setting key.
    std::size_t accepted_count = 0;
    for (unsigned analog = 0; analog <= analog_input_count; ++analog) {
        for (unsigned digital = 0; digital <= digital_input_count; ++digital) {
            for (const auto rate : sample_rates_hz) {
                for (const auto* time : adc_sample_time_names) {
                    auto settings = default_settings();
                    for (unsigned i = 0; i < analog_input_count; ++i) {
                        settings[analog_key(i)] = i < analog ? "true" : "false";
                    }
                    for (unsigned i = 0; i < digital_input_count; ++i) {
                        settings["digital_pb" + std::to_string(8 + i)] =
                            i < digital ? "\"floating\"" : "\"off\"";
                    }
                    settings["sample_rate"] = std::to_string(rate);
                    settings["adc_sample_time"] = std::string("\"") + time + "\"";
                    ScopeConfiguration configuration{};
                    if (!parse(to_json(settings), configuration)) continue;
                    ++accepted_count;
                    assert(configuration.rows_per_block >= 1);
                    const auto block = configuration.rows_per_block * configuration.row_bytes();
                    assert(block <= protocol::max_sample_payload_bytes);
                    assert(block % configuration.row_bytes() == 0);
                    // The DMA buffers in sampler.cpp hold this many values.
                    assert(configuration.rows_per_block * configuration.analog_count <= 256);
                    if (configuration.digital_count != 0) {
                        assert(configuration.rows_per_block <= 128);
                    }
                    assert(wire_bytes_per_second(configuration) <= max_bytes_per_second);
                    // A block never holds back the display for long.
                    const auto block_ms =
                        configuration.rows_per_block * 1000u / configuration.sample_rate_hz;
                    assert(block_ms <= target_block_ms);
                    assert(!runtime_json(configuration).empty());
                }
            }
        }
    }
    assert(accepted_count > 500);
}

void test_low_rates_still_send_blocks() {
    auto settings = default_settings();
    settings["sample_rate"] = "100";
    const auto configuration = accepted(settings);
    assert(configuration.rows_per_block == 6);
}

void test_every_rate_is_exact() {
    for (const auto rate : sample_rates_hz) {
        assert(timer_tick_hz % rate == 0);
        assert(timer_tick_hz / rate - 1u <= 0xFFFFu);
        assert(adc_clock_hz % rate == 0);
    }
}

void test_settings_round_trip() {
    for (const auto& settings : {default_settings(), all_inputs()}) {
        const auto configuration = accepted(settings);
        std::array<char, 512> json{};
        const auto length = build_settings_json(configuration, json.data(), json.size());
        assert(length != 0);
        ScopeConfiguration reparsed{};
        assert(parse(std::string(json.data(), length), reparsed));
        assert(reparsed.sample_rate_hz == configuration.sample_rate_hz);
        assert(reparsed.reference_mv == configuration.reference_mv);
        assert(reparsed.sample_time_index == configuration.sample_time_index);
        assert(reparsed.analog == configuration.analog);
        assert(reparsed.digital == configuration.digital);
    }
}

void test_defaults_match_the_descriptor_text() {
    const auto configuration = accepted(default_settings());
    std::array<char, 512> json{};
    const auto length = build_settings_json(configuration, json.data(), json.size());
    assert(std::string(json.data(), length) == device_descriptor::default_settings);
}

void test_tiny_buffers_fail_cleanly() {
    const auto configuration = accepted(all_inputs());
    const auto full = runtime_json(configuration);
    std::vector<char> output(full.size() + 1, '#');
    for (std::size_t capacity = 0; capacity <= full.size(); ++capacity) {
        assert(build_runtime_config(configuration, output.data(), capacity) == 0);
    }
    assert(build_runtime_config(configuration, output.data(), output.size()) == full.size());
}

void test_largest_response_fits_control_messages() {
    const auto configuration = accepted(all_inputs());
    const auto json = runtime_json(configuration);
    // CONFIGURE_RESULT adds five bytes to this.
    assert(json.size() + 5 <= protocol::max_control_body_bytes);
    std::fprintf(stderr, "largest runtime configuration: %zu bytes of %zu\n", json.size() + 5,
                 protocol::max_control_body_bytes);
}

void test_analog_scaling() {
    auto settings = default_settings();
    settings["digital_pb12"] = "\"off\"";
    settings["sample_rate"] = "100";
    const auto configuration = accepted(settings);
    assert(configuration.rows_per_block == 6);

    // Left-aligned 12-bit results: 0, mid scale, and full scale.
    const std::array<std::uint16_t, 6> adc{0x0000, 0x8000, 0xFFF0, 0x0010, 0x7FF0, 0x8010};
    std::array<std::uint8_t, 12> rows{};
    assert(pack_sample_rows(configuration, adc.data(), nullptr, rows.data()) == rows.size());
    const auto sample = [&](std::size_t row) {
        return static_cast<std::int16_t>(rows[row * 2] | (rows[row * 2 + 1] << 8));
    };
    assert(sample(0) == -32768);
    assert(sample(1) == 0);
    assert(sample(2) == 32752);
    assert(sample(3) == -32752);
    assert(sample(4) == -16);
    assert(sample(5) == 16);
}

void test_analog_and_digital_rows() {
    auto settings = all_inputs();
    settings["digital_pb13"] = "\"off\"";
    settings["sample_rate"] = "100";
    const auto configuration = accepted(settings);
    assert(configuration.row_bytes() == 24);
    assert(configuration.rows_per_block == 6);

    std::array<std::uint16_t, 60> adc{};
    for (std::size_t i = 0; i < adc.size(); ++i) {
        adc[i] = static_cast<std::uint16_t>(0x8000u + (i << 4));
    }
    // Bits outside the enabled pins must not reach the host. PB13 is off.
    const std::array<std::uint16_t, 6> gpio{0xFFFF, 0x0000, 0x0100, 0x2000, 0x4000, 0x80FF};
    std::array<std::uint8_t, 144> rows{};
    assert(pack_sample_rows(configuration, adc.data(), gpio.data(), rows.data()) == rows.size());

    const auto word = [&](std::size_t row) {
        const auto* base = rows.data() + row * 24 + 20;
        return static_cast<std::uint32_t>(base[0]) | (static_cast<std::uint32_t>(base[1]) << 8) |
               (static_cast<std::uint32_t>(base[2]) << 16) |
               (static_cast<std::uint32_t>(base[3]) << 24);
    };
    const auto analog = [&](std::size_t row, std::size_t column) {
        const auto* base = rows.data() + row * 24 + column * 2;
        return static_cast<std::int16_t>(base[0] | (base[1] << 8));
    };
    assert(word(0) == 0xDF00);
    assert(word(1) == 0);
    assert(word(2) == 0x0100);
    assert(word(3) == 0);
    assert(word(4) == 0x4000);
    assert(word(5) == 0x8000);
    assert(analog(0, 0) == 0);
    assert(analog(0, 9) == 9 * 16);
    assert(analog(1, 0) == 10 * 16);
    assert(analog(5, 9) == 59 * 16);
}

void test_digital_only_rows() {
    auto settings = default_settings();
    settings["analog_pa0"] = "false";
    settings["sample_rate"] = "100";
    const auto configuration = accepted(settings);
    const std::array<std::uint16_t, 6> gpio{0x1000, 0, 0x1000, 0, 0x1000, 0xEFFF};
    std::array<std::uint8_t, 24> rows{};
    assert(pack_sample_rows(configuration, nullptr, gpio.data(), rows.data()) == rows.size());
    assert(rows[1] == 0x10 && rows[5] == 0 && rows[9] == 0x10 && rows[21] == 0);
}

void dump_json() {
    std::string descriptor = descriptor_json();
    for (auto& value : descriptor) {
        if (value == '\n') value = ' ';
    }
    std::cout << descriptor << "\n";
    std::cout << runtime_json(accepted(default_settings())) << "\n";
    std::cout << runtime_json(accepted(all_inputs())) << "\n";

    auto settings = default_settings();
    settings["digital_pb12"] = "\"off\"";
    settings["sample_rate"] = "200000";
    std::cout << runtime_json(accepted(settings)) << "\n";

    settings = default_settings();
    settings["analog_pa0"] = "false";
    settings["digital_pb13"] = "\"pull-down\"";
    std::cout << runtime_json(accepted(settings)) << "\n";

    settings = all_inputs();
    settings["reference_mv"] = "3275";
    std::cout << runtime_json(accepted(settings)) << "\n";
}

int configure_from_stdin() {
    const std::string settings{std::istreambuf_iterator<char>(std::cin), {}};
    ScopeConfiguration configuration{};
    std::string message;
    if (!parse(settings, configuration, &message)) {
        std::cout << message << "\n";
        return 1;
    }
    std::cout << runtime_json(configuration) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dump") == 0) {
        dump_json();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--configure") == 0) return configure_from_stdin();

    test_crc();
    test_encoder_matches_the_record_format();
    test_encoder_handles_cobs_extremes();
    test_encoder_refuses_what_does_not_fit();
    test_parser_round_trip();
    test_parser_rejects_damage_and_recovers();
    test_parser_survives_oversized_frames();
    test_parser_accepts_the_largest_settings_object();
    test_defaults();
    test_layouts();
    test_rejections();
    test_a_rejected_configuration_leaves_the_output_alone();
    test_adc_time_budget();
    test_usb_budget();
    test_every_accepted_block_fits_the_buffers();
    test_low_rates_still_send_blocks();
    test_every_rate_is_exact();
    test_settings_round_trip();
    test_defaults_match_the_descriptor_text();
    test_tiny_buffers_fail_cleanly();
    test_largest_response_fits_control_messages();
    test_analog_scaling();
    test_analog_and_digital_rows();
    test_digital_only_rows();
    std::puts("firmware host tests passed");
    return 0;
}
