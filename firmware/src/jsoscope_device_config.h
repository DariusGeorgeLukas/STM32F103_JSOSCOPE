#pragma once

// Buffer limits for the JSOScope device library on the 20 KB STM32F103.

// Largest packed sample block produced by the DMA sampler.
#define JSOSCOPE_MAX_SAMPLE_PAYLOAD_BYTES 512

// Runtime configuration JSON with every input enabled, including status bytes.
// 2000 keeps a worst-case encoded record below the companion's 2048-byte
// serial-frame limit while fitting all eighteen enabled input descriptions.
#define JSOSCOPE_MAX_CONTROL_BODY_BYTES 2000

// The host sends only a flat settings object, so the receive side can be
// smaller than the descriptor and runtime-configuration responses.
#define JSOSCOPE_MAX_REQUEST_BODY_BYTES 768
