#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

// A P-tap polyphase FFT uses P*N input samples instead of the N samples used
// by an unfiltered FFT. The linear-phase prototype therefore moves the
// effective spectrum time backwards by (P*N-N)/2 samples.
inline uint64_t pfb_group_delay_ns(size_t num_taps,
                                   size_t fft_size,
                                   uint64_t sampling_rate_hz)
{
    if (num_taps == 0 || fft_size == 0 || sampling_rate_hz == 0)
        throw std::invalid_argument("invalid PFB timing parameters");

    const long double delay_samples =
        static_cast<long double>(num_taps - 1) * fft_size / 2.0L;
    return static_cast<uint64_t>(std::llround(
        delay_samples * 1000000000.0L / sampling_rate_hz));
}

inline uint64_t compensate_delay_ns(uint64_t timestamp_ns,
                                    uint64_t delay_ns)
{
    return delay_ns > timestamp_ns ? 0 : timestamp_ns - delay_ns;
}
