#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

inline double fft_integration_seconds(int blocks, int fft_size,
                                      int sampling_rate_hz)
{
    return static_cast<double>(blocks) * fft_size / sampling_rate_hz;
}

inline int integration_fft_count(double seconds, int fft_size,
                                 int sampling_rate_hz)
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        throw std::runtime_error("integration_t must be finite and > 0");
    if (fft_size <= 0 || sampling_rate_hz <= 0)
        throw std::runtime_error("invalid FFT timing parameters");

    constexpr int max_blocks = std::numeric_limits<int>::max();
    const auto boundary = [fft_size, sampling_rate_hz](int blocks) {
        return fft_integration_seconds(blocks, fft_size, sampling_rate_hz);
    };
    if (seconds > boundary(max_blocks))
        throw std::runtime_error("integration_t exceeds the supported FFT count");

    // Estimate the block count, then check the actual whole-block boundaries.
    // Compute each boundary from the sample count with a single division.
    int blocks = static_cast<int>(std::min(
        seconds * sampling_rate_hz / fft_size, static_cast<double>(max_blocks)));
    if (blocks < max_blocks && seconds >= boundary(blocks + 1))
        ++blocks;
    else if (blocks > 1 && seconds < boundary(blocks))
        --blocks;
    return std::max(1, blocks);
}

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
