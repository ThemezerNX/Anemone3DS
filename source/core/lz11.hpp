// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// LZ11, the compression of theme bodies (body_LZ.bin).

#pragma once

#include "core/bytes.hpp"
#include "core/result.hpp"

namespace tz::lz11
{

// The decompressed size stated in the header, or 0 when the data isn't LZ11.
uint32_t decompressedSize(ByteSpan data);

// InvalidData for corrupt input, TooLarge when the stated size is above maxSize.
Expected<Bytes> decompress(ByteSpan data, size_t maxSize);

// Compresses well enough that a theme body which fitted the theme cache once compressed by a theme editor fits
// again, and fast enough for a 3DS (a few hundred milliseconds for a body).
Bytes compress(ByteSpan data);

} // namespace tz::lz11
