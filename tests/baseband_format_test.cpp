#include "baseband_quantize.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

int main()
{
    std::array<uint8_t, 4> eight_bit{{0x00, 0x7f, 0x80, 0xff}};
    baseband_quantize::quantize8(eight_bit.data(), eight_bit.size());
    if (eight_bit != std::array<uint8_t, 4>{{0xff, 0x80, 0x7f, 0x00}})
        return 1;

    std::array<uint8_t, 4> four_bit{{0x00, 0x10, 0xa0, 0xf0}};
    baseband_quantize::quantize4(four_bit.data(), four_bit.size());
    if (four_bit[0] != 0x89 || four_bit[1] != 0x27)
        return 2;

    std::array<uint8_t, 4> two_bit{{0x00, 0x40, 0x80, 0xc0}};
    baseband_quantize::quantize2(two_bit.data(), two_bit.size());
    if (two_bit[0] != 0x1b)
        return 3;

    std::array<uint8_t, 128> scalar_eight{};
    std::array<uint8_t, 128> dispatched_eight{};
    for (std::size_t i = 0; i < scalar_eight.size(); ++i)
    {
        scalar_eight[i] = static_cast<uint8_t>(i * 29U);
        dispatched_eight[i] = scalar_eight[i];
    }
    baseband_quantize::quantize8_scalar(
        scalar_eight.data(), scalar_eight.size());
    baseband_quantize::quantize8(
        dispatched_eight.data(), dispatched_eight.size());
    if (scalar_eight != dispatched_eight)
        return 4;

    std::array<uint8_t, 128> scalar_input{};
    std::array<uint8_t, 128> dispatched_input{};
    for (std::size_t i = 0; i < scalar_input.size(); ++i)
    {
        scalar_input[i] = static_cast<uint8_t>(i * 37U);
        dispatched_input[i] = scalar_input[i];
    }
    baseband_quantize::quantize4_scalar(
        scalar_input.data(), scalar_input.size());
    baseband_quantize::quantize4(
        dispatched_input.data(), dispatched_input.size());
    for (std::size_t i = 0; i < scalar_input.size() / 2; ++i)
    {
        if (scalar_input[i] != dispatched_input[i])
            return 5;
    }

    return 0;
}
