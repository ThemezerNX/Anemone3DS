// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The BCSTM header, read by following its references the way the format lays it out (3dbrew's BCSTM page, after
// NintendoWare's CSTM layout) rather than by searching for magic values. Every offset is checked against the bytes
// given, so a damaged file fails with InvalidData instead of being read past its end.
//
// The file header, at 0:
//   0x00  "CSTM"
//   0x04  u16  byte order mark: 0xFEFF read little endian, as every 3DS file is
//   0x06  u16  header size
//   0x08  u32  version
//   0x0C  u32  file size
//   0x10  u16  block count
//   0x14  per block, a sized reference: u16 type, u16 padding, u32 offset from the start of the file, u32 size.
//         0x4000 is the INFO block, 0x4001 SEEK (the decoder state at every block, for seeking; not needed to
//         play from the start), 0x4002 DATA.
//
// Inside the blocks, a reference is u16 type, u16 padding, u32 offset, with 0xFFFFFFFF for none; what the offset is
// relative to depends on where the reference is. A reference table is a u32 count, then that many references
// relative to the start of the table.
//
// The INFO block:
//   0x00  "INFO", u32 size
//   0x08  references relative to 0x08: the stream info (0x4100), the track info table (0x0101; may be none) and the
//         channel info table (0x0101)
// The stream info: see readStreamInfo().
// A track info (0x4101): u8 volume, u8 pan, u16 padding, a reference to its channel numbers. Not needed to play.
// A channel info (0x4102): a reference, relative to the channel info, to its codec's info: 0x0300 for DSP-ADPCM,
//   none for PCM.
// The DSP-ADPCM info: see readAdpcm().
//
// The DATA block: "DATA", u32 size, then the samples, where the stream info's sample data reference (0x1F00, relative
// to the end of those 8 bytes) points. They come in blocks of blockSamples samples, each split by channel: block 0 of
// channel 0, block 0 of channel 1, block 1 of channel 0... Each channel's part of the last block is padded to
// lastBlockPaddedSize.

#include "core/bcstm.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace tz::bcstm
{

namespace
{

// Reference types.
constexpr uint16_t kInfoBlock = 0x4000;
constexpr uint16_t kDataBlock = 0x4002;
constexpr uint16_t kReferenceTable = 0x0101;
constexpr uint16_t kDspAdpcmInfo = 0x0300;
constexpr uint16_t kSampleData = 0x1F00;
constexpr uint16_t kStreamInfo = 0x4100;
constexpr uint16_t kTrackInfo = 0x4101;
constexpr uint16_t kChannelInfo = 0x4102;

constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr uint16_t kLittleEndian = 0xFEFF;
constexpr uint16_t kBigEndian = 0xFFFE; // the mark as it reads from a big endian file

constexpr size_t kFileHeaderSize = 0x14; // up to the block references
constexpr size_t kSizedReferenceSize = 12;
constexpr size_t kReferenceSize = 8;
constexpr size_t kBlockHeaderSize = 8; // magic and size
constexpr size_t kStreamInfoSize = 0x38;
constexpr size_t kTrackInfoSize = 0x0C;
constexpr size_t kAdpcmInfoSize = 0x2C; // without its 2 bytes of padding

// A DSP-ADPCM frame is a byte of predictor and scale followed by 14 samples of 4 bits.
constexpr uint64_t kAdpcmFrameBytes = 8;
constexpr uint64_t kAdpcmFrameSamples = 14;

struct Reference
{
    uint16_t type = 0;
    uint32_t offset = kNone;
};

struct Block
{
    uint64_t offset = 0;
    uint64_t size = 0;
};

// Whether data holds size bytes at offset.
bool fits(ByteSpan data, uint64_t offset, uint64_t size)
{
    return offset <= data.size() && size <= data.size() - offset;
}

std::string hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%X", static_cast<unsigned>(value));
    return text;
}

Expected<Reference> readReference(ByteSpan data, uint64_t at, const char* what)
{
    if (!fits(data, at, kReferenceSize))
        return fail(Errc::InvalidData, std::string(what) + ": reference out of bounds");
    const uint8_t* p = data.data() + at;
    return Reference{readU16(p), readU32(p + 4)};
}

// Where the reference at `at` points, relative to base. It must be there, of the expected type, with size bytes
// where it points.
Expected<uint64_t> follow(ByteSpan data, uint64_t at, uint64_t base, uint16_t type, uint64_t size, const char* what)
{
    const Reference reference = TZ_TRY(readReference(data, at, what));
    if (reference.offset == kNone)
        return fail(Errc::InvalidData, std::string(what) + ": missing");
    if (reference.type != type)
        return fail(Errc::InvalidData, std::string(what) + ": type " + hex(reference.type));
    const uint64_t target = base + reference.offset;
    if (!fits(data, target, size))
        return fail(Errc::InvalidData, std::string(what) + ": out of bounds");
    return target;
}

// A block, from the file header's references. Its own header (magic and size) must be in data; the block itself may
// go on past it, as the DATA block does.
Expected<Block> findBlock(ByteSpan data, uint16_t type, const char* magic)
{
    const size_t count = readU16(data.data() + 0x10);
    if (!fits(data, kFileHeaderSize, uint64_t(count) * kSizedReferenceSize))
        return fail(Errc::InvalidData, "block references out of bounds");
    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t* reference = data.data() + kFileHeaderSize + i * kSizedReferenceSize;
        if (readU16(reference) != type)
            continue;
        const uint64_t offset = readU32(reference + 4);
        if (!fits(data, offset, kBlockHeaderSize))
            return fail(Errc::InvalidData, std::string(magic) + " block out of bounds");
        if (std::memcmp(data.data() + offset, magic, 4) != 0)
            return fail(Errc::InvalidData, std::string("no ") + magic + " block where its reference points");
        return Block{offset, readU32(data.data() + offset + 4)};
    }
    return fail(Errc::InvalidData, std::string("no ") + magic + " block");
}

// The stream info:
//   0x00  u8   encoding: 0 PCM8, 1 PCM16, 2 DSP-ADPCM, 3 IMA-ADPCM
//   0x01  u8   loops
//   0x02  u8   channel count
//   0x04  u32  sample rate
//   0x08  u32  loop start, in samples
//   0x0C  u32  loop end: where the music ends, looping or not
//   0x10  u32  block count
//   0x14  u32  block size, per channel
//   0x18  u32  samples per block
//   0x1C  u32  the last block's size, per channel
//   0x20  u32  the last block's samples
//   0x24  u32  the last block's size with its padding: how far apart its channels are
//   0x28  u32  seek entry size, 0x2C u32 samples per seek entry, for the SEEK block
//   0x30  the sample data reference (0x1F00), relative to the DATA block's contents
// Returns the sample data reference.
Expected<Reference> readStreamInfo(const uint8_t* p, Info& info)
{
    const uint8_t encoding = p[0x00];
    if (encoding == 3)
        return fail(Errc::Unsupported, "IMA-ADPCM music");
    if (encoding > static_cast<uint8_t>(Encoding::DspAdpcm))
        return fail(Errc::Unsupported, "encoding " + std::to_string(encoding));
    info.encoding = static_cast<Encoding>(encoding);
    info.looping = p[0x01] != 0;
    info.channels = p[0x02];
    if (info.channels == 0 || info.channels > 2)
        return fail(Errc::Unsupported, std::to_string(info.channels) + " channels");
    info.sampleRate = readU32(p + 0x04);
    info.loopStart = readU32(p + 0x08);
    info.loopEnd = readU32(p + 0x0C);
    info.blockCount = readU32(p + 0x10);
    info.blockSize = readU32(p + 0x14);
    info.blockSamples = readU32(p + 0x18);
    info.lastBlockSize = readU32(p + 0x1C);
    info.lastBlockSamples = readU32(p + 0x20);
    info.lastBlockPaddedSize = readU32(p + 0x24);
    return Reference{readU16(p + 0x30), readU32(p + 0x34)};
}

// The DSP-ADPCM info:
//   0x00  16 s16 coefficients
//   0x20  the decoder state at the start: u16 predictor and scale, s16 history 1 (the last sample), s16 history 2
//   0x26  the same at the loop start
//   0x2C  padding
void readAdpcm(const uint8_t* p, AdpcmChannel& channel)
{
    for (size_t i = 0; i < channel.coefficients.size(); ++i)
        channel.coefficients[i] = static_cast<int16_t>(readU16(p + i * 2));
    channel.predictorScale = readU16(p + 0x20);
    channel.history1 = static_cast<int16_t>(readU16(p + 0x22));
    channel.history2 = static_cast<int16_t>(readU16(p + 0x24));
    channel.loopPredictorScale = readU16(p + 0x26);
    channel.loopHistory1 = static_cast<int16_t>(readU16(p + 0x28));
    channel.loopHistory2 = static_cast<int16_t>(readU16(p + 0x2A));
}

// Track infos aren't needed to play, but a table of them must be sound when there is one: the HOME Menu reads it.
Expected<void> checkTracks(ByteSpan info)
{
    const Reference reference = TZ_TRY(readReference(info, 0x10, "track info table"));
    if (reference.offset == kNone)
        return {};
    const uint64_t table = TZ_TRY(follow(info, 0x10, 0x08, kReferenceTable, 4, "track info table"));
    const uint32_t count = readU32(info.data() + table);
    if (!fits(info, table + 4, uint64_t(count) * kReferenceSize))
        return fail(Errc::InvalidData, "track info table out of bounds");
    for (uint32_t i = 0; i < count; ++i)
    {
        auto track = follow(info, table + 4 + uint64_t(i) * kReferenceSize, table, kTrackInfo, kTrackInfoSize,
                            "track info");
        if (!track)
            return fail(track.error());
    }
    return {};
}

// The bytes holding this many samples of one channel.
uint64_t bytesFor(Encoding encoding, uint64_t samples)
{
    switch (encoding)
    {
    case Encoding::Pcm8:
        return samples;
    case Encoding::Pcm16:
        return samples * 2;
    case Encoding::DspAdpcm:
    {
        // whole frames, then a frame cut short: its header byte and two samples a byte
        const uint64_t rest = samples % kAdpcmFrameSamples;
        return samples / kAdpcmFrameSamples * kAdpcmFrameBytes + (rest ? 1 + (rest + 1) / 2 : 0);
    }
    }
    return 0;
}

// What the player relies on: blocks big enough for their samples, all of them inside the DATA block, and the loop
// inside the music.
Expected<void> checkLayout(const Info& info, uint64_t samples, const Block& data)
{
    if (info.sampleRate == 0)
        return fail(Errc::InvalidData, "sample rate of 0");
    if (info.blockCount == 0 || info.blockSize == 0 || info.blockSamples == 0)
        return fail(Errc::InvalidData, "no blocks");
    if (bytesFor(info.encoding, info.blockSamples) > info.blockSize)
        return fail(Errc::InvalidData, "blocks smaller than their samples");
    if (info.lastBlockSamples == 0 || info.lastBlockSamples > info.blockSamples ||
        bytesFor(info.encoding, info.lastBlockSamples) > info.lastBlockSize || info.lastBlockSize > info.blockSize ||
        info.lastBlockSize > info.lastBlockPaddedSize)
        return fail(Errc::InvalidData, "bad last block");

    // Every block but the last takes blockSize per channel; the last block's channels are lastBlockPaddedSize apart,
    // and the last channel's padding may be left out at the end of the file.
    const uint64_t dataEnd = data.offset + data.size;
    if (samples > dataEnd)
        return fail(Errc::InvalidData, "samples past the DATA block");
    const uint64_t room = dataEnd - samples;
    const uint64_t blockBytes = uint64_t(info.blockSize) * info.channels;
    if (info.blockCount - 1 > room / blockBytes)
        return fail(Errc::InvalidData, "more blocks than the DATA block holds");
    const uint64_t needed = uint64_t(info.blockCount - 1) * blockBytes +
                            uint64_t(info.channels - 1) * info.lastBlockPaddedSize + info.lastBlockSize;
    if (needed > room)
        return fail(Errc::InvalidData, "more samples than the DATA block holds");

    const uint64_t length = uint64_t(info.blockCount - 1) * info.blockSamples + info.lastBlockSamples;
    if (info.loopEnd == 0 || info.loopEnd > length)
        return fail(Errc::InvalidData, "music ends outside its samples");
    if (info.looping && info.loopStart >= info.loopEnd)
        return fail(Errc::InvalidData, "loop starts after it ends");
    return {};
}

} // namespace

Expected<Info> parse(ByteSpan header)
{
    if (!startsWith(header, "CSTM"))
        return fail(Errc::InvalidData, "not a BCSTM");
    if (header.size() < kFileHeaderSize)
        return fail(Errc::InvalidData, "file header cut short");
    const uint16_t byteOrder = readU16(header.data() + 0x04);
    if (byteOrder == kBigEndian)
        return fail(Errc::Unsupported, "big endian BCSTM");
    if (byteOrder != kLittleEndian)
        return fail(Errc::InvalidData, "byte order mark " + hex(byteOrder));
    const uint64_t fileSize = readU32(header.data() + 0x0C);

    // The INFO block, which must be here whole.
    const Block infoBlock = TZ_TRY(findBlock(header, kInfoBlock, "INFO"));
    if (!fits(header, infoBlock.offset, infoBlock.size))
        return fail(Errc::InvalidData, "INFO block out of bounds");
    const ByteSpan info = header.subspan(static_cast<size_t>(infoBlock.offset), static_cast<size_t>(infoBlock.size));

    Info result;
    const uint64_t stream = TZ_TRY(follow(info, 0x08, 0x08, kStreamInfo, kStreamInfoSize, "stream info"));
    const Reference samples = TZ_TRY(readStreamInfo(info.data() + stream, result));
    TZ_CHECK(checkTracks(info));

    const uint64_t channels = TZ_TRY(follow(info, 0x18, 0x08, kReferenceTable, 4, "channel info table"));
    const uint32_t channelCount = readU32(info.data() + channels);
    if (channelCount < result.channels || !fits(info, channels + 4, uint64_t(channelCount) * kReferenceSize))
        return fail(Errc::InvalidData, "bad channel info table");
    for (size_t c = 0; c < result.channels; ++c)
    {
        const uint64_t channel = TZ_TRY(follow(info, channels + 4 + c * kReferenceSize, channels, kChannelInfo,
                                               kReferenceSize, "channel info"));
        if (result.encoding == Encoding::DspAdpcm)
        {
            const uint64_t adpcm =
                TZ_TRY(follow(info, channel, channel, kDspAdpcmInfo, kAdpcmInfoSize, "DSP-ADPCM info"));
            readAdpcm(info.data() + adpcm, result.adpcm[c]);
        }
    }

    // The DATA block, of which only the header needs to be here.
    const Block dataBlock = TZ_TRY(findBlock(header, kDataBlock, "DATA"));
    if (dataBlock.offset + dataBlock.size > fileSize)
        return fail(Errc::InvalidData, "DATA block past the end of the file");
    if (samples.offset == kNone || samples.type != kSampleData)
        return fail(Errc::InvalidData, "no sample data reference");
    const uint64_t samplesOffset = dataBlock.offset + kBlockHeaderSize + samples.offset;
    TZ_CHECK(checkLayout(result, samplesOffset, dataBlock));
    // within the file's u32 size, as checkLayout found the samples inside the DATA block
    result.dataOffset = static_cast<uint32_t>(samplesOffset);
    return result;
}

} // namespace tz::bcstm
