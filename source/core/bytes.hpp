// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace tz
{

using Bytes = std::vector<uint8_t>;
using ByteSpan = std::span<const uint8_t>;
using MutableByteSpan = std::span<uint8_t>;

// Little endian reads and writes; every 3DS file format here is little endian.

inline uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t readU64(const uint8_t* p)
{
    return static_cast<uint64_t>(readU32(p)) | (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

inline void writeU16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void writeU32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void writeU64(uint8_t* p, uint64_t v)
{
    writeU32(p, static_cast<uint32_t>(v));
    writeU32(p + 4, static_cast<uint32_t>(v >> 32));
}

// Reads that check the bounds of the buffer they read from; out of range yields 0.
inline uint16_t readU16(ByteSpan data, size_t offset)
{
    return offset + 2 <= data.size() ? readU16(data.data() + offset) : 0;
}

inline uint32_t readU32(ByteSpan data, size_t offset)
{
    return offset + 4 <= data.size() ? readU32(data.data() + offset) : 0;
}

inline uint8_t readU8(ByteSpan data, size_t offset)
{
    return offset < data.size() ? data[offset] : 0;
}

inline bool startsWith(ByteSpan data, const char* magic)
{
    const size_t length = std::strlen(magic);
    return data.size() >= length && std::memcmp(data.data(), magic, length) == 0;
}

} // namespace tz
