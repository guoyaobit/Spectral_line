#include "timing.hpp"
#include "vdif.hpp"

int main()
{
    if (pfb_group_delay_ns(4, 65536, 256000000) != 384000ULL)
        return 1;

    // Epoch 16 spans the positive leap second at the end of 2008.
    constexpr uint64_t start_2009_ns = 1230768000000000000ULL;
    if (vdif_to_timestamp_ns(16, 31622401U, 0) != start_2009_ns)
        return 2;

    // POSIX cannot name 23:59:60; it is folded onto 23:59:59.
    constexpr uint64_t leap_2008_folded_ns = 1230767999000000000ULL;
    if (vdif_to_timestamp_ns(16, 31622400U, 0) !=
        leap_2008_folded_ns)
        return 3;

    // A reference epoch after the leap starts directly at Unix midnight.
    if (vdif_to_timestamp_ns(18, 0, 0) != start_2009_ns)
        return 4;

    // Preserve the 16-us VDIF frame phase after leap-second conversion.
    if (vdif_to_timestamp_ns(16, 31622401U, 7) !=
        start_2009_ns + 112000ULL)
        return 5;

    // The most recent leap second (2016-12-31).
    constexpr uint64_t start_2017_ns = 1483228800000000000ULL;
    if (vdif_to_timestamp_ns(32, 31622401U, 0) != start_2017_ns)
        return 6;

    // A clock held at epoch 0 accumulates all five leap seconds since 2000.
    if (vdif_to_timestamp_ns(0, 536544005U, 0) != start_2017_ns)
        return 7;

    if (vdif_to_timestamp_ns(34, 0, 0) != start_2017_ns)
        return 8;

    return 0;
}
