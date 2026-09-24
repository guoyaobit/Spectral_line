#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace baseband_quantize {

inline void quantize8_scalar(uint8_t *buf, size_t bytes)
{
    size_t i = 0;
    for (; i + 1 < bytes; i += 2)
    {
        const uint8_t q = buf[i];
        const uint8_t in_phase = buf[i + 1];
        buf[i] = in_phase ^ 0x80;
        buf[i + 1] = q ^ 0x80;
    }
    if (i < bytes)
        buf[i] ^= 0x80;
}

inline void quantize4_scalar(uint8_t *buf, size_t bytes)
{
    size_t in = 0;
    size_t out = 0;
    while (in + 1 < bytes)
    {
        // FPGA input is Q,I. VDIF stores I in the low nibble and Q in the
        // high nibble. Flip the offset-binary sign bits at the same time.
        buf[out++] = static_cast<uint8_t>(
            ((buf[in + 1] ^ 0x80) >> 4) |
            ((buf[in] ^ 0x80) & 0xf0));
        in += 2;
    }
}

#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
__attribute__((target("avx512f,avx512bw")))
inline void quantize8_avx512(uint8_t *buf, size_t bytes)
{
    size_t i = 0;
    const __m512i sign_bits = _mm512_set1_epi8(static_cast<char>(0x80));
    while (i + 64 <= bytes)
    {
        const __m512i samples =
            _mm512_loadu_si512(reinterpret_cast<const void *>(buf + i));
        const __m512i iq = _mm512_or_si512(
            _mm512_slli_epi16(samples, 8),
            _mm512_srli_epi16(samples, 8));
        _mm512_storeu_si512(
            reinterpret_cast<void *>(buf + i),
            _mm512_xor_si512(iq, sign_bits));
        i += 64;
    }
    quantize8_scalar(buf + i, bytes - i);
}

__attribute__((target("avx512f,avx512bw")))
inline void quantize4_avx512(uint8_t *buf, size_t bytes)
{
    size_t in = 0;
    size_t out = 0;
    alignas(64) uint8_t nibbles[64];

    while (in + 64 <= bytes)
    {
        const __m512i samples = _mm512_xor_si512(
            _mm512_loadu_si512(reinterpret_cast<const void *>(buf + in)),
            _mm512_set1_epi8(static_cast<char>(0x80)));
        const __m512i shifted = _mm512_srai_epi16(samples, 4);
        const __m512i quantized = _mm512_and_si512(
            shifted, _mm512_set1_epi8(0x0f));
        _mm512_store_si512(reinterpret_cast<__m512i *>(nibbles), quantized);

        for (size_t i = 0; i < 64; i += 2)
        {
            nibbles[i / 2] = static_cast<uint8_t>(
                nibbles[i + 1] | (nibbles[i] << 4));
        }
        std::memcpy(buf + out, nibbles, 32);
        in += 64;
        out += 32;
    }

    while (in + 1 < bytes)
    {
        buf[out++] = static_cast<uint8_t>(
            ((buf[in + 1] ^ 0x80) >> 4) |
            ((buf[in] ^ 0x80) & 0xf0));
        in += 2;
    }
}
#endif

inline void quantize8(uint8_t *buf, size_t bytes)
{
#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw"))
    {
        quantize8_avx512(buf, bytes);
        return;
    }
#endif
    quantize8_scalar(buf, bytes);
}

inline void quantize4(uint8_t *buf, size_t bytes)
{
#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw"))
    {
        quantize4_avx512(buf, bytes);
        return;
    }
#endif
    quantize4_scalar(buf, bytes);
}

inline void quantize2(uint8_t *buf, size_t bytes)
{
    size_t in = 0;
    size_t out = 0;
    while (in + 3 < bytes)
    {
        // FPGA input is Q0,I0,Q1,I1. VDIF packs I0,Q0,I1,Q1 from the
        // least-significant to the most-significant two-bit field.
        const uint8_t q0 = (buf[in] ^ 0x80) >> 6;
        const uint8_t i0 = (buf[in + 1] ^ 0x80) >> 6;
        const uint8_t q1 = (buf[in + 2] ^ 0x80) >> 6;
        const uint8_t i1 = (buf[in + 3] ^ 0x80) >> 6;
        buf[out++] = static_cast<uint8_t>(
            i0 | (q0 << 2) | (i1 << 4) | (q1 << 6));
        in += 4;
    }
}

} // namespace baseband_quantize
