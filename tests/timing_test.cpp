#include "timing.hpp"
#include "vdif.hpp"
#include "SpectrumTransport.hpp"

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

    VDIF header;
    constexpr uint64_t packet_id =
        1234ULL * VDIF::FRAMES_PER_SECOND + 5678ULL;
    header.setPacketId(packet_id);
    header.setWord(7, 1U);

    const VDIF::Metadata metadata =
        VDIF::readMetadata(header.headerPtr(), true);
    if (metadata.seconds_from_epoch != 1234U ||
        metadata.frame_number != 5678U ||
        metadata.noise_source_state != 1U ||
        metadata.packet_id() != packet_id)
        return 9;

    if (VDIF::readMetadata(header.headerPtr(), false)
            .noise_source_state != 0U)
        return 10;

    header.configureBaseband(8, 17);
    if (header.getEDV() != 1U ||
        header.getFrameLength() !=
            VDIF::HEADER_SIZE + VDIF::DEFAULT_PAYLOAD_BYTES ||
        !header.getComplex() ||
        header.getBitsPerSample() != 8U ||
        header.getThreadID() != 17U)
        return 11;

    header.configureBaseband(4, 18);
    if (header.getFrameLength() !=
            VDIF::HEADER_SIZE + VDIF::DEFAULT_PAYLOAD_BYTES / 2 ||
        header.getBitsPerSample() != 4U ||
        header.getThreadID() != 18U)
        return 12;

    header.configureBaseband(2, 19);
    if (header.getFrameLength() !=
            VDIF::HEADER_SIZE + VDIF::DEFAULT_PAYLOAD_BYTES / 4 ||
        header.getBitsPerSample() != 2U ||
        header.getThreadID() != 19U)
        return 13;

    static constexpr char crc_input[] = "123456789";
    if (spectrum_crc32c(crc_input, sizeof(crc_input) - 1) != 0xe3069283U)
        return 14;

    return 0;
}
