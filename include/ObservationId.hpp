#pragma once

#include <SpectrumTransport.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

static constexpr size_t OBSERVATION_ID_MAX_LENGTH = 64;

inline bool observation_id_is_valid(const std::string &id)
{
    if (id.empty() || id.size() > OBSERVATION_ID_MAX_LENGTH)
        return false;

    for (const unsigned char c : id)
    {
        const bool ascii_alnum =
            (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z');
        if (!ascii_alnum && c != '-' && c != '_' && c != '.')
            return false;
    }
    return id != "." && id != "..";
}

inline uint32_t observation_id_numeric(const std::string &id)
{
    return spectrum_crc32c(id.data(), id.size());
}
