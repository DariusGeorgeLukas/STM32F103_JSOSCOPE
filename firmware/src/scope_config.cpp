#include "scope_config.hpp"

#include "jsoscope/protocol.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
using View = std::string_view;

constexpr std::array<View, 4> digital_mode_values{{
    "off", "floating", "pull-up", "pull-down",
}};

// ADC clocks allowed for the trigger to synchronize, so that a scan always
// finishes before the next trigger arrives.
constexpr std::uint32_t adc_trigger_margin_clocks = 2;

void set_error(char* output, std::size_t capacity, const char* message) {
    if (capacity == 0) return;
    std::snprintf(output, capacity, "%s", message);
}

// string_view::substr can throw, and linking the exception path costs far more
// flash than this project can spare.
View slice(View view, std::size_t begin, std::size_t end) {
    return View{view.data() + begin, end - begin};
}

// The firmware has no general JSON parser, because it only needs to find keys
// in a flat object of strings, whole numbers, and booleans. Every key is
// required, and a value that cannot be read is an error.
bool value_for(View json, View key, View& output) {
    std::array<char, 32> quoted{};
    if (key.size() + 3 > quoted.size()) return false;
    quoted[0] = '"';
    std::memcpy(quoted.data() + 1, key.data(), key.size());
    quoted[key.size() + 1] = '"';
    const View token{quoted.data(), key.size() + 2};
    auto position = json.find(token);
    if (position == View::npos) return false;
    position = json.find(':', position + token.size());
    if (position == View::npos) return false;
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }
    if (position == json.size()) return false;
    if (json[position] == '"') {
        const auto end = json.find('"', position + 1);
        if (end == View::npos) return false;
        output = slice(json, position, end + 1);
        return true;
    }
    auto end = position;
    while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
    while (end > position &&
           (json[end - 1] == ' ' || json[end - 1] == '\t' ||
            json[end - 1] == '\r' || json[end - 1] == '\n')) {
        --end;
    }
    output = slice(json, position, end);
    return !output.empty();
}

bool string_for(View json, View key, View& output) {
    View value;
    if (!value_for(json, key, value) || value.size() < 2 ||
        value.front() != '"' || value.back() != '"') {
        return false;
    }
    output = slice(value, 1, value.size() - 1);
    return true;
}

bool bool_for(View json, View key, bool& output) {
    View value;
    if (!value_for(json, key, value)) return false;
    if (value == "true") {
        output = true;
        return true;
    }
    if (value == "false") {
        output = false;
        return true;
    }
    return false;
}

// Whole numbers only, because parsing floats would pull in a large library routine.
bool whole_number_for(View json, View key, std::uint32_t& output) {
    View value;
    if (!value_for(json, key, value) || value.size() > 9) return false;
    std::uint32_t result = 0;
    for (const char digit : value) {
        if (digit < '0' || digit > '9') return false;
        result = result * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    output = result;
    return true;
}

// "PA0" becomes "analog_pa0".
void input_key(char* output, std::size_t capacity, const char* prefix, const char* name) {
    const auto used = static_cast<std::size_t>(std::snprintf(output, capacity, "%s", prefix));
    std::size_t index = 0;
    for (; name[index] != '\0' && used + index + 1 < capacity; ++index) {
        const char letter = name[index];
        output[used + index] = letter >= 'A' && letter <= 'Z'
                                   ? static_cast<char>(letter - 'A' + 'a')
                                   : letter;
    }
    output[used + index] = '\0';
}

std::uint32_t rows_per_block(std::uint32_t sample_rate_hz, std::size_t row_bytes) {
    const auto capacity = static_cast<std::uint32_t>(
        protocol::max_sample_payload_bytes / row_bytes);
    auto rows = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(sample_rate_hz) * target_block_ms / 1000u);
    if (rows > capacity) rows = capacity;
    return rows == 0 ? 1u : rows;
}

// A sequential writer that remembers whether anything failed to fit.
class Json {
public:
    Json(char* output, std::size_t capacity) : output_(output), capacity_(capacity) {}

    template <typename... Args>
    void print(const char* format, Args... args) {
        if (failed_) return;
        const auto written = std::snprintf(output_ + used_, capacity_ - used_, format, args...);
        if (written < 0 || static_cast<std::size_t>(written) >= capacity_ - used_) {
            failed_ = true;
            return;
        }
        used_ += static_cast<std::size_t>(written);
    }

    void text(const char* value) { print("%s", value); }

    char* cursor() { return output_ + used_; }
    std::size_t remaining() const { return capacity_ - used_; }
    void advance(std::size_t bytes) {
        if (bytes == 0) failed_ = true;
        used_ += bytes;
    }
    std::size_t result() const { return failed_ ? 0 : used_; }

private:
    char* output_;
    std::size_t capacity_;
    std::size_t used_{};
    bool failed_{};
};
}  // namespace

std::uint32_t wire_bytes_per_second(const ScopeConfiguration& configuration) {
    const auto block_bytes = configuration.row_bytes() * configuration.rows_per_block;
    const auto wire_bytes = protocol::wire_bytes_for_body(block_bytes);
    const auto bytes = static_cast<std::uint64_t>(wire_bytes) * configuration.sample_rate_hz;
    return static_cast<std::uint32_t>(
        (bytes + configuration.rows_per_block - 1) / configuration.rows_per_block);
}

bool parse_scope_configuration(const std::uint8_t* bytes, std::size_t length,
                               ScopeConfiguration& output,
                               char* error, std::size_t error_capacity) {
    if (bytes == nullptr || length == 0) {
        set_error(error, error_capacity, "configuration JSON is empty");
        return false;
    }
    const View json{reinterpret_cast<const char*>(bytes), length};

    std::uint32_t sample_rate = 0;
    std::uint32_t reference_mv = 0;
    View sample_time;
    if (!whole_number_for(json, "sample_rate", sample_rate) ||
        !whole_number_for(json, "reference_mv", reference_mv) ||
        !string_for(json, "adc_sample_time", sample_time)) {
        set_error(error, error_capacity, "configuration is missing a field or has the wrong type");
        return false;
    }

    ScopeConfiguration candidate{};
    bool known_rate = false;
    for (const auto rate : sample_rates_hz) known_rate = known_rate || rate == sample_rate;
    if (!known_rate) {
        set_error(error, error_capacity, "sample rate is unsupported");
        return false;
    }
    candidate.sample_rate_hz = sample_rate;

    if (reference_mv < min_reference_mv || reference_mv > max_reference_mv) {
        set_error(error, error_capacity, "analog reference must be from 2400 to 3600 mV");
        return false;
    }
    candidate.reference_mv = static_cast<std::uint16_t>(reference_mv);

    bool known_time = false;
    for (std::size_t index = 0; index < adc_sample_time_names.size(); ++index) {
        if (sample_time == adc_sample_time_names[index]) {
            candidate.sample_time_index = static_cast<std::uint8_t>(index);
            known_time = true;
        }
    }
    if (!known_time) {
        set_error(error, error_capacity, "ADC sample time is unsupported");
        return false;
    }

    std::array<char, 24> key{};
    for (std::size_t index = 0; index < analog_input_count; ++index) {
        input_key(key.data(), key.size(), "analog_", analog_input_names[index]);
        bool enabled = false;
        if (!bool_for(json, key.data(), enabled)) {
            set_error(error, error_capacity, "configuration is missing a field or has the wrong type");
            return false;
        }
        candidate.analog[index] = enabled;
        if (enabled) ++candidate.analog_count;
    }

    for (std::size_t index = 0; index < digital_input_count; ++index) {
        input_key(key.data(), key.size(), "digital_", digital_input_names[index]);
        View mode;
        if (!string_for(json, key.data(), mode)) {
            set_error(error, error_capacity, "configuration is missing a field or has the wrong type");
            return false;
        }
        int found = -1;
        for (std::size_t option = 0; option < digital_mode_values.size(); ++option) {
            if (mode == digital_mode_values[option]) found = static_cast<int>(option);
        }
        if (found < 0) {
            set_error(error, error_capacity, "digital input mode is unsupported");
            return false;
        }
        candidate.digital[index] = static_cast<DigitalMode>(found);
        if (candidate.digital[index] != DigitalMode::off) {
            ++candidate.digital_count;
            candidate.digital_mask = static_cast<std::uint16_t>(
                candidate.digital_mask | (1u << digital_pins[index]));
        }
    }

    if (candidate.analog_count == 0 && candidate.digital_count == 0) {
        set_error(error, error_capacity, "select at least one input");
        return false;
    }

    // Tenths of an ADC clock, compared with the clocks in one row period. A
    // trigger that arrives during a scan is lost, which would slip the analog
    // columns against the digital word.
    const auto scan_tenths =
        candidate.analog_count *
            (adc_sample_time_tenths[candidate.sample_time_index] + adc_conversion_tenths) +
        adc_trigger_margin_clocks * 10u;
    const auto row_tenths = adc_clock_hz / candidate.sample_rate_hz * 10u;
    if (candidate.analog_count != 0 && scan_tenths > row_tenths) {
        set_error(error, error_capacity,
                  "sample rate is too fast for this many analog inputs at this ADC sample time");
        return false;
    }

    candidate.rows_per_block = static_cast<std::uint16_t>(
        rows_per_block(candidate.sample_rate_hz, candidate.row_bytes()));
    if (wire_bytes_per_second(candidate) > max_bytes_per_second) {
        set_error(error, error_capacity,
                  "sample rate is too fast for USB with these inputs; lower it or disable inputs");
        return false;
    }

    output = candidate;
    return true;
}

std::size_t build_stream_config(const ScopeConfiguration& configuration,
                                char* output, std::size_t capacity) {
    Json json{output, capacity};
    json.print("{\"source_name\":\"STM32F103 Blue Pill\",\"sample_rate_hz\":%lu,\"channels\":[",
               static_cast<unsigned long>(configuration.sample_rate_hz));

    bool first = true;
    unsigned offset = 0;
    for (std::size_t index = 0; index < analog_input_count; ++index) {
        if (!configuration.analog[index]) continue;
        // JSOScope's analog display defaults an omitted unit to volts. Leaving
        // that redundant field out keeps the 18-channel response in one frame.
        json.print("%s{\"name\":\"%s\",\"kind\":\"analog\",\"type\":\"int16\",\"offset\":%u,"
                   "\"min\":0,\"max\":%u.%03u}",
                   first ? "" : ",", analog_input_names[index], offset,
                   configuration.reference_mv / 1000u, configuration.reference_mv % 1000u);
        offset += sizeof(std::int16_t);
        first = false;
    }
    for (std::size_t index = 0; index < digital_input_count; ++index) {
        if (configuration.digital[index] == DigitalMode::off) continue;
        json.print("%s{\"name\":\"%s\",\"kind\":\"digital\",\"type\":\"uint32\",\"offset\":%u,"
                   "\"bit\":%u}",
                   first ? "" : ",", digital_input_names[index], offset,
                   static_cast<unsigned>(digital_pins[index]));
        first = false;
    }

    json.print("],\"frame\":{\"row_stride_bytes\":%u,\"samples_per_frame\":%u}}",
               static_cast<unsigned>(configuration.row_bytes()),
               static_cast<unsigned>(configuration.rows_per_block));
    return json.result();
}

std::size_t build_settings_json(const ScopeConfiguration& configuration,
                                char* output, std::size_t capacity) {
    Json json{output, capacity};
    std::array<char, 24> key{};
    json.print("{\"sample_rate\":%lu", static_cast<unsigned long>(configuration.sample_rate_hz));
    for (std::size_t index = 0; index < analog_input_count; ++index) {
        input_key(key.data(), key.size(), "analog_", analog_input_names[index]);
        json.print(",\"%s\":%s", key.data(), configuration.analog[index] ? "true" : "false");
    }
    for (std::size_t index = 0; index < digital_input_count; ++index) {
        input_key(key.data(), key.size(), "digital_", digital_input_names[index]);
        const auto mode = static_cast<std::size_t>(configuration.digital[index]);
        json.print(",\"%s\":\"%s\"", key.data(), digital_mode_values[mode].data());
    }
    json.print(",\"adc_sample_time\":\"%s\",\"reference_mv\":%u}",
               adc_sample_time_names[configuration.sample_time_index],
               static_cast<unsigned>(configuration.reference_mv));
    return json.result();
}

std::size_t build_runtime_config(const ScopeConfiguration& configuration,
                                 char* output, std::size_t capacity) {
    Json json{output, capacity};
    json.text("{\"stream\":");
    json.advance(build_stream_config(configuration, json.cursor(), json.remaining()));
    json.text(",\"settings\":");
    json.advance(build_settings_json(configuration, json.cursor(), json.remaining()));
    json.text("}");
    return json.result();
}

std::size_t pack_sample_rows(const ScopeConfiguration& configuration,
                             const std::uint16_t* adc, const std::uint16_t* gpio,
                             std::uint8_t* output) {
    const std::size_t analog_count = configuration.analog_count;
    const bool has_digital = configuration.digital_count != 0;
    auto* write = output;
    for (std::size_t row = 0; row < configuration.rows_per_block; ++row) {
        for (std::size_t column = 0; column < analog_count; ++column) {
            // A left-aligned result is unsigned. Flipping the top bit gives an
            // int16 whose full range is 0 V to the reference, which is how the
            // UI reads min and max for this type.
            const auto sample = static_cast<std::uint16_t>(*adc++ ^ 0x8000u);
            *write++ = static_cast<std::uint8_t>(sample);
            *write++ = static_cast<std::uint8_t>(sample >> 8);
        }
        if (has_digital) {
            const auto word = static_cast<std::uint16_t>(gpio[row] & configuration.digital_mask);
            *write++ = static_cast<std::uint8_t>(word);
            *write++ = static_cast<std::uint8_t>(word >> 8);
            *write++ = 0;
            *write++ = 0;
        }
    }
    return static_cast<std::size_t>(write - output);
}
