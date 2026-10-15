// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/bcstm.hpp"
#include "test.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

using namespace tz;

namespace
{

// The app's own music: looping stereo DSP-ADPCM at 32728 Hz. Tests run from the root of the repository.
constexpr const char* kMusicPath = "romfs/bgm.bcstm";
constexpr size_t kMusicSize = 1686752;

// Where things are in it, from its header.
constexpr size_t kBlockCount = 0x10;      // u16, in the file header
constexpr size_t kInfoReference = 0x14;   // the INFO block's sized reference
constexpr size_t kStreamReference = 0x48; // in the INFO block
constexpr size_t kTrackReference = 0x50;
constexpr size_t kChannelReference = 0x58;
constexpr size_t kStreamInfo = 0x60;
constexpr size_t kTrackTable = 0x98;
constexpr size_t kChannelTable = 0xA4;
constexpr size_t kChannelInfo0 = 0xCC;
constexpr size_t kDataBlock = 0x480;
constexpr size_t kSamples = 0x4A0;

Bytes readFile(const char* path)
{
    Bytes data;
    std::FILE* file = std::fopen(path, "rb");
    if (!file)
        return data;
    uint8_t chunk[0x4000];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
        data.insert(data.end(), chunk, chunk + got);
    std::fclose(file);
    return data;
}

// The start of the bundled music, as the player reads it.
Bytes musicHeader()
{
    Bytes data = readFile(kMusicPath);
    if (data.size() > bcstm::kHeaderSize)
        data.resize(bcstm::kHeaderSize);
    return data;
}

Bytes changed(Bytes data, const std::function<void(Bytes&)>& change)
{
    change(data);
    return data;
}

std::function<void(Bytes&)> set16(size_t at, uint16_t value)
{
    return [=](Bytes& data) { writeU16(data.data() + at, value); };
}

std::function<void(Bytes&)> set32(size_t at, uint32_t value)
{
    return [=](Bytes& data) { writeU32(data.data() + at, value); };
}

std::function<void(Bytes&)> set8(size_t at, uint8_t value)
{
    return [=](Bytes& data) { data[at] = value; };
}

bool failsWith(const Expected<bcstm::Info>& result, Errc code)
{
    return !result && result.error().code == code;
}

// What the player relies on, whenever parse() succeeds.
void checkSane(const Expected<bcstm::Info>& result)
{
    if (!result)
        return;
    const bcstm::Info& info = *result;
    CHECK(info.channels == 1 || info.channels == 2);
    CHECK(info.encoding == bcstm::Encoding::Pcm8 || info.encoding == bcstm::Encoding::Pcm16 ||
          info.encoding == bcstm::Encoding::DspAdpcm);
    CHECK(info.sampleRate > 0);
    CHECK(info.blockCount > 0 && info.blockSamples > 0 && info.blockSize > 0);
    CHECK(info.lastBlockSamples > 0 && info.lastBlockSamples <= info.blockSamples);
    CHECK(info.lastBlockSize <= info.lastBlockPaddedSize && info.lastBlockSize <= info.blockSize);
    const uint64_t length = uint64_t(info.blockCount - 1) * info.blockSamples + info.lastBlockSamples;
    CHECK(info.loopEnd > 0 && info.loopEnd <= length);
    CHECK(!info.looping || info.loopStart < info.loopEnd);
}

// A BCSTM laid out the way encoders write one (file header, INFO, DATA; no SEEK block, no tracks), with silent
// samples, for what the bundled music doesn't cover.
struct Synthetic
{
    bcstm::Encoding encoding = bcstm::Encoding::Pcm16;
    bool looping = false;
    uint8_t channels = 1;
    uint32_t sampleRate = 22050;
    uint32_t loopStart = 0;
    uint32_t blockCount = 3;
    uint32_t blockSamples = 0x1000;
    uint32_t lastBlockSamples = 0x123;
};

uint32_t bytesFor(bcstm::Encoding encoding, uint32_t samples)
{
    switch (encoding)
    {
    case bcstm::Encoding::Pcm8:
        return samples;
    case bcstm::Encoding::Pcm16:
        return samples * 2;
    case bcstm::Encoding::DspAdpcm:
        return samples / 14 * 8 + (samples % 14 ? 1 + (samples % 14 + 1) / 2 : 0);
    }
    return 0;
}

uint32_t align(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

Bytes build(const Synthetic& s)
{
    const bool adpcm = s.encoding == bcstm::Encoding::DspAdpcm;
    const uint32_t blockSize = align(bytesFor(s.encoding, s.blockSamples), 0x20);
    const uint32_t lastBlockSize = bytesFor(s.encoding, s.lastBlockSamples);
    const uint32_t lastPadded = align(lastBlockSize, 0x20);
    const uint32_t length = (s.blockCount - 1) * s.blockSamples + s.lastBlockSamples;

    // INFO, relative to its start: header, references, stream info at 0x20, channel table at 0x58, channel infos,
    // then the DSP-ADPCM infos
    const uint32_t channelTable = 0x58;
    const uint32_t channelInfos = channelTable + 4 + 8 * s.channels;
    const uint32_t adpcmInfos = channelInfos + 8 * s.channels;
    const uint32_t infoSize = align(adpcmInfos + (adpcm ? 0x2E * s.channels : 0), 0x20);
    const uint32_t info = 0x40;
    const uint32_t data = info + infoSize;
    const uint32_t dataSize = 0x20 + (s.blockCount - 1) * blockSize * s.channels + lastPadded * s.channels;

    Bytes file(data + dataSize, 0);
    uint8_t* p = file.data();
    std::memcpy(p, "CSTM", 4);
    writeU16(p + 0x04, 0xFEFF);
    writeU16(p + 0x06, 0x40);
    writeU32(p + 0x08, 0x02000000);
    writeU32(p + 0x0C, static_cast<uint32_t>(file.size()));
    writeU16(p + 0x10, 2);
    writeU16(p + 0x14, 0x4000);
    writeU32(p + 0x18, info);
    writeU32(p + 0x1C, infoSize);
    writeU16(p + 0x20, 0x4002);
    writeU32(p + 0x24, data);
    writeU32(p + 0x28, dataSize);

    uint8_t* i = p + info;
    std::memcpy(i, "INFO", 4);
    writeU32(i + 0x04, infoSize);
    writeU16(i + 0x08, 0x4100); // stream info, relative to INFO + 8
    writeU32(i + 0x0C, 0x20 - 8);
    writeU32(i + 0x14, 0xFFFFFFFF); // no track table
    writeU16(i + 0x18, 0x0101);
    writeU32(i + 0x1C, channelTable - 8);

    uint8_t* stream = i + 0x20;
    stream[0x00] = static_cast<uint8_t>(s.encoding);
    stream[0x01] = s.looping;
    stream[0x02] = s.channels;
    writeU32(stream + 0x04, s.sampleRate);
    writeU32(stream + 0x08, s.loopStart);
    writeU32(stream + 0x0C, length);
    writeU32(stream + 0x10, s.blockCount);
    writeU32(stream + 0x14, blockSize);
    writeU32(stream + 0x18, s.blockSamples);
    writeU32(stream + 0x1C, lastBlockSize);
    writeU32(stream + 0x20, s.lastBlockSamples);
    writeU32(stream + 0x24, lastPadded);
    writeU16(stream + 0x30, 0x1F00);
    writeU32(stream + 0x34, 0x18); // the samples at DATA + 0x20

    writeU32(i + channelTable, s.channels);
    for (uint32_t c = 0; c < s.channels; ++c)
    {
        writeU16(i + channelTable + 4 + 8 * c, 0x4102);
        writeU32(i + channelTable + 8 + 8 * c, channelInfos + 8 * c - channelTable);
        uint8_t* channel = i + channelInfos + 8 * c;
        if (!adpcm)
        {
            writeU32(channel + 4, 0xFFFFFFFF);
            continue;
        }
        writeU16(channel, 0x0300);
        writeU32(channel + 4, adpcmInfos + 0x2E * c - (channelInfos + 8 * c));
        uint8_t* coefficients = i + adpcmInfos + 0x2E * c;
        for (uint32_t k = 0; k < 16; ++k)
            writeU16(coefficients + 2 * k, static_cast<uint16_t>(100 * c + k));
        writeU16(coefficients + 0x20, 0x17);
        writeU16(coefficients + 0x26, 0x23 + c);
        writeU16(coefficients + 0x28, static_cast<uint16_t>(-1000));
    }

    std::memcpy(p + data, "DATA", 4);
    writeU32(p + data + 4, dataSize);
    return file;
}

} // namespace

TEST(bcstm_bundled_music)
{
    const Bytes header = musicHeader();
    REQUIRE(header.size() == bcstm::kHeaderSize);
    const auto info = bcstm::parse(header);
    REQUIRE(info.has_value());
    CHECK(info->encoding == bcstm::Encoding::DspAdpcm);
    CHECK(info->looping);
    CHECK_EQ(info->channels, 2);
    CHECK(!info->mono());
    CHECK_EQ(info->sampleRate, 32728u);
    CHECK_EQ(info->loopStart, 530432u);
    CHECK_EQ(info->loopEnd, 1474817u);
    CHECK_EQ(info->blockCount, 103u);
    CHECK_EQ(info->blockSize, 0x2000u);
    CHECK_EQ(info->blockSamples, 14336u);
    CHECK_EQ(info->lastBlockSize, 7170u);
    CHECK_EQ(info->lastBlockSamples, 12545u);
    CHECK_EQ(info->lastBlockPaddedSize, 7200u);
    CHECK_EQ(info->dataOffset, uint32_t(kSamples));
    // the loop starts on a block
    CHECK_EQ(info->loopStart % info->blockSamples, 0u);

    const bcstm::AdpcmChannel& left = info->adpcm[0];
    CHECK_EQ(left.coefficients[0], 218);
    CHECK_EQ(left.coefficients[1], -475);
    CHECK_EQ(left.coefficients[15], -1895);
    CHECK_EQ(left.predictorScale, 0x58);
    CHECK_EQ(left.history1, 0);
    CHECK_EQ(left.history2, 0);
    CHECK_EQ(left.loopPredictorScale, 0x68);
    CHECK_EQ(left.loopHistory1, -5151);
    CHECK_EQ(left.loopHistory2, -5353);

    const bcstm::AdpcmChannel& right = info->adpcm[1];
    CHECK_EQ(right.coefficients[0], 166);
    CHECK_EQ(right.predictorScale, 0x68);
    CHECK_EQ(right.loopPredictorScale, 0x67);
    CHECK_EQ(right.loopHistory1, -5525);
    CHECK_EQ(right.loopHistory2, -5196);
}

TEST(bcstm_header_lengths)
{
    const Bytes file = readFile(kMusicPath);
    REQUIRE(file.size() == kMusicSize);
    const auto expected = bcstm::parse(ByteSpan(file).first(bcstm::kHeaderSize));
    REQUIRE(expected.has_value());

    // the whole file reads the same
    const auto whole = bcstm::parse(file);
    REQUIRE(whole.has_value());
    CHECK_EQ(whole->dataOffset, expected->dataOffset);
    CHECK_EQ(whole->loopEnd, expected->loopEnd);

    // the header is enough up to the DATA block's own header; anything shorter fails, and never reads past the end
    const auto shortest = bcstm::parse(ByteSpan(file).first(kDataBlock + 8));
    REQUIRE(shortest.has_value());
    CHECK_EQ(shortest->dataOffset, expected->dataOffset);
    for (size_t length = 0; length < kDataBlock + 8; ++length)
    {
        // a copy of exactly that length, so the sanitizer sees any read past it
        const Bytes cut(file.begin(), file.begin() + static_cast<ptrdiff_t>(length));
        if (!failsWith(bcstm::parse(cut), Errc::InvalidData))
            test::fail(__FILE__, __LINE__, "a header of " + std::to_string(length) + " bytes didn't fail");
    }
}

TEST(bcstm_unsupported)
{
    const Bytes header = musicHeader();
    REQUIRE(header.size() == bcstm::kHeaderSize);
    CHECK(failsWith(bcstm::parse(changed(header, set8(kStreamInfo + 2, 0))), Errc::Unsupported));
    CHECK(failsWith(bcstm::parse(changed(header, set8(kStreamInfo + 2, 3))), Errc::Unsupported));
    CHECK(failsWith(bcstm::parse(changed(header, set8(kStreamInfo + 2, 0xFF))), Errc::Unsupported));
    CHECK(failsWith(bcstm::parse(changed(header, set8(kStreamInfo, 3))), Errc::Unsupported)); // IMA-ADPCM
    CHECK(failsWith(bcstm::parse(changed(header, set8(kStreamInfo, 0x80))), Errc::Unsupported));
    // the same file written big endian: its mark reads backwards
    CHECK(failsWith(bcstm::parse(changed(header, set16(4, 0xFFFE))), Errc::Unsupported));
}

TEST(bcstm_invalid)
{
    const Bytes header = musicHeader();
    REQUIRE(header.size() == bcstm::kHeaderSize);
    const auto invalid = [&](const std::function<void(Bytes&)>& change) {
        return failsWith(bcstm::parse(changed(header, change)), Errc::InvalidData);
    };

    // file header
    CHECK(invalid(set8(3, 'X')));
    CHECK(invalid(set16(4, 0x0000)));
    CHECK(invalid(set32(0x0C, kDataBlock + 0x1000))); // a file size that ends before the DATA block does
    CHECK(invalid(set16(kBlockCount, 0)));
    CHECK(invalid(set16(kBlockCount, 0xFFFF)));
    CHECK(invalid(set16(kInfoReference, 0x4001)));        // no INFO block
    CHECK(invalid(set32(kInfoReference + 4, 0x3FFC)));    // INFO's header past the end
    CHECK(invalid(set32(kInfoReference + 4, 0x44)));      // no INFO block there
    CHECK(invalid(set32(kInfoReference + 4, 0xFFFFFFFF)));
    CHECK(invalid(set32(0x44, 0x4000)));                  // an INFO block bigger than the header
    CHECK(invalid(set32(0x44, 0x10)));                    // too small for its references
    CHECK(invalid(set16(0x2C, 0x4001)));                  // no DATA block
    CHECK(invalid(set32(0x30, 0x7FFFFFFF)));              // DATA's header past the end
    CHECK(invalid(set8(kDataBlock, 'd')));

    // INFO's references
    CHECK(invalid(set16(kStreamReference, 0x4101)));
    CHECK(invalid(set32(kStreamReference + 4, 0xFFFFFFFF)));
    CHECK(invalid(set32(kStreamReference + 4, 0xE0))); // runs past the end of INFO
    CHECK(invalid(set16(kTrackReference, 0x0100)));
    CHECK(invalid(set32(kTrackReference + 4, 0x200)));
    CHECK(invalid(set32(kTrackTable, 0x1000))); // more tracks than fit
    CHECK(invalid(set16(kTrackTable + 4, 0x4102)));
    CHECK(invalid(set16(kChannelReference, 0x0100)));
    CHECK(invalid(set32(kChannelReference + 4, 0xFFFFFFFF)));
    CHECK(invalid(set32(kChannelReference + 4, 0xFC)));
    CHECK(invalid(set32(kChannelTable, 1))); // fewer channel infos than channels
    CHECK(invalid(set32(kChannelTable, 0x40000000)));
    CHECK(invalid(set16(kChannelTable + 4, 0x4101)));
    CHECK(invalid(set32(kChannelTable + 8, 0x1000)));
    CHECK(invalid(set16(kChannelInfo0, 0x0301))); // IMA-ADPCM info in a DSP-ADPCM stream
    CHECK(invalid(set32(kChannelInfo0 + 4, 0xFFFFFFFF)));
    CHECK(invalid(set32(kChannelInfo0 + 4, 0x70))); // the coefficients would run past INFO

    // the stream info
    CHECK(invalid(set32(kStreamInfo + 0x04, 0)));          // sample rate
    CHECK(invalid(set32(kStreamInfo + 0x0C, 0)));          // loop end
    CHECK(invalid(set32(kStreamInfo + 0x0C, 1474818)));    // past the last sample
    CHECK(invalid(set32(kStreamInfo + 0x08, 1474817)));    // a loop that starts at its end
    CHECK(invalid(set32(kStreamInfo + 0x10, 0)));          // block count
    CHECK(invalid(set32(kStreamInfo + 0x10, 104)));        // one block more than the DATA block holds
    CHECK(invalid(set32(kStreamInfo + 0x10, 0xFFFFFFFF)));
    CHECK(invalid(set32(kStreamInfo + 0x14, 0)));          // block size
    CHECK(invalid(set32(kStreamInfo + 0x14, 0x1FFF)));     // too small for 14336 samples
    CHECK(invalid(set32(kStreamInfo + 0x14, 0xFFFFFFFF)));
    CHECK(invalid(set32(kStreamInfo + 0x18, 0)));          // samples per block
    CHECK(invalid(set32(kStreamInfo + 0x1C, 7169)));       // last block too small for its samples
    CHECK(invalid(set32(kStreamInfo + 0x1C, 7201)));       // bigger than with its padding
    CHECK(invalid(set32(kStreamInfo + 0x20, 0)));
    CHECK(invalid(set32(kStreamInfo + 0x20, 14337)));
    CHECK(invalid(set32(kStreamInfo + 0x24, 7169)));
    CHECK(invalid(set16(kStreamInfo + 0x30, 0x1F01))); // sample data reference
    CHECK(invalid(set32(kStreamInfo + 0x34, 0xFFFFFFFF)));
    CHECK(invalid(set32(kStreamInfo + 0x34, 0x19B860)));
    CHECK(invalid(set8(kStreamInfo, 0))); // PCM8 doesn't fit 14336 samples in 0x2000 bytes

    // the last channel's padding may be left out of the DATA block, but no more
    const uint32_t dataSize = readU32(header.data() + kDataBlock + 4);
    CHECK(bcstm::parse(changed(header, set32(kDataBlock + 4, dataSize - 30))).has_value());
    CHECK(invalid(set32(kDataBlock + 4, dataSize - 31)));
}

TEST(bcstm_optional_parts)
{
    const Bytes header = musicHeader();
    REQUIRE(header.size() == bcstm::kHeaderSize);

    // no track table
    CHECK(bcstm::parse(changed(header, set32(kTrackReference + 4, 0xFFFFFFFF))).has_value());

    // the loop start doesn't matter in music that doesn't loop
    const auto once = bcstm::parse(changed(header, [](Bytes& data) {
        data[kStreamInfo + 1] = 0;
        writeU32(data.data() + kStreamInfo + 0x08, 0xFFFFFFFF);
    }));
    REQUIRE(once.has_value());
    CHECK(!once->looping);
    CHECK_EQ(once->loopEnd, 1474817u);
}

TEST(bcstm_synthetic)
{
    // mono PCM16, played once
    Synthetic pcm16;
    const auto mono = bcstm::parse(build(pcm16));
    REQUIRE(mono.has_value());
    CHECK(mono->encoding == bcstm::Encoding::Pcm16);
    CHECK(mono->mono());
    CHECK(!mono->looping);
    CHECK_EQ(mono->sampleRate, 22050u);
    CHECK_EQ(mono->blockSize, 0x2000u);
    CHECK_EQ(mono->lastBlockSize, 0x246u);
    CHECK_EQ(mono->lastBlockPaddedSize, 0x260u);
    CHECK_EQ(mono->loopEnd, 2u * 0x1000 + 0x123);
    CHECK_EQ(mono->adpcm[0].coefficients[3], 0);

    // stereo PCM8, looping, whole: the last block is the only one
    Synthetic pcm8;
    pcm8.encoding = bcstm::Encoding::Pcm8;
    pcm8.channels = 2;
    pcm8.looping = true;
    pcm8.loopStart = 0x50;
    pcm8.blockCount = 1;
    pcm8.lastBlockSamples = 0x1000;
    const Bytes stereo = build(pcm8);
    const auto parsed = bcstm::parse(stereo);
    REQUIRE(parsed.has_value());
    CHECK(parsed->encoding == bcstm::Encoding::Pcm8);
    CHECK_EQ(parsed->channels, 2);
    CHECK(parsed->looping);
    CHECK_EQ(parsed->loopStart, 0x50u);
    CHECK_EQ(parsed->dataOffset, readU32(stereo.data() + 0x24) + 0x20);

    // mono DSP-ADPCM, with a loop that doesn't start on a block
    Synthetic adpcm;
    adpcm.encoding = bcstm::Encoding::DspAdpcm;
    adpcm.looping = true;
    adpcm.loopStart = 14336 + 140;
    adpcm.blockSamples = 14336;
    adpcm.lastBlockSamples = 1;
    const auto dsp = bcstm::parse(build(adpcm));
    REQUIRE(dsp.has_value());
    CHECK_EQ(dsp->lastBlockSize, 2u); // a frame header and one sample
    CHECK_EQ(dsp->adpcm[0].coefficients[15], 15);
    CHECK_EQ(dsp->adpcm[0].predictorScale, 0x17);
    CHECK_EQ(dsp->adpcm[0].loopPredictorScale, 0x23);
    CHECK_EQ(dsp->adpcm[0].loopHistory1, -1000);

    // stereo PCM16 is fine, with the stream info saying three channels it isn't
    Synthetic two = pcm16;
    two.channels = 2;
    Bytes file = build(two);
    CHECK(bcstm::parse(file).has_value());
    file[0x40 + 0x20 + 2] = 3; // INFO, its stream info, the channel count
    CHECK(failsWith(bcstm::parse(file), Errc::Unsupported));
}

// Damage anywhere in the header, one byte at a time: parse() fails or gives something playable, and never reads past
// the buffer (the sanitizers catch that).
TEST(bcstm_damaged_bytes)
{
    const Bytes header = musicHeader();
    REQUIRE(header.size() == bcstm::kHeaderSize);
    const uint8_t values[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
    for (size_t at = 0; at < kSamples; ++at)
    {
        for (uint8_t value : values)
            checkSane(bcstm::parse(changed(header, set8(at, value))));
        checkSane(bcstm::parse(changed(header, set8(at, static_cast<uint8_t>(header[at] ^ 0x10)))));
    }
}

TEST(bcstm_random_damage)
{
    const Bytes header = musicHeader();
    REQUIRE(header.size() == bcstm::kHeaderSize);
    uint32_t seed = 0x3D5;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int round = 0; round < 5000; ++round)
    {
        Bytes data = header;
        const uint32_t changes = 1 + next() % 6;
        for (uint32_t i = 0; i < changes; ++i)
        {
            // mostly 32-bit fields, which is where offsets and counts are
            const size_t at = (next() % kSamples) & ~size_t(3);
            if (next() % 2)
                writeU32(data.data() + at, next() % 4 ? next() % 0x500 : next() * 2654435761u);
            else
                data[at + next() % 4] = static_cast<uint8_t>(next());
        }
        if (next() % 4 == 0)
            data.resize(next() % (kSamples + 0x40));
        checkSane(bcstm::parse(data));
    }
}
