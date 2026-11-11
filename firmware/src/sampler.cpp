#include "sampler.hpp"

#include "jsoscope/protocol.hpp"
#include "stm32f1xx_hal.h"

#include <array>

namespace {

// Each buffer holds two blocks. The DMA half-transfer and transfer-complete
// interrupts mark each block as it finishes, and the main loop reads that half
// while the DMA fills the other one.
//
// A row takes two bytes per analog input, and a digital row takes four, so a
// block of max_sample_payload_bytes holds these many values at most.
constexpr std::size_t max_adc_values = protocol::max_sample_payload_bytes / sizeof(std::uint16_t);
constexpr std::size_t max_gpio_values = protocol::max_sample_payload_bytes / sizeof(std::uint32_t);

alignas(4) std::array<std::uint16_t, 2 * max_adc_values> adc_buffer;
alignas(4) std::array<std::uint16_t, 2 * max_gpio_values> gpio_buffer;

auto* const adc_dma = DMA1_Channel1;   // ADC1
auto* const gpio_dma = DMA1_Channel3;  // TIM3_UP

constexpr std::uint32_t timer_clock_hz = 72'000'000;
constexpr std::uint32_t adc_cr2_trigger_tim3_trgo = 4u << ADC_CR2_EXTSEL_Pos;

std::size_t block_adc_values;
std::size_t block_gpio_values;

volatile std::uint32_t completed_halves;
std::uint32_t taken_halves;

void configure_digital_pins(const ScopeConfiguration& configuration) {
    constexpr std::uint32_t input_floating = 0b0100;
    constexpr std::uint32_t input_pull = 0b1000;
    for (std::size_t index = 0; index < digital_input_count; ++index) {
        const std::uint32_t pin = digital_pins[index];
        static_assert(digital_pins[0] >= 8, "digital pins use GPIOB->CRH");
        const auto mode = configuration.digital[index];
        const auto shift = (pin - 8u) * 4u;
        const auto cnf_mode = mode == DigitalMode::pull_up || mode == DigitalMode::pull_down
                                  ? input_pull
                                  : input_floating;
        GPIOB->CRH = (GPIOB->CRH & ~(0xFu << shift)) | (cnf_mode << shift);
        // In input-pull mode, the output register selects up or down.
        GPIOB->BSRR = mode == DigitalMode::pull_up ? (1u << pin) : (1u << (pin + 16u));
    }
}

// Calibration leaves its result in ADC_DR, so it has to finish before the DMA
// request is enabled, or that value would be stored as the first sample.
void calibrate_adc() {
    ADC1->CR2 = ADC_CR2_ADON;
    HAL_Delay(1);  // tSTAB is 1 us, and calibration needs two ADC clocks after it.
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_RSTCAL;
    while ((ADC1->CR2 & ADC_CR2_RSTCAL) != 0) {
    }
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_CAL;
    while ((ADC1->CR2 & ADC_CR2_CAL) != 0) {
    }
    (void)ADC1->DR;
}

void configure_adc(const ScopeConfiguration& configuration) {
    std::uint32_t smpr2 = 0;
    std::uint32_t sqr2 = 0;
    std::uint32_t sqr3 = 0;
    std::uint32_t rank = 0;
    for (std::size_t index = 0; index < analog_input_count; ++index) {
        const std::uint32_t channel = analog_adc_channels[index];
        static_assert(analog_adc_channels.back() < 10, "analog inputs use ADC_SMPR2");
        static_assert(analog_input_count <= 12, "the scan order fits in ADC_SQR3 and ADC_SQR2");
        smpr2 |= static_cast<std::uint32_t>(configuration.sample_time_index) << (channel * 3u);
        if (!configuration.analog[index]) continue;
        if (rank < 6) {
            sqr3 |= channel << (rank * 5u);
        } else {
            sqr2 |= channel << ((rank - 6u) * 5u);
        }
        ++rank;
    }
    ADC1->SMPR2 = smpr2;
    ADC1->SQR3 = sqr3;
    ADC1->SQR2 = sqr2;
    ADC1->SQR1 = (rank - 1u) << ADC_SQR1_L_Pos;
    ADC1->CR1 = ADC_CR1_SCAN;
    // ADON is already set. Changing other bits in the same write stops this
    // from starting a conversion. EXTTRIG waits for TIM3.
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_ALIGN | ADC_CR2_DMA | ADC_CR2_EXTTRIG |
                adc_cr2_trigger_tim3_trgo;
}

// Only one channel needs to interrupt. With analog inputs that is the ADC,
// because a row's conversions finish after its GPIO read has already happened.
void arm_dma(DMA_Channel_TypeDef* channel, volatile std::uint32_t* peripheral,
             std::uint16_t* memory, std::size_t values, std::uint32_t peripheral_size,
             std::uint32_t priority, bool interrupts) {
    channel->CCR = 0;
    channel->CPAR = reinterpret_cast<std::uint32_t>(peripheral);
    channel->CMAR = reinterpret_cast<std::uint32_t>(memory);
    channel->CNDTR = static_cast<std::uint32_t>(values);
    channel->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | peripheral_size | DMA_CCR_MSIZE_0 | priority |
                   (interrupts ? DMA_CCR_HTIE | DMA_CCR_TCIE : 0u) | DMA_CCR_EN;
}

void prepare_timer(const ScopeConfiguration& configuration, bool digital) {
    TIM3->CR1 = 0;
    TIM3->CR2 = 0;
    TIM3->DIER = 0;
    TIM3->PSC = timer_clock_hz / timer_tick_hz - 1u;
    TIM3->ARR = timer_tick_hz / configuration.sample_rate_hz - 1u;
    TIM3->CNT = 0;
    // Loads the prescaler. TRGO and the DMA request are still off, so this
    // event cannot start a row.
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR = 0;
    TIM3->CR2 = TIM_CR2_MMS_1;  // TRGO on update.
    if (digital) TIM3->DIER = TIM_DIER_UDE;
}

void block_finished(std::uint32_t status, std::uint32_t half_flag, std::uint32_t complete_flag,
                    std::uint32_t clear) {
    std::uint32_t halves = completed_halves;
    if ((status & half_flag) != 0) ++halves;
    if ((status & complete_flag) != 0) ++halves;
    completed_halves = halves;
    DMA1->IFCR = clear;
}

}  // namespace

extern "C" void DMA1_Channel1_IRQHandler(void) {
    block_finished(DMA1->ISR, DMA_ISR_HTIF1, DMA_ISR_TCIF1, DMA_IFCR_CGIF1);
}

extern "C" void DMA1_Channel3_IRQHandler(void) {
    block_finished(DMA1->ISR, DMA_ISR_HTIF3, DMA_ISR_TCIF3, DMA_IFCR_CGIF3);
}

namespace sampler {

void init() {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6);

    // All ADC input pins stay in analog mode whether or not they are sampled.
    GPIOA->CRL = 0;
    const auto port_b_low_config = GPIOB->CRL;
    GPIOB->CRL = port_b_low_config & ~0xFFu;  // PB0 and PB1.

    // Above USB, because these handlers do almost nothing and a late one can
    // cost a block.
    NVIC_SetPriority(DMA1_Channel1_IRQn, 1);
    NVIC_SetPriority(DMA1_Channel3_IRQn, 1);
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

void stop() {
    TIM3->CR1 = 0;
    TIM3->DIER = 0;
    adc_dma->CCR = 0;
    gpio_dma->CCR = 0;
    ADC1->CR2 = 0;
    DMA1->IFCR = DMA_IFCR_CGIF1 | DMA_IFCR_CGIF3;
    NVIC_ClearPendingIRQ(DMA1_Channel1_IRQn);
    NVIC_ClearPendingIRQ(DMA1_Channel3_IRQn);
    completed_halves = 0;
    taken_halves = 0;
}

void start(const ScopeConfiguration& configuration) {
    const bool analog = configuration.analog_count != 0;
    const bool digital = configuration.digital_count != 0;
    block_adc_values = analog ? configuration.rows_per_block * configuration.analog_count : 0u;
    block_gpio_values = digital ? configuration.rows_per_block : 0u;

    // Digital inputs settle before the first row, so it reads the new pull.
    configure_digital_pins(configuration);
    prepare_timer(configuration, digital);
    if (analog) {
        calibrate_adc();
        configure_adc(configuration);
        arm_dma(adc_dma, &ADC1->DR, adc_buffer.data(), 2 * block_adc_values, DMA_CCR_PSIZE_0,
                DMA_CCR_PL_1, true);
    }
    if (digital) {
        // GPIO registers allow only word access. The DMA reads a word and
        // stores its low half, which holds all of PB0 to PB15.
        arm_dma(gpio_dma, &GPIOB->IDR, gpio_buffer.data(), 2 * block_gpio_values,
                DMA_CCR_PSIZE_1, DMA_CCR_PL_0, !analog);
    }
    TIM3->CR1 = TIM_CR1_CEN;
}

bool take_block(SampleBlock& block) {
    const std::uint32_t completed = completed_halves;
    if (completed == taken_halves) return false;

    // The newest finished half is always the one the DMA is not writing, so
    // after an overrun that is the only half still safe to read.
    block.lost = completed - taken_halves - 1u;
    const std::size_t half = (completed - 1u) & 1u;
    taken_halves = completed;
    block.adc = adc_buffer.data() + half * block_adc_values;
    block.gpio = gpio_buffer.data() + half * block_gpio_values;
    return true;
}

}  // namespace sampler
