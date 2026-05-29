/*
 * Copyright 2026-present Alibaba Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "paimon/global_index/tantivy/tantivy_archive_layout.h"

#include <chrono>  // [PROF_L25]
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "fmt/format.h"
#include "glog/logging.h"  // [PROF_L25]
#include "paimon/fs/file_system.h"

namespace paimon::tantivy {

namespace {

/// 1MB lookahead — covers ~25-50 entry headers in a typical tantivy archive
/// when payloads keep us within the buffer; refills when the cursor jumps
/// past a payload that exceeds the buffer tail.
constexpr std::size_t kLookaheadBytes = 1 << 20;

int32_t ParseBE32(const char* p) {
    return (static_cast<int32_t>(static_cast<uint8_t>(p[0])) << 24) |
           (static_cast<int32_t>(static_cast<uint8_t>(p[1])) << 16) |
           (static_cast<int32_t>(static_cast<uint8_t>(p[2])) << 8) |
           static_cast<int32_t>(static_cast<uint8_t>(p[3]));
}

int64_t ParseBE64(const char* p) {
    return (static_cast<int64_t>(static_cast<uint32_t>(ParseBE32(p))) << 32) |
           static_cast<int64_t>(static_cast<uint32_t>(ParseBE32(p + 4)));
}

}  // namespace

Result<ArchiveLayout> ParseArchiveHeader(InputStream* in) {
    if (in == nullptr) {
        return Status::Invalid("ParseArchiveHeader: null input stream");
    }
    const auto t_start = std::chrono::steady_clock::now();
    PAIMON_ASSIGN_OR_RAISE(uint64_t file_size, in->Length());

    std::vector<char> buf(kLookaheadBytes);
    int64_t buf_off = 0;
    std::size_t buf_size = 0;
    int64_t cursor = 0;
    uint32_t io_count = 0;
    uint64_t io_bytes = 0;

    auto refill = [&](int64_t pos, std::size_t need) -> Status {
        if (static_cast<uint64_t>(pos) >= file_size) {
            return Status::Invalid(fmt::format(
                "ParseArchiveHeader: pos {} past file end {}", pos, file_size));
        }
        // Cap by remaining file bytes — paimon InputStreams reject short reads,
        // so requesting a 1MB tail of a 6KB file would IOError.
        const uint32_t want = static_cast<uint32_t>(
            std::min<uint64_t>(kLookaheadBytes, file_size - static_cast<uint64_t>(pos)));
        PAIMON_ASSIGN_OR_RAISE(int32_t got, in->Read(buf.data(), want, pos));
        buf_off = pos;
        buf_size = static_cast<std::size_t>(got);
        ++io_count;
        io_bytes += buf_size;
        if (buf_size < need) {
            return Status::Invalid(fmt::format(
                "ParseArchiveHeader: short pread at offset {} (got {} need {})",
                pos, buf_size, need));
        }
        return Status::OK();
    };

    auto read_at = [&](void* dest, std::size_t n) -> Status {
        const int64_t end = cursor + static_cast<int64_t>(n);
        if (cursor < buf_off || end > buf_off + static_cast<int64_t>(buf_size)) {
            PAIMON_RETURN_NOT_OK(refill(cursor, n));
        }
        std::memcpy(dest, buf.data() + (cursor - buf_off), n);
        cursor += static_cast<int64_t>(n);
        return Status::OK();
    };

    // Initial 1MB pread covers file_count + first ~25-50 entry headers in
    // most archives, eliminating the per-entry seek round-trip cost.
    PAIMON_RETURN_NOT_OK(refill(0, 4));

    char fc_buf[4];
    PAIMON_RETURN_NOT_OK(read_at(fc_buf, 4));
    int32_t file_count = ParseBE32(fc_buf);
    if (file_count < 0) {
        return Status::Invalid(
            fmt::format("ParseArchiveHeader: negative file_count {}", file_count));
    }

    ArchiveLayout layout;
    layout.count = static_cast<std::size_t>(file_count);
    layout.names.reserve(layout.count);
    layout.offsets.reserve(layout.count);
    layout.lengths.reserve(layout.count);

    for (int32_t i = 0; i < file_count; ++i) {
        char nl_buf[4];
        PAIMON_RETURN_NOT_OK(read_at(nl_buf, 4));
        int32_t name_len = ParseBE32(nl_buf);
        if (name_len <= 0 || name_len > (1 << 20)) {
            return Status::Invalid(fmt::format(
                "ParseArchiveHeader: bad name_len {} at entry {}", name_len, i));
        }
        std::string name(static_cast<std::size_t>(name_len), '\0');
        PAIMON_RETURN_NOT_OK(read_at(name.data(), static_cast<std::size_t>(name_len)));

        char dl_buf[8];
        PAIMON_RETURN_NOT_OK(read_at(dl_buf, 8));
        int64_t data_len = ParseBE64(dl_buf);
        if (data_len < 0) {
            return Status::Invalid(fmt::format(
                "ParseArchiveHeader: negative data_len {} for '{}'", data_len, name));
        }

        const int64_t data_offset = cursor;
        layout.names.push_back(std::move(name));
        layout.offsets.push_back(static_cast<uint64_t>(data_offset));
        layout.lengths.push_back(static_cast<uint64_t>(data_len));

        // Skip past payload by advancing cursor only; refill happens lazily
        // on the next read if next-entry header falls outside the buffer.
        cursor = data_offset + data_len;
    }

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t_start)
                          .count();
    LOG(INFO) << "[PROF_L25] parse_header file_count=" << file_count
              << " io_count=" << io_count
              << " io_bytes=" << io_bytes
              << " total_ms=" << ms;

    return layout;
}

}  // namespace paimon::tantivy
