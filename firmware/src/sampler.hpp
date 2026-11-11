#pragma once

#include "scope_config.hpp"

#include <cstdint>

// One half of the DMA buffers, ready for pack_sample_rows().
struct SampleBlock {
    const std::uint16_t* adc{};
    const std::uint16_t* gpio{};
    // Blocks overwritten before they could be taken since the previous block.
    std::uint32_t lost{};
};

// Acquires rows in hardware. TIM3 paces both the ADC scan and a DMA read of
// GPIOB->IDR, so every row's analog and digital values come from one timer tick
// and no CPU time is spent per sample. See docs/architecture.md.
namespace sampler {

void init();
void start(const ScopeConfiguration& configuration);
void stop();

// Returns false when no new block has completed.
bool take_block(SampleBlock& block);

}  // namespace sampler
