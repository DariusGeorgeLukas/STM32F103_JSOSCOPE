#include "device_descriptor.hpp"
#include "sampler.hpp"
#include "scope_config.hpp"
#include "usb_bridge.h"

#include "jsoscope/device_session.hpp"
#include "jsoscope/instrument.hpp"

#include "main.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// The USB callback is an interrupt-side producer and the main loop is the only
// consumer. Power-of-two sizing makes both indices wrap without division.
constexpr std::uint32_t rx_size = 512;
static_assert((rx_size & (rx_size - 1u)) == 0, "indices wrap by masking");

class UsbCdcTransport final : public jsoscope::ByteTransport {
public:
    void receive(const std::uint8_t* data, std::uint32_t length) {
        auto head = rx_head_;
        for (std::uint32_t index = 0; index < length; ++index) {
            if (head - rx_tail_ == rx_size) break;
            rx_ring_[head & (rx_size - 1u)] = data[index];
            ++head;
        }
        rx_head_ = head;
    }

    std::size_t read(std::uint8_t* data, std::size_t capacity) override {
        std::size_t count = 0;
        while (count < capacity && rx_tail_ != rx_head_) {
            data[count++] = rx_ring_[rx_tail_ & (rx_size - 1u)];
            rx_tail_ = rx_tail_ + 1u;
        }
        return count;
    }

    std::size_t write(const std::uint8_t* data, std::size_t length) override {
        if (length == 0 || CDC_Transmit_Idle_FS() == 0) return 0;
        const auto count = length < tx_record_.size() ? length : tx_record_.size();
        // The ST USB stack retains the pointer while it splits the record into
        // endpoint packets. Own the complete transfer so DeviceSession may
        // reuse its buffer as soon as write() reports the bytes accepted.
        std::memcpy(tx_record_.data(), data, count);
        return CDC_Transmit_FS(tx_record_.data(), static_cast<std::uint16_t>(count)) == 0
                   ? count
                   : 0;
    }

private:
    std::array<std::uint8_t, rx_size> rx_ring_{};
    volatile std::uint32_t rx_head_{};
    volatile std::uint32_t rx_tail_{};
    std::array<std::uint8_t, protocol::max_control_wire_bytes> tx_record_{};
};

class ScopeInstrument final : public jsoscope::Instrument {
public:
    bool init(const char* serial) {
        std::memcpy(serial_.data(), serial, serial_.size());
        std::array<char, 96> error{};
        if (!parse_scope_configuration(
                reinterpret_cast<const std::uint8_t*>(device_descriptor::default_settings),
                sizeof(device_descriptor::default_settings) - 1, configuration_, error.data(),
                error.size())) {
            return false;
        }
        sampler::init();
        sampler::start(configuration_);
        return true;
    }

    std::size_t descriptor_length() const override { return device_descriptor::length; }

    void read_descriptor(std::size_t offset, std::uint8_t* output,
                         std::size_t length) const override {
        const jsoscope::TextPart parts[] = {
            {device_descriptor::before_serial, sizeof(device_descriptor::before_serial) - 1},
            {serial_.data(), serial_.size()},
            {device_descriptor::after_serial, sizeof(device_descriptor::after_serial) - 1},
        };
        jsoscope::copy_text_parts(parts, std::size(parts), offset, output, length);
    }

    std::uint32_t generation() const override { return generation_; }

    std::size_t runtime_config(char* output, std::size_t capacity) override {
        return build_runtime_config(configuration_, output, capacity);
    }

    std::uint8_t configure(const std::uint8_t* settings, std::size_t settings_length,
                           char* output, std::size_t capacity,
                           std::size_t& output_length) override {
        ScopeConfiguration candidate{};
        if (!parse_scope_configuration(settings, settings_length, candidate, output, capacity)) {
            output_length = bounded_length(output, capacity);
            return 1;
        }

        // Build first, so a response that does not fit cannot leave the board
        // running a configuration the host was told had failed.
        output_length = build_runtime_config(candidate, output, capacity);
        if (output_length == 0) {
            return fail("configuration response is too large", output, capacity, output_length,
                        2);
        }

        sampler::stop();
        configuration_ = candidate;
        ++generation_;
        sampler::start(configuration_);
        return 0;
    }

    jsoscope::SampleBlock take_block() override {
        SampleBlock block;
        if (!sampler::take_block(block)) return {};
        const auto size = pack_sample_rows(configuration_, block.adc, block.gpio,
                                           sample_rows_.data());
        samples_since_led_ = true;
        return {sample_rows_.data(), size};
    }

    bool take_led_activity() {
        const bool active = samples_since_led_;
        samples_since_led_ = false;
        return active;
    }

private:
    static std::size_t bounded_length(const char* text, std::size_t capacity) {
        std::size_t length = 0;
        while (length < capacity && text[length] != '\0') ++length;
        return length;
    }

    static std::uint8_t fail(const char* message, char* output, std::size_t capacity,
                             std::size_t& output_length, std::uint8_t status) {
        output_length = std::strlen(message);
        if (output_length > capacity) output_length = capacity;
        std::memcpy(output, message, output_length);
        return status;
    }

    ScopeConfiguration configuration_{};
    std::uint32_t generation_{};
    std::array<char, device_descriptor::serial_length> serial_{};
    std::array<std::uint8_t, protocol::max_sample_payload_bytes> sample_rows_{};
    bool samples_since_led_{};
};

UsbCdcTransport usb;
ScopeInstrument instrument;
jsoscope::DeviceSession session{instrument, usb};
std::uint32_t last_led_toggle_ms;
bool led_activity;

char hex_digit(std::uint32_t value) {
    value &= 0xFu;
    return static_cast<char>(value < 10 ? '0' + value : 'A' + value - 10);
}

void append_hex(char*& output, std::uint32_t value, unsigned digits) {
    for (unsigned index = 0; index < digits; ++index) {
        const unsigned shift = 4u * (digits - index - 1u);
        *output++ = hex_digit(value >> shift);
    }
}

// Match the 12-character serial generated by Cube's USB descriptor: eight hex
// digits from UID word 0 + word 2, followed by four from UID word 1.
void read_usb_serial(char* output) {
    const auto* uid = reinterpret_cast<const volatile std::uint32_t*>(UID_BASE);
    append_hex(output, uid[0] + uid[2], 8);
    append_hex(output, uid[1] >> 16, 4);
}

void service_led() {
    led_activity = instrument.take_led_activity() || led_activity;
    constexpr std::uint32_t period_ms = 250;
    const auto now = HAL_GetTick();
    if (!led_activity || now - last_led_toggle_ms < period_ms) return;
    last_led_toggle_ms = now;
    led_activity = false;
    GPIOC->ODR = GPIOC->ODR ^ GPIO_ODR_ODR13;
}

}  // namespace

extern "C" void usb_bridge_receive(const std::uint8_t* data, std::uint32_t length) {
    usb.receive(data, length);
}

extern "C" void app_setup(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef led{};
    led.Pin = GPIO_PIN_13;
    led.Mode = GPIO_MODE_OUTPUT_PP;
    led.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &led);

    std::array<char, device_descriptor::serial_length> serial{};
    read_usb_serial(serial.data());
    if (!instrument.init(serial.data())) Error_Handler();
}

extern "C" void app_loop(void) {
    session.service();
    service_led();
}
