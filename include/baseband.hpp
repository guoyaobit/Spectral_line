#pragma once
#include <cstdint>
#include <algorithm>
#include <vector>
#include <memory>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <fcntl.h>  // open, O_WRONLY, O_CREAT, O_TRUNC
#include <unistd.h> // write, close, read
#include <sys/stat.h>
#include <sys/types.h>
#include <baseband_quantize.hpp>
#include <Globalcfg.hpp>
#include <sys/uio.h>

class baseband
{
private:
    int m_stream_id;
    int m_subband_id;
    moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *> *m_queue;
    int fd = -1;
    static constexpr uint64_t DEFAULT_MAX_FILE_SIZE = 8ULL * 1024 * 1024 * 1024; // 8 GiB
    uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE;
    uint64_t current_size = 0;
    uint32_t file_index = 0;
    std::string m_folder;

    static constexpr size_t FRAME_PAYLOAD = 8192;
    static constexpr size_t HEADER_SIZE = 32;
    std::vector<iovec> iov;
    size_t iov_max = 1024;

    // create_file: returns 0 on success, -1 on error
    int create_file()
    {
        std::filesystem::path folder(m_folder);
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        auto &cfg = GlobalConfig::getInstance();
        if (ec)
        {
            cfg.logger_->error("baseband: create_directories({}) failed: {}", folder.string(), ec.message());
            return -1;
        }
        char basename[256];

        const unsigned int current_file_index = file_index;
        const char *pol = (m_stream_id % 2 == 0) ? "X" : "Y";

        const time_t now = static_cast<time_t>(std::time(nullptr));

        struct tm utc_tm {};
        gmtime_r(&now, &utc_tm);

        char utc_date[16];
        char utc_time[16];

        std::strftime(utc_date, sizeof(utc_date),
                    "%Y-%m-%d", &utc_tm);

        std::strftime(utc_time, sizeof(utc_time),
                    "%H:%M:%S", &utc_tm);

        const int filename_length = std::snprintf(
                basename,
                sizeof(basename),
                "%s_%s_%s_b%02u_%s_t%04u.vdif",
                cfg.receiver_name.c_str(),
                utc_date,
                utc_time,
                static_cast<unsigned int>(m_subband_id+cfg.ServerID*8),
                pol,
                current_file_index);
        if (filename_length < 0 ||
            static_cast<size_t>(filename_length) >= sizeof(basename))
        {
            cfg.logger_->error(
                "baseband: output filename generation failed for {} polarization",
                pol);
            return -1;
        }

        std::filesystem::path filepath = folder / basename;
        const int new_fd =
            ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (new_fd < 0)
        {
            cfg.logger_->error("baseband: open('{}') failed: {} (errno={})", filepath.string(), std::strerror(errno), errno);
            return -1;
        }

        if (fd >= 0)
            ::close(fd);
        fd = new_fd;
        current_size = 0;
        ++file_index;
        return 0;
    }

    bool write_all_iov(size_t iov_count)
    {
        auto &cfg = GlobalConfig::getInstance();
        size_t first = 0;

        while (first < iov_count)
        {
            const size_t count = std::min(iov_max, iov_count - first);
            const ssize_t written = ::writev(
                fd, iov.data() + first, static_cast<int>(count));
            if (written < 0)
            {
                if (errno == EINTR)
                    continue;
                cfg.logger_->error(
                    "baseband: writev failed: {}", std::strerror(errno));
                return false;
            }
            if (written == 0)
            {
                cfg.logger_->error("baseband: writev made no progress");
                return false;
            }

            current_size += static_cast<uint64_t>(written);
            size_t remaining = static_cast<size_t>(written);
            while (remaining != 0 && first < iov_count)
            {
                if (remaining >= iov[first].iov_len)
                {
                    remaining -= iov[first].iov_len;
                    ++first;
                }
                else
                {
                    iov[first].iov_base =
                        static_cast<uint8_t *>(iov[first].iov_base) + remaining;
                    iov[first].iov_len -= remaining;
                    remaining = 0;
                }
            }
        }
        return true;
    }

public:
    baseband(int);
    ~baseband();
    void recoder()
    {
        auto &cfg = GlobalConfig::getInstance();
        PacketBatch *readblock = nullptr;
        m_queue->wait_dequeue(readblock);
        const int frames_to_write = readblock->count;
        if (frames_to_write < 0)
        {
            cfg.logger_->error("baseband: invalid negative frame count {}", frames_to_write);
            return;
        }
        if (cfg.Baseband_bits != 8 && cfg.Baseband_bits != 4 &&
            cfg.Baseband_bits != 2)
        {
            cfg.logger_->error(
                "baseband: unsupported Baseband_bits {}", cfg.Baseband_bits);
            return;
        }
        if (static_cast<size_t>(frames_to_write) > readblock->hdrs.size() ||
            static_cast<size_t>(frames_to_write) > readblock->pkts.size())
        {
            cfg.logger_->error(
                "baseband: frame count {} exceeds batch storage", frames_to_write);
            return;
        }
        const size_t payload_bytes =
            FRAME_PAYLOAD * static_cast<size_t>(cfg.Baseband_bits) / 8;
        const uint64_t batch_bytes = static_cast<uint64_t>(frames_to_write) *
                                     (HEADER_SIZE + payload_bytes);
        if (current_size != 0 && current_size + batch_bytes > max_file_size)
        {
            if (create_file() != 0)
            {
                cfg.logger_->error(
                    "baseband: create_file failed during rotation; "
                    "continuing with the current file");
            }
        }
        iov.resize(static_cast<size_t>(frames_to_write) * 2);
        if(cfg.Baseband_bits == 8)
        {
            for (int i = 0; i < frames_to_write; ++i)
            {
                baseband_quantize::quantize8(
                    readblock->pkts[i]->payload, FRAME_PAYLOAD);

                iov[2 * i].iov_base = readblock->hdrs[i].headerPtr();
                iov[2 * i].iov_len  = HEADER_SIZE;
                iov[2 * i + 1].iov_base = readblock->pkts[i]->payload;
                iov[2 * i + 1].iov_len  = FRAME_PAYLOAD;
            }
        }
        else if(cfg.Baseband_bits == 4)
        {
            for (int i = 0; i < frames_to_write; ++i)
            {
                baseband_quantize::quantize4(
                    readblock->pkts[i]->payload, FRAME_PAYLOAD);

                iov[2 * i].iov_base = readblock->hdrs[i].headerPtr();
                iov[2 * i].iov_len  = HEADER_SIZE;

                iov[2 * i + 1].iov_base = readblock->pkts[i]->payload;
                iov[2 * i + 1].iov_len  = FRAME_PAYLOAD/2;
            }
        }
        else if(cfg.Baseband_bits == 2)
        {
            for (int i = 0; i < frames_to_write; ++i)
            {
                baseband_quantize::quantize2(
                    readblock->pkts[i]->payload, FRAME_PAYLOAD);

                iov[2 * i].iov_base = readblock->hdrs[i].headerPtr();
                iov[2 * i].iov_len  = HEADER_SIZE;

                iov[2 * i + 1].iov_base = readblock->pkts[i]->payload;
                iov[2 * i + 1].iov_len  = FRAME_PAYLOAD/4;
            }
        }
        else
        {
            cfg.logger_->error("baseband: unsupported Baseband_bits {}", cfg.Baseband_bits);
            return;
        }
        write_all_iov(static_cast<size_t>(frames_to_write) * 2);
    }
};

baseband::baseband(int stream_id): m_stream_id(stream_id)
{
    auto &cfg = GlobalConfig::getInstance();
    m_queue = &cfg.streams[m_stream_id].queue;
    m_subband_id = m_stream_id/2;

    if (m_subband_id < 4)
        m_folder = cfg.Baseband_folder0;
    else
        m_folder = cfg.Baseband_folder1;
    const long system_iov_max = ::sysconf(_SC_IOV_MAX);
    if (system_iov_max > 0)
        iov_max = static_cast<size_t>(system_iov_max);
    iov.resize(static_cast<size_t>(cfg.batchsize()) * 2);
    if (create_file() != 0)
    {
        throw std::runtime_error(
            "baseband: initial output file creation failed for subband " +
            std::to_string(m_subband_id));
    }
}

baseband::~baseband()
{
    if (fd >= 0)close(fd);
}
