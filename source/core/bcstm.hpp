// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// BCSTM, the HOME Menu's music format (bgm.bcstm).

#pragma once

#include "core/bytes.hpp"
#include "core/result.hpp"

#include <array>

namespace tz::bcstm
{

// The HOME Menu's limit for a theme's music.
constexpr uint32_t kMaxSize = 0x337000;

enum class Encoding : uint8_t
{
    Pcm8 = 0,
    Pcm16 = 1,
    DspAdpcm = 2,
};

struct AdpcmChannel
{
    std::array<int16_t, 16> coefficients{};
    // decoder state at the start of the stream, and at the loop start
    uint16_t predictorScale = 0;
    int16_t history1 = 0;
    int16_t history2 = 0;
    uint16_t loopPredictorScale = 0;
    int16_t loopHistory1 = 0;
    int16_t loopHistory2 = 0;
};

struct Info
{
    Encoding encoding = Encoding::DspAdpcm;
    bool looping = false;
    uint8_t channels = 0; // 1 or 2 are played; more are reported as Unsupported
    uint32_t sampleRate = 0;
    uint32_t loopStart = 0; // in samples
    uint32_t loopEnd = 0;   // in samples: the length of the music
    uint32_t blockCount = 0;
    uint32_t blockSize = 0;    // bytes per channel per block
    uint32_t blockSamples = 0; // samples per block
    uint32_t lastBlockSize = 0;
    uint32_t lastBlockSamples = 0;
    uint32_t lastBlockPaddedSize = 0;
    uint32_t dataOffset = 0; // where the first block starts in the file; blocks are interleaved by channel
    std::array<AdpcmChannel, 2> adpcm{};

    // Mono music, which the HOME Menu is known to play badly; the app warns before installing it.
    bool mono() const { return channels == 1; }
};

// Reads the header from the start of the file. header must reach the sample data: kHeaderSize bytes always do.
// InvalidData for anything that isn't a BCSTM, Unsupported for more than two channels.
Expected<Info> parse(ByteSpan header);

constexpr size_t kHeaderSize = 0x4000;

} // namespace tz::bcstm
