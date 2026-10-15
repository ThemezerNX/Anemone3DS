// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The HOME Menu's badge files, as documented on 3dbrew ("Home Menu") and used by GYTB (MrCheeze) and Advanced Badge
// Editor (AntiMach). Both files have fixed sizes; everything is little endian.
//
// BadgeData.dat, names and pictures by slot:
//   0x000000  set names: 100 sets x 16 languages x 0x8A bytes of UTF-16, NUL padded
//   0x035E80  badge names: 1000 badges x 16 languages x 0x8A bytes
//   0x250F80  set icons: 100 x 0x2000, a 64x64 tiled RGB565 texture with the 48x48 icon in its top left corner
//   0x318F80  badges: 1000 x 0x2800, 64x64 tiled RGB565 (0x2000 bytes) then its alpha (0x800)
//   0xCDCF80  small badges: 1000 x 0xA00, the same at 32x32 (0x800 + 0x200)
// Alpha has 4 bits per pixel, two pixels a byte in the texture's tile order, the even one in the low nibble.
//
// BadgeMngFile.dat, what there is:
//   0x0     u32 0, which the HOME Menu requires
//   0x4     u32 number of sets
//   0x8     u32 number of badges
//   0xC     u32 number of badges placed on the HOME Menu
//   0x10    u32 the set the badge case shows, 0xFFFFFFFF for all
//   0x14    u32 the column the badge case shows
//   0x18    u32 how many badges there are in all (the sum of their quantities)
//   0x1C    u32 the principal ID of the console's Nintendo Network ID
//   0x358   one bit per badge slot in use
//   0x3D8   one bit per set slot in use
//   0x3E8   1000 badges of 0x28 bytes: +0x4 u32 badge ID, +0x8 u32 set ID, +0xC u16 slot of its names and pictures,
//           +0xE u16 part of a larger badge (0 for a whole one), +0x10 u16 number placed, +0x12 u16 quantity,
//           +0x18 u64 the title ID of the app it opens (0xFFFFFFFFFFFFFFFF for none), repeated at +0x20
//   0xA028  100 sets of 0x30 bytes: +0x0 8 x 0xFF, +0xC u32 0x2710, +0x10 u32 set ID, +0x14 u32 slot of its name and
//           icon, +0x18 u32 0xFFFFFFFF, +0x1C u32 number of badges, +0x20 u32 their total quantity, +0x24 u32 slot of
//           its first badge
//   0xB2E8  360 badges placed on the HOME Menu, 0x18 bytes each, which the HOME Menu manages

#include "domain/badges.hpp"

#include "core/path.hpp"
#include "core/utf.hpp"
#include "core/zip.hpp"
#include "domain/library.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace tz::badges
{

namespace
{

constexpr const char* kDataPath = "/BadgeData.dat";
constexpr const char* kManagerPath = "/BadgeMngFile.dat";
constexpr const char* kDataName = "BadgeData.dat";
constexpr const char* kManagerName = "BadgeMngFile.dat";
// a badge exported with this name would come back as its set's icon
constexpr std::string_view kIconStem = "_seticon";

constexpr int kLanguages = 16;
constexpr int kEnglish = 1;
constexpr size_t kNameSize = 0x8A;
// what a name can take, keeping its last unit for the terminator
constexpr size_t kNameUnits = kNameSize / 2 - 1;
// one name in every language: a set's or a badge's
constexpr size_t kNamesSize = kLanguages * kNameSize;

constexpr uint32_t kSetNamesOffset = 0;
constexpr uint32_t kBadgeNamesOffset = 0x35E80;
constexpr uint32_t kSetIconsOffset = 0x250F80;
constexpr size_t kSetIconSize = 0x2000;
constexpr uint32_t kLargeOffset = 0x318F80;
constexpr size_t kLargeSize = 0x2800;
constexpr uint32_t kSmallOffset = 0xCDCF80;
constexpr size_t kSmallSize = 0xA00;

constexpr int kBadgeSide = 64;
constexpr int kSmallSide = 32;
constexpr int kIconSide = 48;
constexpr int kIconTexture = 64;
constexpr uint32_t kMaxSheetColumns = 12;
constexpr uint32_t kMaxSheetRows = 6;

constexpr size_t kSetCountOffset = 0x4;
constexpr size_t kBadgeCountOffset = 0x8;
constexpr size_t kSelectedSetOffset = 0x10;
constexpr size_t kTotalOffset = 0x18;
constexpr size_t kPrincipalIdOffset = 0x1C;
constexpr size_t kBadgeBitsOffset = 0x358;
constexpr size_t kSetBitsOffset = 0x3D8;
constexpr size_t kBadgesOffset = 0x3E8;
constexpr size_t kBadgeRecordSize = 0x28;
constexpr size_t kSetsOffset = 0xA028;
constexpr size_t kSetRecordSize = 0x30;
constexpr size_t kPlacedOffset = 0xB2E8;

constexpr uint32_t kAllSets = 0xFFFFFFFF;
// every badge installed is had as many times as the HOME Menu allows
constexpr uint16_t kQuantity = 0xFFFF;
constexpr uint64_t kNoShortcut = 0xFFFFFFFFFFFFFFFF;
// the high half of the title IDs of system applications, which a badge can open
constexpr uint64_t kSystemApplications = 0x0004001000000000;

constexpr uint64_t kMaxPictureSize = 8u << 20;
// badges collected before their names and pictures are written, three writes for all of them
constexpr int kBatch = 32;
constexpr size_t kCopyChunk = 0x10000;
constexpr size_t kZeroChunk = 0x20000;

// ---- names

// "Mario.00021700.png" is a badge named "Mario" that opens the system application 0004001000021700, GYTB's
// convention: the name ends at the first dot, and eight hex digits between the first two dots are the low half of
// the title ID.
void parseFileName(std::string_view fileName, std::string& name, uint64_t& shortcut)
{
    const size_t dot = fileName.find('.');
    name = std::string(fileName.substr(0, dot));
    shortcut = kNoShortcut;
    if (dot == std::string_view::npos)
        return;
    const std::string_view rest = fileName.substr(dot + 1);
    if (rest.find('.') != 8)
        return;
    uint32_t low = 0;
    for (const char c : rest.substr(0, 8))
    {
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<uint32_t>(c - 'A' + 10);
        else
            return;
        low = low << 4 | digit;
    }
    shortcut = kSystemApplications | low;
}

// A name in all 16 languages.
Bytes nameSlots(std::string_view name)
{
    Bytes slots(kNamesSize, 0);
    utf::toUtf16Le(name, slots.data(), kNameUnits);
    for (int language = 1; language < kLanguages; ++language)
        std::memcpy(&slots[language * kNameSize], slots.data(), kNameSize);
    return slots;
}

// The English name, or the first one there is.
std::string pickName(const uint8_t* slots)
{
    std::string name = utf::fromUtf16Le(slots + kEnglish * kNameSize, kNameSize / 2);
    for (int language = 0; language < kLanguages && name.empty(); ++language)
        name = utf::fromUtf16Le(slots + language * kNameSize, kNameSize / 2);
    return name;
}

bool hiddenName(std::string_view name)
{
    return name.empty() || name.front() == '.' || path::equalsIgnoreCase(name, "__MACOSX");
}

std::string lowered(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

// ---- pictures

// Gives transparent pixels the colour of their visible neighbours. Where the HOME Menu filters a badge it draws scaled,
// the colour under a transparent pixel (usually black) would otherwise darken the edges next to it. Advanced Badge
// Editor does the same.
void bleed(Image& image)
{
    const int width = image.width;
    const int height = image.height;
    std::vector<uint8_t> known(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (size_t i = 0; i < known.size(); ++i)
        known[i] = image.pixels[i * 4 + 3] != 0;
    std::vector<uint8_t> next;
    // filtering only reaches the next pixel; two rings are plenty
    for (int pass = 0; pass < 2; ++pass)
    {
        next = known;
        bool changed = false;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                if (known[index])
                    continue;
                int sums[3] = {};
                int count = 0;
                const int neighbours[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& [nx, ny] : neighbours)
                {
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height ||
                        !known[static_cast<size_t>(ny) * static_cast<size_t>(width) + static_cast<size_t>(nx)])
                        continue;
                    const uint8_t* pixel = image.at(nx, ny);
                    sums[0] += pixel[0];
                    sums[1] += pixel[1];
                    sums[2] += pixel[2];
                    ++count;
                }
                if (count == 0)
                    continue;
                uint8_t* pixel = image.at(x, y);
                for (int c = 0; c < 3; ++c)
                    pixel[c] = static_cast<uint8_t>(sums[c] / count);
                next[index] = 1;
                changed = true;
            }
        }
        known.swap(next);
        if (!changed)
            break;
    }
}

// A square picture as the HOME Menu stores it: tiled RGB565, then tiled 4-bit alpha.
void encodePixels(const Image& image, int side, uint8_t* out)
{
    const size_t pixels = static_cast<size_t>(side) * static_cast<size_t>(side);
    uint8_t* alpha = out + pixels * 2;
    std::memset(alpha, 0, pixels / 2);
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            const uint8_t* pixel = image.at(x, y);
            const uint32_t index =
                tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(side));
            writeU16(out + index * 2, toRgb565(pixel[0], pixel[1], pixel[2]));
            alpha[index / 2] |= static_cast<uint8_t>((pixel[3] >> 4) << ((index & 1) * 4));
        }
    }
}

uint8_t widen5(uint16_t value)
{
    return static_cast<uint8_t>(value << 3 | value >> 2);
}

uint8_t widen6(uint16_t value)
{
    return static_cast<uint8_t>(value << 2 | value >> 4);
}

void rgb565ToPixel(uint16_t value, uint8_t* pixel)
{
    pixel[0] = widen5((value >> 11) & 0x1F);
    pixel[1] = widen6((value >> 5) & 0x3F);
    pixel[2] = widen5(value & 0x1F);
}

Image decodePixels(const uint8_t* in, int side)
{
    Image image(side, side);
    const uint8_t* alpha = in + static_cast<size_t>(side) * static_cast<size_t>(side) * 2;
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            const uint32_t index =
                tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(side));
            uint8_t* pixel = image.at(x, y);
            rgb565ToPixel(readU16(in + index * 2), pixel);
            pixel[3] = static_cast<uint8_t>(((alpha[index / 2] >> ((index & 1) * 4)) & 0xF) * 0x11);
        }
    }
    return image;
}

// A set icon goes in the top left of a 64x64 texture without alpha. What is transparent goes black, and the edge
// pixels are repeated over the rest so that filtering at the icon's edges finds the same colours.
void encodeSetIcon(const Image& icon, uint8_t* out)
{
    for (int y = 0; y < kIconTexture; ++y)
    {
        for (int x = 0; x < kIconTexture; ++x)
        {
            const uint8_t* pixel = icon.at(std::min(x, kIconSide - 1), std::min(y, kIconSide - 1));
            const unsigned a = pixel[3];
            const uint32_t index =
                tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(kIconTexture));
            writeU16(out + index * 2,
                     toRgb565(static_cast<uint8_t>(pixel[0] * a / 255), static_cast<uint8_t>(pixel[1] * a / 255),
                              static_cast<uint8_t>(pixel[2] * a / 255)));
        }
    }
}

Image decodeSetIcon(const uint8_t* in)
{
    Image icon(kIconSide, kIconSide);
    for (int y = 0; y < kIconSide; ++y)
    {
        for (int x = 0; x < kIconSide; ++x)
        {
            const uint32_t index =
                tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(kIconTexture));
            uint8_t* pixel = icon.at(x, y);
            rgb565ToPixel(readU16(in + index * 2), pixel);
            pixel[3] = 0xFF;
        }
    }
    return icon;
}

// The size in a PNG's header, which comes first: the IHDR chunk's length and type at 8, then the width and height
// (big endian).
bool pngSize(ByteSpan png, uint32_t& width, uint32_t& height)
{
    if (png.size() < 24 || std::memcmp(png.data() + 12, "IHDR", 4) != 0)
        return false;
    auto bigEndian = [&](size_t at) {
        return static_cast<uint32_t>(png[at]) << 24 | static_cast<uint32_t>(png[at + 1]) << 16 |
               static_cast<uint32_t>(png[at + 2]) << 8 | png[at + 3];
    };
    width = bigEndian(16);
    height = bigEndian(20);
    return true;
}

// A badge picture is 64x64, or a sheet of up to 12x6 of them; anything else isn't one.
Expected<Image> decodeSheet(ByteSpan png)
{
    uint32_t width;
    uint32_t height;
    if (!pngSize(png, width, height))
        return fail(Errc::InvalidData, "not a PNG");
    if (width == 0 || height == 0 || width % kBadgeSide != 0 || height % kBadgeSide != 0 ||
        width > kMaxSheetColumns * kBadgeSide || height > kMaxSheetRows * kBadgeSide)
        return fail(Errc::Unsupported, "not a badge size: " + std::to_string(width) + "x" + std::to_string(height));
    Image sheet = TZ_TRY(decodePng(png, static_cast<int>(width), static_cast<int>(height)));
    if (sheet.width != static_cast<int>(width) || sheet.height != static_cast<int>(height))
        return fail(Errc::InvalidData, "PNG size changed while decoding");
    return sheet;
}

// The badges of a sheet in the order they are installed: a column at a time, each from the top, as GYTB cuts them.
template <class Visit>
bool forEachBadge(const Image& sheet, Visit&& visit)
{
    for (int x = 0; x < sheet.width; x += kBadgeSide)
        for (int y = 0; y < sheet.height; y += kBadgeSide)
            if (!visit(crop(sheet, x, y, kBadgeSide, kBadgeSide)))
                return false;
    return true;
}

Image toIcon(Image image)
{
    if (image.width != kIconSide || image.height != kIconSide)
        image = resize(image, kIconSide, kIconSide);
    return image;
}

Expected<Image> decodeIcon(ByteSpan png)
{
    return toIcon(TZ_TRY(decodePng(png, kIconSide, kIconSide)));
}

// ---- a set's files

struct PictureFile
{
    std::string name; // the file's name, which names its badges
    std::string path; // on the SD card, or of the zip it is in
    int zip = -1;     // which of the set's zips it is in, if it is in one
    size_t entry = 0;
};

// What a set has, in the order it is installed: the PNGs and zips in its folder by name (a zip's PNGs by their path
// in it), then the PNGs in its zip. The icon is the folder's _seticon.png, else the first in a zip.
class SetFiles
{
public:
    std::vector<PictureFile> pictures;
    std::optional<PictureFile> icon;
    int broken = 0; // zips that couldn't be read

    static Expected<SetFiles> open(FileSystem& sd, const SetSource& set)
    {
        SetFiles files;
        if (set.loose())
        {
            for (const std::string& file : set.loosePaths)
                files.pictures.push_back({std::string(path::filename(file)), file});
            const std::string icon = path::join(path::parent(set.loosePaths.front()), kSetIconName);
            if (sd.exists(icon))
                files.icon = PictureFile{kSetIconName, icon};
            return files;
        }

        std::optional<PictureFile> folderIcon;
        if (!set.folderPath.empty())
        {
            auto listing = sd.list(set.folderPath);
            if (!listing && listing.error().code != Errc::NotFound)
                return fail(listing.error());
            if (listing)
            {
                std::vector<DirEntry> entries = std::move(*listing);
                std::erase_if(entries, [](const DirEntry& e) { return e.isDirectory || hiddenName(e.name); });
                // (through pointers: GCC notes an ABI change for DirEntry's vector iterators on ARM)
                std::sort(entries.data(), entries.data() + entries.size(), [](const DirEntry& a, const DirEntry& b) {
                    return path::compareIgnoreCase(a.name, b.name) < 0;
                });
                for (const DirEntry& entry : entries)
                {
                    std::string file = path::join(set.folderPath, entry.name);
                    if (path::equalsIgnoreCase(entry.name, kSetIconName))
                        folderIcon = PictureFile{entry.name, std::move(file)};
                    else if (path::hasExtension(entry.name, "png"))
                        files.pictures.push_back({entry.name, std::move(file)});
                    else if (path::hasExtension(entry.name, "zip"))
                        files.addZip(sd, file);
                }
            }
        }
        if (!set.zipPath.empty())
            files.addZip(sd, set.zipPath);
        if (folderIcon)
            files.icon = std::move(folderIcon);
        return files;
    }

    Expected<Bytes> read(FileSystem& sd, const PictureFile& file) const
    {
        if (file.zip < 0)
            return sd.readFile(file.path, kMaxPictureSize);
        const ZipArchive& zip = zips_[static_cast<size_t>(file.zip)];
        return zip.read(zip.entries()[file.entry], kMaxPictureSize);
    }

private:
    void addZip(FileSystem& sd, const std::string& zipPath)
    {
        auto file = sd.open(zipPath, OpenMode::Read);
        if (!file)
        {
            ++broken;
            return;
        }
        auto zip = ZipArchive::open(std::shared_ptr<Reader>(std::move(*file)));
        if (!zip)
        {
            ++broken;
            return;
        }

        const std::vector<ZipEntry>& entries = zip->entries();
        std::vector<size_t> pngs;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const ZipEntry& entry = entries[i];
            const std::string_view name = path::filename(entry.name);
            if (entry.isDirectory() || hiddenName(name) || !path::hasExtension(name, "png") ||
                path::equalsIgnoreCase(name, kSetIconName))
                continue;
            const std::vector<std::string_view> parts = path::split(entry.name);
            if (std::any_of(parts.begin(), parts.end(), [](std::string_view part) { return hiddenName(part); }))
                continue;
            pngs.push_back(i);
        }
        std::sort(pngs.begin(), pngs.end(), [&](size_t a, size_t b) {
            return path::compareIgnoreCase(entries[a].name, entries[b].name) < 0;
        });

        const int index = static_cast<int>(zips_.size());
        for (const size_t i : pngs)
            pictures.push_back({std::string(path::filename(entries[i].name)), zipPath, index, i});
        if (!icon)
        {
            if (const ZipEntry* entry = zip->find(kSetIconName))
                icon = PictureFile{kSetIconName, zipPath, index, static_cast<size_t>(entry - entries.data())};
        }
        zips_.push_back(std::move(*zip));
    }

    std::vector<ZipArchive> zips_;
};

// ---- BadgeData.dat

// Collects the names and pictures of consecutive badges and writes them in three writes: the HOME Menu's storage is
// slow to write to a little at a time.
class BadgeWriter
{
public:
    explicit BadgeWriter(File& file)
        : file_(file), names_(kBatch * kNamesSize), large_(kBatch * kLargeSize), small_(kBatch * kSmallSize)
    {
    }

    // Badges come in slot order.
    Expected<void> add(int slot, const Bytes& names, Image badge)
    {
        if (count_ == kBatch)
            TZ_CHECK(flush());
        if (count_ == 0)
            first_ = slot;
        const auto at = static_cast<size_t>(count_);
        std::memcpy(&names_[at * kNamesSize], names.data(), kNamesSize);
        Image small = resize(badge, kSmallSide, kSmallSide);
        bleed(badge);
        encodePixels(badge, kBadgeSide, &large_[at * kLargeSize]);
        bleed(small);
        encodePixels(small, kSmallSide, &small_[at * kSmallSize]);
        ++count_;
        return {};
    }

    Expected<void> flush()
    {
        if (count_ == 0)
            return {};
        // a write that fails may have changed the file all the same
        wrote_ = true;
        const auto first = static_cast<uint64_t>(first_);
        const auto count = static_cast<size_t>(count_);
        TZ_CHECK(file_.writeAt(kBadgeNamesOffset + first * kNamesSize, ByteSpan(names_).first(count * kNamesSize)));
        TZ_CHECK(file_.writeAt(kLargeOffset + first * kLargeSize, ByteSpan(large_).first(count * kLargeSize)));
        TZ_CHECK(file_.writeAt(kSmallOffset + first * kSmallSize, ByteSpan(small_).first(count * kSmallSize)));
        count_ = 0;
        return {};
    }

    bool wrote() const { return wrote_; }

private:
    File& file_;
    int first_ = 0;
    int count_ = 0;
    bool wrote_ = false;
    Bytes names_;
    Bytes large_;
    Bytes small_;
};

// Reads the slots of BadgeData.dat a block of consecutive badges at a time, since badges are read in order.
class SlotReader
{
public:
    SlotReader(File& file, uint64_t offset, size_t size) : file_(file), offset_(offset), size_(size) {}

    Expected<const uint8_t*> get(uint32_t slot)
    {
        const uint32_t block = slot / kBlock;
        if (block != block_)
        {
            const uint32_t count = std::min<uint32_t>(kBlock, kMaxBadges - block * kBlock);
            buffer_.resize(count * size_);
            TZ_CHECK(file_.readAt(offset_ + uint64_t{block} * kBlock * size_, buffer_));
            block_ = block;
        }
        return buffer_.data() + (slot % kBlock) * size_;
    }

private:
    static constexpr uint32_t kBlock = 32;
    File& file_;
    uint64_t offset_;
    size_t size_;
    uint32_t block_ = UINT32_MAX;
    Bytes buffer_;
};

// ---- BadgeMngFile.dat

// What an unused set looks like; the constant fields of a used one are the same.
void writeEmptySet(uint8_t* record)
{
    std::memset(record, 0xFF, 8);
    std::memset(record + 0x8, 0, 4);
    writeU32(record + 0xC, 0x2710);
    std::memset(record + 0x10, 0xFF, 0xC);
    std::memset(record + 0x1C, 0, 8);
    std::memset(record + 0x24, 0xFF, 4);
    std::memset(record + 0x28, 0, 8);
}

void writeSet(uint8_t* record, uint32_t slot, uint32_t firstBadge, uint32_t badges)
{
    writeEmptySet(record);
    writeU32(record + 0x10, slot + 1); // set ID
    writeU32(record + 0x14, slot);
    writeU32(record + 0x1C, badges);
    writeU32(record + 0x20, badges * kQuantity);
    writeU32(record + 0x24, firstBadge);
}

void writeBadge(uint8_t* record, uint32_t slot, uint32_t setId, uint64_t shortcut)
{
    writeU32(record + 0x4, slot + 1); // badge ID
    writeU32(record + 0x8, setId);
    writeU16(record + 0xC, static_cast<uint16_t>(slot));
    writeU16(record + 0x12, kQuantity);
    writeU64(record + 0x18, shortcut);
    writeU64(record + 0x20, shortcut);
}

// No badges and no sets, nothing selected and nothing placed.
Bytes emptyManager(uint32_t principalId)
{
    Bytes manager(kManagerSize, 0);
    writeU32(&manager[kSelectedSetOffset], kAllSets);
    writeU32(&manager[kPrincipalIdOffset], principalId);
    for (int set = 0; set < kMaxSets; ++set)
        writeEmptySet(&manager[kSetsOffset + static_cast<size_t>(set) * kSetRecordSize]);
    return manager;
}

uint32_t readManagerU32(FileSystem& badgeExt, size_t offset)
{
    auto file = badgeExt.open(kManagerPath, OpenMode::Read);
    uint8_t value[4];
    if (!file || (*file)->size() != kManagerSize || !(*file)->readAt(offset, MutableByteSpan(value, 4)))
        return 0;
    return readU32(value);
}

// Writes all of a file. Extdata only keeps the hashes of what has been written, so a part that never was may not read
// back, and the HOME Menu reads more of these files than what is in use.
Expected<void> fill(File& file, uint64_t size, ByteSpan content)
{
    if (!content.empty())
    {
        TZ_CHECK(file.writeAt(0, content));
    }
    else
    {
        const Bytes zeros(kZeroChunk, 0);
        for (uint64_t at = 0; at < size; at += kZeroChunk)
        {
            const auto length = static_cast<size_t>(std::min<uint64_t>(kZeroChunk, size - at));
            TZ_CHECK(file.writeAt(at, ByteSpan(zeros).first(length)));
        }
    }
    return file.flush();
}

// Creates the file at its size, unless it is there with that size already (another size is as good as missing: the
// HOME Menu refuses it, and extdata files can't be resized). Returns whether it had to.
Expected<bool> ensureFile(FileSystem& fs, const char* path, uint64_t size, ByteSpan content)
{
    auto entry = fs.stat(path);
    if (entry && !entry->isDirectory && entry->size == size)
        return false;
    if (entry)
        TZ_CHECK(fs.removeFile(path));
    else if (entry.error().code != Errc::NotFound)
        return fail(entry.error());
    TZ_CHECK(fs.createFile(path, size));
    auto file = TZ_TRY(fs.open(path, OpenMode::ReadWrite));
    TZ_CHECK(fill(*file, size, content));
    return true;
}

Expected<void> copyFile(File& from, File& to, uint64_t size)
{
    Bytes buffer(kCopyChunk);
    for (uint64_t at = 0; at < size; at += kCopyChunk)
    {
        const MutableByteSpan chunk(buffer.data(), static_cast<size_t>(std::min<uint64_t>(kCopyChunk, size - at)));
        TZ_CHECK(from.readAt(at, chunk));
        TZ_CHECK(to.writeAt(at, chunk));
    }
    return to.flush();
}

// Copies a badge file to the SD card, through a temporary name so that an interrupted copy is never taken for one.
Expected<void> backupFile(FileSystem& badgeExt, const char* source, FileSystem& sd, const std::string& target,
                          uint64_t size)
{
    auto from = TZ_TRY(badgeExt.open(source, OpenMode::Read));
    if (from->size() != size)
        return fail(Errc::InvalidData, std::string(source) + " has the wrong size");
    const std::string partial = target + ".part";
    if (sd.exists(partial))
        TZ_CHECK(sd.removeFile(partial));
    TZ_CHECK(sd.createFile(partial, size));
    {
        auto to = TZ_TRY(sd.open(partial, OpenMode::ReadWrite));
        TZ_CHECK(copyFile(*from, *to, size));
    }
    if (sd.exists(target))
        TZ_CHECK(sd.removeFile(target));
    return sd.rename(partial, target);
}

bool isFile(FileSystem& fs, const std::string& path, uint64_t size)
{
    auto entry = fs.stat(path);
    return entry && !entry->isDirectory && entry->size == size;
}

// A name that isn't taken yet in its folder (compared as the SD card does, ignoring case): "Name", "Name (2)"... for
// folders, and "Name.2"... for badges (without their ".png"), whose names end at the first dot when installed again.
std::string unique(std::string name, std::set<std::string>& taken, bool badge)
{
    std::string candidate = name;
    for (int n = 2; taken.contains(lowered(candidate)); ++n)
        candidate = badge ? name + "." + std::to_string(n) : name + " (" + std::to_string(n) + ")";
    taken.insert(lowered(candidate));
    return candidate;
}

} // namespace

Expected<std::vector<SetSource>> findSets(FileSystem& sd, std::string_view root, std::string_view looseName)
{
    auto listing = sd.list(root);
    if (!listing)
    {
        if (listing.error().code == Errc::NotFound)
            return std::vector<SetSource>{};
        return fail(listing.error());
    }

    std::vector<SetSource> sets;
    std::vector<std::string> loose;
    auto setNamed = [&](std::string_view name) -> SetSource& {
        for (SetSource& set : sets)
            if (path::equalsIgnoreCase(set.name, name))
                return set;
        sets.push_back(SetSource{std::string(name)});
        return sets.back();
    };

    for (const DirEntry& entry : *listing)
    {
        if (hiddenName(entry.name))
            continue;
        if (entry.isDirectory)
        {
            SetSource& set = setNamed(entry.name);
            set.name = entry.name; // the folder's spelling, if a zip differs in case
            set.folderPath = path::join(root, entry.name);
        }
        else if (path::hasExtension(entry.name, "zip"))
        {
            const std::string_view stem = path::stem(entry.name);
            if (stem.empty())
                continue;
            // a download from Themezer is "<name> Badge Set by <author> (Themezer-<id>).zip"
            const auto download = library::parseDownloadName(stem);
            SetSource& set = setNamed(download ? std::string_view(download->name) : stem);
            set.zipPath = path::join(root, entry.name);
            if (download)
                set.author = download->author;
        }
        else if (path::hasExtension(entry.name, "png") && !path::equalsIgnoreCase(entry.name, kSetIconName))
        {
            loose.push_back(path::join(root, entry.name));
        }
    }

    std::sort(sets.begin(), sets.end(), [](const SetSource& a, const SetSource& b) {
        const int order = path::compareIgnoreCase(a.name, b.name);
        return order != 0 ? order < 0 : a.name < b.name;
    });
    if (!loose.empty())
    {
        std::sort(loose.begin(), loose.end(), [](const std::string& a, const std::string& b) {
            return path::compareIgnoreCase(path::filename(a), path::filename(b)) < 0;
        });
        SetSource set;
        set.name = std::string(looseName);
        set.loosePaths = std::move(loose);
        sets.push_back(std::move(set));
    }
    return sets;
}

Expected<SetSummary> summarize(FileSystem& sd, const SetSource& set)
{
    const SetFiles files = TZ_TRY(SetFiles::open(sd, set));
    return SetSummary{static_cast<int>(files.pictures.size()), files.icon.has_value()};
}

Expected<Image> setIcon(FileSystem& sd, const SetSource& set)
{
    const SetFiles files = TZ_TRY(SetFiles::open(sd, set));
    if (!files.icon)
        return fail(Errc::NotFound, "no set icon");
    const Bytes png = TZ_TRY(files.read(sd, *files.icon));
    return decodeIcon(png);
}

Expected<std::vector<Image>> previewBadges(FileSystem& sd, const SetSource& set, int max, const CancelToken& cancel)
{
    std::vector<Image> badges;
    if (max <= 0)
        return badges;
    const SetFiles files = TZ_TRY(SetFiles::open(sd, set));
    for (const PictureFile& picture : files.pictures)
    {
        if (cancel.cancelled())
            return fail(Errc::Cancelled, "cancelled");
        auto png = files.read(sd, picture);
        if (!png)
            continue;
        auto sheet = decodeSheet(*png);
        if (!sheet)
            continue;
        const bool more = forEachBadge(*sheet, [&](Image badge) {
            badges.push_back(std::move(badge));
            return static_cast<int>(badges.size()) < max;
        });
        if (!more)
            break;
    }
    return badges;
}

Expected<InstallResult> install(FileSystem& sd, FileSystem& badgeExt, std::span<const SetSource> sets,
                                uint32_t principalId, ByteSpan defaultIcon, const ProgressFn& progress)
{
    TZ_CHECK(ensureFiles(badgeExt));

    // the steps progress counts: every picture
    int total = 0;
    for (const SetSource& set : sets)
        if (auto summary = summarize(sd, set))
            total += summary->images;

    auto data = TZ_TRY(badgeExt.open(kDataPath, OpenMode::ReadWrite));
    auto managerFile = TZ_TRY(badgeExt.open(kManagerPath, OpenMode::ReadWrite));
    // A new manager file, except for the badges placed on the HOME Menu, which are the HOME Menu's to keep. The
    // count of placed badges (0xC) starts again from 0, as GYTB and Anemone3DS have always left it.
    Bytes manager = emptyManager(principalId);
    TZ_CHECK(managerFile->readAt(kPlacedOffset, MutableByteSpan(manager).subspan(kPlacedOffset)));

    InstallResult result;
    BadgeWriter writer(*data);
    Bytes setNames;
    Bytes setIcons;
    std::optional<Image> fallbackIcon;
    int done = 0;
    int badges = 0;
    bool cancelled = false;

    for (const SetSource& set : sets)
    {
        auto opened = SetFiles::open(sd, set);
        if (!opened)
        {
            ++result.skipped;
            continue;
        }
        const SetFiles& files = *opened;
        result.skipped += files.broken;
        if (files.pictures.empty())
            continue;
        if (result.sets == kMaxSets)
        {
            result.full = true;
            break;
        }

        const auto slot = static_cast<uint32_t>(result.sets);
        const int firstBadge = badges;
        for (const PictureFile& picture : files.pictures)
        {
            if (progress && !progress(done, std::max(total, done)))
            {
                cancelled = true;
                break;
            }
            ++done;
            auto png = files.read(sd, picture);
            auto sheet = png ? decodeSheet(*png) : fail(png.error());
            if (!sheet)
            {
                ++result.skipped;
                continue;
            }

            std::string name;
            uint64_t shortcut;
            parseFileName(picture.name, name, shortcut);
            const Bytes names = nameSlots(name.empty() ? std::string_view(set.name) : std::string_view(name));
            Expected<void> written;
            forEachBadge(*sheet, [&](Image badge) {
                if (badges == kMaxBadges)
                {
                    result.full = true;
                    return false;
                }
                written = writer.add(badges, names, std::move(badge));
                if (!written)
                    return false;
                writeBadge(&manager[kBadgesOffset + static_cast<size_t>(badges) * kBadgeRecordSize],
                           static_cast<uint32_t>(badges), slot + 1, shortcut);
                ++badges;
                return true;
            });
            TZ_CHECK(written);
            if (result.full)
                break;
        }

        if (badges > firstBadge)
        {
            writeSet(&manager[kSetsOffset + slot * kSetRecordSize], slot, static_cast<uint32_t>(firstBadge),
                     static_cast<uint32_t>(badges - firstBadge));
            const Bytes names = nameSlots(set.name);
            setNames.insert(setNames.end(), names.begin(), names.end());

            std::optional<Image> icon;
            if (files.icon)
                if (auto png = files.read(sd, *files.icon))
                    if (auto decoded = decodeIcon(*png))
                        icon = std::move(*decoded);
            if (!icon)
            {
                if (!fallbackIcon)
                {
                    auto decoded = decodeIcon(defaultIcon);
                    if (decoded)
                    {
                        fallbackIcon = std::move(*decoded);
                    }
                    else
                    {
                        // no usable default either: plain grey
                        fallbackIcon = Image(kIconSide, kIconSide);
                        for (size_t i = 0; i < fallbackIcon->pixels.size(); i += 4)
                            std::memcpy(&fallbackIcon->pixels[i], "\x80\x80\x80\xFF", 4);
                    }
                }
                icon = fallbackIcon;
            }
            setIcons.resize(setIcons.size() + kSetIconSize);
            encodeSetIcon(*icon, &setIcons[setIcons.size() - kSetIconSize]);
            ++result.sets;
        }
        if (cancelled || result.full)
            break;
    }

    // Cancelled before anything was written: everything is as it was. Later, the badges so far are finished off, so
    // that the manager file describes what the data file holds.
    if (cancelled && !writer.wrote())
        return fail(Errc::Cancelled, "cancelled");
    result.badges = badges;

    TZ_CHECK(writer.flush());
    if (!setNames.empty())
    {
        TZ_CHECK(data->writeAt(kSetNamesOffset, setNames));
        TZ_CHECK(data->writeAt(kSetIconsOffset, setIcons));
    }
    TZ_CHECK(data->flush());

    writeU32(&manager[kSetCountOffset], static_cast<uint32_t>(result.sets));
    writeU32(&manager[kBadgeCountOffset], static_cast<uint32_t>(badges));
    writeU32(&manager[kTotalOffset], static_cast<uint32_t>(badges) * kQuantity);
    for (int i = 0; i < badges; ++i)
        manager[kBadgeBitsOffset + static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1 << (i % 8));
    for (int i = 0; i < result.sets; ++i)
        manager[kSetBitsOffset + static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1 << (i % 8));
    TZ_CHECK(managerFile->writeAt(0, manager));
    TZ_CHECK(managerFile->flush());

    if (cancelled)
        return fail(Errc::Cancelled, "cancelled after " + std::to_string(badges) + " badges");
    if (progress)
        progress(done, std::max(total, done));
    return result;
}

Expected<void> uninstall(FileSystem& badgeExt)
{
    TZ_CHECK(ensureFiles(badgeExt));
    const Bytes manager = emptyManager(storedPrincipalId(badgeExt));
    auto file = TZ_TRY(badgeExt.open(kManagerPath, OpenMode::ReadWrite));
    TZ_CHECK(file->writeAt(0, manager));
    return file->flush();
}

int installedCount(FileSystem& badgeExt)
{
    return static_cast<int>(std::min<uint32_t>(readManagerU32(badgeExt, kBadgeCountOffset), kMaxBadges));
}

uint32_t storedPrincipalId(FileSystem& badgeExt)
{
    return readManagerU32(badgeExt, kPrincipalIdOffset);
}

Expected<void> ensureFiles(FileSystem& badgeExt)
{
    const bool newData = TZ_TRY(ensureFile(badgeExt, kDataPath, kDataSize, {}));
    const bool newManager = TZ_TRY(ensureFile(badgeExt, kManagerPath, kManagerSize, emptyManager(0)));
    if (newData && !newManager)
    {
        // the manager would describe badges whose names and pictures are gone
        auto file = TZ_TRY(badgeExt.open(kManagerPath, OpenMode::ReadWrite));
        TZ_CHECK(file->writeAt(0, emptyManager(storedPrincipalId(badgeExt))));
        TZ_CHECK(file->flush());
    }
    return {};
}

Expected<bool> backupOnce(FileSystem& badgeExt, FileSystem& sd, std::string_view dir)
{
    if (hasBackup(sd, dir))
        return false;
    TZ_CHECK(sd.createDirectories(dir));
    TZ_CHECK(backupFile(badgeExt, kDataPath, sd, path::join(dir, kDataName), kDataSize));
    TZ_CHECK(backupFile(badgeExt, kManagerPath, sd, path::join(dir, kManagerName), kManagerSize));
    return true;
}

Expected<void> restoreBackup(FileSystem& sd, std::string_view dir, FileSystem& badgeExt)
{
    if (!hasBackup(sd, dir))
        return fail(Errc::NotFound, "no badge backup");
    auto dataBackup = TZ_TRY(sd.open(path::join(dir, kDataName), OpenMode::Read));
    auto managerBackup = TZ_TRY(sd.open(path::join(dir, kManagerName), OpenMode::Read));

    // missing or broken files are created, without filling them first: the copy writes all of them
    for (const auto& [file, size] :
         {std::pair{kDataPath, uint64_t{kDataSize}}, std::pair{kManagerPath, uint64_t{kManagerSize}}})
    {
        if (isFile(badgeExt, file, size))
            continue;
        if (badgeExt.exists(file))
            TZ_CHECK(badgeExt.removeFile(file));
        TZ_CHECK(badgeExt.createFile(file, size));
    }
    {
        auto data = TZ_TRY(badgeExt.open(kDataPath, OpenMode::ReadWrite));
        TZ_CHECK(copyFile(*dataBackup, *data, kDataSize));
    }
    auto manager = TZ_TRY(badgeExt.open(kManagerPath, OpenMode::ReadWrite));
    return copyFile(*managerBackup, *manager, kManagerSize);
}

bool hasBackup(FileSystem& sd, std::string_view dir)
{
    return isFile(sd, path::join(dir, kDataName), kDataSize) && isFile(sd, path::join(dir, kManagerName), kManagerSize);
}

Expected<int> exportBadges(FileSystem& badgeExt, FileSystem& sd, std::string_view dir, const ProgressFn& progress)
{
    const Bytes manager = TZ_TRY(badgeExt.readFile(kManagerPath, kManagerSize));
    if (manager.size() != kManagerSize)
        return fail(Errc::InvalidData, "BadgeMngFile.dat has the wrong size");
    const uint32_t badgeCount = std::min<uint32_t>(readU32(&manager[kBadgeCountOffset]), kMaxBadges);
    const uint32_t setCount = std::min<uint32_t>(readU32(&manager[kSetCountOffset]), kMaxSets);
    if (badgeCount == 0)
        return 0;
    auto data = TZ_TRY(badgeExt.open(kDataPath, OpenMode::Read));
    if (data->size() != kDataSize)
        return fail(Errc::InvalidData, "BadgeData.dat has the wrong size");
    TZ_CHECK(sd.createDirectories(dir));

    // a folder per set, with its icon
    struct Folder
    {
        uint32_t setId;
        std::string path;
        std::set<std::string> taken;
    };
    std::vector<Folder> folders;
    std::set<std::string> folderNames;
    Bytes names(kNamesSize);
    Bytes icon(kSetIconSize);
    for (uint32_t i = 0; i < setCount; ++i)
    {
        const uint8_t* record = &manager[kSetsOffset + i * kSetRecordSize];
        const uint32_t slot = readU32(record + 0x14);
        if (slot >= static_cast<uint32_t>(kMaxSets))
            continue;
        TZ_CHECK(data->readAt(kSetNamesOffset + uint64_t{slot} * kNamesSize, names));
        const std::string folder = unique(path::sanitizeFilename(pickName(names.data())), folderNames, false);
        Folder entry{readU32(record + 0x10), path::join(dir, folder), {std::string(kIconStem)}};
        TZ_CHECK(sd.createDirectory(entry.path));
        TZ_CHECK(data->readAt(kSetIconsOffset + uint64_t{slot} * kSetIconSize, icon));
        const Bytes png = TZ_TRY(encodePng(decodeSetIcon(icon.data())));
        TZ_CHECK(sd.writeFile(path::join(entry.path, kSetIconName), png));
        folders.push_back(std::move(entry));
    }

    // Badges of a set that isn't there (GYTB doesn't make sets) go in dir itself, to be installed as loose badges.
    const size_t loose = folders.size();
    folders.push_back({kAllSets, std::string(dir), {std::string(kIconStem)}});

    // Their file names first: badges of the same name (the parts of a sheet) are numbered in order, so that they are
    // installed in that order again.
    struct Planned
    {
        size_t folder;
        uint32_t slot;
        std::string name;
    };
    std::vector<Planned> planned;
    std::map<std::pair<size_t, std::string>, int> sameName;
    SlotReader nameReader(*data, kBadgeNamesOffset, kNamesSize);
    for (uint32_t i = 0; i < badgeCount; ++i)
    {
        const uint8_t* record = &manager[kBadgesOffset + i * kBadgeRecordSize];
        const uint32_t setId = readU32(record + 0x8);
        uint32_t slot = readU16(record + 0xC);
        if (slot >= static_cast<uint32_t>(kMaxBadges))
            slot = i;
        const uint64_t shortcut = readU64(record + 0x18);
        size_t folder = 0;
        while (folder < loose && folders[folder].setId != setId)
            ++folder;

        // dots would end the name, so they go; a shortcut is written the way install reads it back
        std::string name = pickName(TZ_TRY(nameReader.get(slot)));
        std::replace(name.begin(), name.end(), '.', '-');
        name = path::sanitizeFilename(name, 120);
        if (shortcut >> 32 == kSystemApplications >> 32)
        {
            char low[12];
            std::snprintf(low, sizeof(low), ".%08X", static_cast<unsigned>(shortcut & 0xFFFFFFFF));
            name += low;
        }
        ++sameName[{folder, lowered(name)}];
        planned.push_back({folder, slot, std::move(name)});
    }

    SlotReader pictureReader(*data, kLargeOffset, kLargeSize);
    std::map<std::pair<size_t, std::string>, int> numbered;
    int saved = 0;
    for (const Planned& badge : planned)
    {
        if (progress && !progress(saved, static_cast<int>(planned.size())))
            return fail(Errc::Cancelled, "cancelled");
        std::string name = badge.name;
        const std::pair<size_t, std::string> key{badge.folder, lowered(name)};
        if (const int count = sameName[key]; count > 1)
        {
            const std::string number = std::to_string(++numbered[key]);
            name += "." + std::string(std::to_string(count).size() - number.size(), '0') + number;
        }
        Folder& folder = folders[badge.folder];
        const std::string file = unique(std::move(name), folder.taken, true) + ".png";

        const Bytes png = TZ_TRY(encodePng(decodePixels(TZ_TRY(pictureReader.get(badge.slot)), kBadgeSide)));
        TZ_CHECK(sd.writeFile(path::join(folder.path, file), png));
        ++saved;
    }
    if (progress)
        progress(saved, saved);
    return saved;
}

} // namespace tz::badges
