// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Test data: zips, theme bodies, HOME Menu files.

#pragma once

#include "core/bytes.hpp"

#include <string>
#include <utility>
#include <vector>

namespace test
{

// A zip with these files, stored without compression.
tz::Bytes storedZip(const std::vector<std::pair<std::string, tz::Bytes>>& files);

// An LZ11-compressed theme body of plainSize bytes, with the music flag (offset 5) set to musicFlag.
tz::Bytes themeBody(uint8_t musicFlag, size_t plainSize = 0x2000, uint8_t fill = 0);

// Something that starts like a BCSTM.
tz::Bytes fakeBgm(size_t size, uint8_t fill);

// A HOME Menu SaveData.dat as the current firmware makes it, all zero.
tz::Bytes blankSaveData();

tz::Bytes bytesOf(const std::string& text);

} // namespace test
