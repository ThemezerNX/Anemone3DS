// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// LZ11, Nintendo's LZ77 variant with the type byte 0x11. After the header (0x11 and the decompressed size in 24 bits,
// little endian; when those are 0 the size follows in 32 bits) come groups of a flag byte and up to eight items, the
// most significant flag bit first. A clear bit is a literal byte. A set bit is a copy of `length` bytes from
// `distance` (1..4096) bytes back in the output, in one of three encodings told apart by the high nibble of its first
// byte, where D is distance - 1 in 12 bits:
//
//   nibble 2..15   NL LD DD              length = N + 1               3..16
//   nibble 0       0L LD DD              length = L + 0x11            17..272     (8 bits of L)
//   nibble 1       1L LL LD DD           length = L + 0x111           273..65808  (16 bits of L)

#include "core/lz11.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace tz::lz11
{

namespace
{

constexpr uint8_t kType = 0x11;
constexpr uint32_t kMaxDistance = 0x1000;
constexpr uint32_t kMinLength = 3;
constexpr uint32_t kMaxLength = 0x10110;
// The most output a byte of input can stand for: eight longest copies in 33 bytes. A stated size beyond that is a
// corrupt header, not a reason to allocate.
constexpr uint64_t kMaxExpansion = 8 * kMaxLength / 33 + 1;

struct Header
{
    uint32_t size = 0;
    size_t dataStart = 0;
};

bool readHeader(ByteSpan data, Header& header)
{
    if (data.size() < 4 || data[0] != kType)
        return false;
    header.size = static_cast<uint32_t>(data[1] | (data[2] << 8) | (data[3] << 16));
    header.dataStart = 4;
    if (header.size == 0)
    {
        if (data.size() < 8)
            return false;
        header.size = readU32(data.data() + 4);
        header.dataStart = 8;
    }
    return true;
}

// What each item costs in bits, the flag bit included; all the parser below weighs.
constexpr uint32_t kLiteralCost = 9;

constexpr uint32_t copyCost(uint32_t length)
{
    return length <= 0x10 ? 17 : length <= 0x110 ? 25 : 33;
}

// Writes the items, into a buffer sized for the worst case so that nothing is reallocated.
class Output
{
public:
    explicit Output(size_t inputSize) : data_(8 + inputSize + inputSize / 8 + 1)
    {
        data_[0] = kType;
        if (inputSize != 0 && inputSize <= 0xFFFFFF)
        {
            data_[1] = static_cast<uint8_t>(inputSize);
            data_[2] = static_cast<uint8_t>(inputSize >> 8);
            data_[3] = static_cast<uint8_t>(inputSize >> 16);
            size_ = 4;
        }
        else
        {
            // a 24-bit size of 0 announces the 32-bit one (and is the only way to say "empty")
            data_[1] = data_[2] = data_[3] = 0;
            writeU32(data_.data() + 4, static_cast<uint32_t>(inputSize));
            size_ = 8;
        }
    }

    void literal(uint8_t value)
    {
        startItem();
        data_[size_++] = value;
    }

    void copy(uint32_t length, uint32_t distance)
    {
        startItem();
        data_[flags_] |= static_cast<uint8_t>(0x80 >> (count_ - 1));
        const uint32_t d = distance - 1;
        if (length <= 0x10)
        {
            data_[size_++] = static_cast<uint8_t>(((length - 1) << 4) | (d >> 8));
        }
        else if (length <= 0x110)
        {
            const uint32_t l = length - 0x11;
            data_[size_++] = static_cast<uint8_t>(l >> 4);
            data_[size_++] = static_cast<uint8_t>(((l & 0xF) << 4) | (d >> 8));
        }
        else
        {
            const uint32_t l = length - 0x111;
            data_[size_++] = static_cast<uint8_t>(0x10 | (l >> 12));
            data_[size_++] = static_cast<uint8_t>(l >> 4);
            data_[size_++] = static_cast<uint8_t>(((l & 0xF) << 4) | (d >> 8));
        }
        data_[size_++] = static_cast<uint8_t>(d);
    }

    Bytes finish()
    {
        data_.resize(size_);
        return std::move(data_);
    }

private:
    void startItem()
    {
        if (count_ == 8)
        {
            flags_ = size_;
            data_[size_++] = 0;
            count_ = 0;
        }
        ++count_;
    }

    Bytes data_;
    size_t size_ = 0;
    size_t flags_ = 0;
    int count_ = 8;
};

// Finds the longest earlier match at every position. The positions whose first three bytes hash alike form a binary
// search tree ordered by the bytes from there on, the newest at the root, as in LZMA's binary-tree match finders:
// inserting a position walks down from the root and is at the same time the search for its longest match, splitting
// the old tree into what sorts before and after it. A comparison starts where both bounds of the subtree already
// agree with the new position, so repetitive data costs little, and the walk stops after kDepth nodes.
//
// Hash chains (newest first) are cheaper per position but miss the longest match in textures, where thousands of
// positions start with the same pixel; on theme bodies that cost several percent against the tools themes are made
// with, and a body that fitted the theme cache has to fit again after the app changes it.
class MatchTree
{
public:
    explicit MatchTree(ByteSpan data)
        : data_(data.data()), size_(data.size()), heads_(size_t{1} << kHashBits, -1), children_(2 * kSlots, -1)
    {
    }

    // Adds position p (positions are added in increasing order, gaps allowed) and returns the length of its longest
    // match, up to limit bytes, with its distance; 0 when there is none of kMinLength.
    uint32_t insert(size_t p, uint32_t limit, uint32_t& distance)
    {
        if (p + kMinLength > size_)
            return 0;
        limit = static_cast<uint32_t>(std::min<size_t>(limit, size_ - p));

        const uint8_t* const current = data_ + p;
        const uint32_t prefix = static_cast<uint32_t>(current[0]) << 16 | current[1] << 8 | current[2];
        const uint32_t hash = (prefix * 2654435761u) >> (32 - kHashBits);
        int32_t node = heads_[hash];
        heads_[hash] = static_cast<int32_t>(p);

        // where the next node found to sort before (smaller) or after (larger) the new position gets attached, and
        // how many bytes the last of each agreed with it
        int32_t* smaller = &children_[2 * (p & (kSlots - 1))];
        int32_t* larger = smaller + 1;
        uint32_t smallerAgree = 0;
        uint32_t largerAgree = 0;
        uint32_t best = 0;

        for (int depth = kDepth;; --depth)
        {
            if (node < 0 || p - static_cast<size_t>(node) > kMaxDistance || depth == 0)
            {
                *smaller = -1;
                *larger = -1;
                break;
            }
            int32_t* const pair = &children_[2 * (static_cast<size_t>(node) & (kSlots - 1))];
            const uint8_t* const candidate = data_ + node;
            uint32_t length = std::min(smallerAgree, largerAgree);
            if (candidate[length] == current[length])
            {
                while (++length < limit && candidate[length] == current[length])
                {
                }
                if (length > best)
                {
                    best = length;
                    distance = static_cast<uint32_t>(p - static_cast<size_t>(node));
                }
                if (length >= limit)
                {
                    // as far as the limit lets us see they're equal: the new position takes the old one's place
                    *smaller = pair[0];
                    *larger = pair[1];
                    break;
                }
            }
            if (candidate[length] < current[length])
            {
                *smaller = node;
                smaller = pair + 1;
                node = *smaller;
                smallerAgree = length;
            }
            else
            {
                *larger = node;
                larger = pair;
                node = *larger;
                largerAgree = length;
            }
        }
        return best >= kMinLength ? best : 0;
    }

private:
    // 16 KiB of heads, the size of the 3DS's data cache; more buckets compress no better
    static constexpr int kHashBits = 12;
    static constexpr int kDepth = 16;
    // Twice the window: a node exactly kMaxDistance back must not share the slot of the position being inserted.
    static constexpr size_t kSlots = 2 * kMaxDistance;

    const uint8_t* data_;
    size_t size_;
    std::vector<int32_t> heads_;    // the newest position per hash, -1 for none
    std::vector<int32_t> children_; // per slot: the subtrees that sort before and after that position
};

// A match this long is taken as it is: its cost no longer grows with its length, and looking for a better way through
// the positions it covers costs more time than it can save.
constexpr uint32_t kLongMatch = 0x111;
// After a long match, its last positions still go into the tree, so that the data following it can refer back.
constexpr uint32_t kLongMatchTail = 64;
// Positions parsed together. Longer blocks find marginally better boundaries between items.
constexpr size_t kBlock = 0x1000;

// Picks the cheapest items for one block: the cost of the rest of the block is known for every later position, so
// each position compares a literal with copies of every useful length, back to front. Shorter copies than the longest
// can win by ending where a better match starts; past 18 bytes only the longest of each cost class is worth trying.
void parseBlock(ByteSpan data, size_t start, size_t count, const std::vector<uint32_t>& lengths,
                const std::vector<uint16_t>& distances, std::vector<uint32_t>& cost, std::vector<uint32_t>& choice,
                Output& out)
{
    cost[count] = 0;
    for (size_t i = count; i-- > 0;)
    {
        uint32_t best = kLiteralCost + cost[i + 1];
        uint32_t bestLength = 1;
        const uint32_t length = std::min<uint32_t>(lengths[i], static_cast<uint32_t>(count - i));
        if (length >= kMinLength)
        {
            const uint32_t shortTop = std::min<uint32_t>(length, 18);
            for (uint32_t l = kMinLength; l <= shortTop; ++l)
            {
                const uint32_t c = copyCost(l) + cost[i + l];
                if (c < best)
                {
                    best = c;
                    bestLength = l;
                }
            }
            for (const uint32_t l : {std::min<uint32_t>(length, 0x110), length})
            {
                const uint32_t c = copyCost(l) + cost[i + l];
                if (c < best)
                {
                    best = c;
                    bestLength = l;
                }
            }
        }
        cost[i] = best;
        choice[i] = bestLength;
    }

    for (size_t i = 0; i < count;)
    {
        if (choice[i] == 1)
        {
            out.literal(data[start + i]);
            ++i;
        }
        else
        {
            out.copy(choice[i], distances[i]);
            i += choice[i];
        }
    }
}

} // namespace

uint32_t decompressedSize(ByteSpan data)
{
    Header header;
    return readHeader(data, header) ? header.size : 0;
}

Expected<Bytes> decompress(ByteSpan data, size_t maxSize)
{
    Header header;
    if (!readHeader(data, header))
        return fail(Errc::InvalidData, "not LZ11");
    if (header.size > maxSize)
        return fail(Errc::TooLarge, "LZ11 data of " + std::to_string(header.size) + " bytes");
    const size_t inputSize = data.size();
    if (header.size > (inputSize - header.dataStart) * kMaxExpansion)
        return fail(Errc::InvalidData, "LZ11 size larger than its data can hold");

    Bytes out(header.size);
    uint8_t* const output = out.data();
    const size_t outputSize = out.size();
    size_t written = 0;
    size_t in = header.dataStart;

    while (written < outputSize)
    {
        if (in >= inputSize)
            return fail(Errc::InvalidData, "LZ11 data ends early");
        const uint8_t flags = data[in++];
        for (int bit = 7; bit >= 0 && written < outputSize; --bit)
        {
            if (!(flags & (1 << bit)))
            {
                if (in >= inputSize)
                    return fail(Errc::InvalidData, "LZ11 data ends early");
                output[written++] = data[in++];
                continue;
            }

            if (in + 2 > inputSize)
                return fail(Errc::InvalidData, "LZ11 data ends early");
            const uint8_t first = data[in];
            uint32_t length;
            uint32_t distance;
            switch (first >> 4)
            {
            case 0:
                if (in + 3 > inputSize)
                    return fail(Errc::InvalidData, "LZ11 data ends early");
                length = (((first & 0xFu) << 4) | (data[in + 1] >> 4)) + 0x11;
                distance = (((data[in + 1] & 0xFu) << 8) | data[in + 2]) + 1;
                in += 3;
                break;
            case 1:
                if (in + 4 > inputSize)
                    return fail(Errc::InvalidData, "LZ11 data ends early");
                length = (((first & 0xFu) << 12) | (static_cast<uint32_t>(data[in + 1]) << 4) | (data[in + 2] >> 4)) +
                         0x111;
                distance = (((data[in + 2] & 0xFu) << 8) | data[in + 3]) + 1;
                in += 4;
                break;
            default:
                length = (first >> 4) + 1u;
                distance = (((first & 0xFu) << 8) | data[in + 1]) + 1;
                in += 2;
                break;
            }

            if (distance > written)
                return fail(Errc::InvalidData, "LZ11 copy from before the start");
            // a copy running past the stated size is cut there, as the console's own decoder stops at the size
            length = static_cast<uint32_t>(std::min<size_t>(length, outputSize - written));
            const uint8_t* from = output + written - distance;
            uint8_t* to = output + written;
            if (distance >= length)
            {
                std::memcpy(to, from, length);
            }
            else
            {
                // overlapping: a copy of what this copy itself writes, byte by byte
                for (uint32_t i = 0; i < length; ++i)
                    to[i] = from[i];
            }
            written += length;
        }
    }
    return out;
}

Bytes compress(ByteSpan data)
{
    const size_t size = data.size();
    Output out(size);
    MatchTree tree(data);
    std::vector<uint32_t> lengths(kBlock);
    std::vector<uint16_t> distances(kBlock);
    std::vector<uint32_t> cost(kBlock + 1);
    std::vector<uint32_t> choice(kBlock + 1);

    size_t p = 0;
    while (p < size)
    {
        // find the matches of a block of positions, up to the first long one
        const size_t start = p;
        uint32_t longLength = 0;
        uint32_t longDistance = 0;
        while (p < size && p - start < kBlock)
        {
            uint32_t distance = 0;
            uint32_t length = tree.insert(p, kLongMatch, distance);
            if (length >= kLongMatch)
            {
                const size_t limit = std::min<size_t>(kMaxLength, size - p);
                while (length < limit && data[p + length] == data[p + length - distance])
                    ++length;
                longLength = length;
                longDistance = distance;
                break;
            }
            lengths[p - start] = length;
            distances[p - start] = static_cast<uint16_t>(distance);
            ++p;
        }

        parseBlock(data, start, p - start, lengths, distances, cost, choice, out);

        if (longLength != 0)
        {
            out.copy(longLength, longDistance);
            const size_t end = p + longLength;
            uint32_t ignored;
            for (size_t q = std::max<size_t>(p + 1, end > kLongMatchTail ? end - kLongMatchTail : 0); q < end; ++q)
                tree.insert(q, kLongMatch, ignored);
            p = end;
        }
    }
    return out.finish();
}

} // namespace tz::lz11
