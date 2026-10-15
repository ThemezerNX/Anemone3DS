// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "fixtures.hpp"

#include "core/lz11.hpp"
#include "core/zip.hpp"

namespace test
{

namespace
{
void put16(tz::Bytes& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(tz::Bytes& out, uint32_t v)
{
    put16(out, static_cast<uint16_t>(v));
    put16(out, static_cast<uint16_t>(v >> 16));
}
} // namespace

tz::Bytes storedZip(const std::vector<std::pair<std::string, tz::Bytes>>& files)
{
    tz::Bytes out;
    tz::Bytes central;
    for (const auto& [name, data] : files)
    {
        const uint32_t offset = static_cast<uint32_t>(out.size());
        const uint32_t crc = tz::crc32(data);
        put32(out, 0x04034B50);
        put16(out, 20);
        put16(out, 0x0800); // UTF-8 names
        put16(out, 0);      // stored
        put32(out, 0);      // time and date
        put32(out, crc);
        put32(out, static_cast<uint32_t>(data.size()));
        put32(out, static_cast<uint32_t>(data.size()));
        put16(out, static_cast<uint16_t>(name.size()));
        put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());

        put32(central, 0x02014B50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, 0);
        put32(central, 0);
        put32(central, crc);
        put32(central, static_cast<uint32_t>(data.size()));
        put32(central, static_cast<uint32_t>(data.size()));
        put16(central, static_cast<uint16_t>(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
    }
    const uint32_t centralOffset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054B50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint16_t>(files.size()));
    put16(out, static_cast<uint16_t>(files.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, centralOffset);
    put16(out, 0);
    return out;
}

tz::Bytes themeBody(uint8_t musicFlag, size_t plainSize, uint8_t fill)
{
    tz::Bytes plain(plainSize, fill);
    plain[0] = 1; // version
    plain[5] = musicFlag;
    for (size_t i = 0x100; i < plain.size(); i += 7)
        plain[i] = static_cast<uint8_t>(i * 13 + fill);
    return tz::lz11::compress(plain);
}

tz::Bytes fakeBgm(size_t size, uint8_t fill)
{
    tz::Bytes bgm(size, fill);
    bgm[0] = 'C';
    bgm[1] = 'S';
    bgm[2] = 'T';
    bgm[3] = 'M';
    return bgm;
}

tz::Bytes blankSaveData()
{
    return tz::Bytes(0x2DA0, 0);
}

tz::Bytes bytesOf(const std::string& text)
{
    return tz::Bytes(text.begin(), text.end());
}

} // namespace test
