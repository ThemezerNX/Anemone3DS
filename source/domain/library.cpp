// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "domain/library.hpp"

#include "core/path.hpp"
#include "core/smdh.hpp"
#include "core/zip.hpp"
#include "domain/package.hpp"
#include "domain/splashes.hpp"

#include <algorithm>
#include <cstring>

namespace tz::library
{

namespace
{

constexpr const char* kSmdhName = "info.smdh";
constexpr const char* kPreviewName = "preview.png";
constexpr const char* kBodyName = "body_LZ.bin";
constexpr const char* kMusicName = "bgm.bcstm";

constexpr char kCacheMagic[4] = {'T', 'Z', 'L', 'C'};
constexpr uint16_t kCacheVersion = 1;

FileId fileId(const Package& package, std::string_view name)
{
    const auto size = package.size(name);
    if (!size || *size == 0 || *size > UINT32_MAX)
        return {};
    return FileId{static_cast<uint32_t>(*size), package.knownCrc(name).value_or(0)};
}

// A small writer and reader for the cache file.
class Writer
{
public:
    void u8(uint8_t v) { data_.push_back(v); }
    void u16(uint16_t v)
    {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v)
    {
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v >> 16));
    }
    void u64(uint64_t v)
    {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    void text(std::string_view s)
    {
        const size_t length = std::min<size_t>(s.size(), 0xFFFF);
        u16(static_cast<uint16_t>(length));
        data_.insert(data_.end(), s.begin(), s.begin() + static_cast<ptrdiff_t>(length));
    }
    void raw(const void* p, size_t n)
    {
        const auto* bytes = static_cast<const uint8_t*>(p);
        data_.insert(data_.end(), bytes, bytes + n);
    }
    Bytes& data() { return data_; }

private:
    Bytes data_;
};

class Parser
{
public:
    explicit Parser(ByteSpan data) : data_(data) {}
    bool ok() const { return ok_; }
    uint8_t u8() { return need(1) ? data_[pos_++] : 0; }
    uint16_t u16()
    {
        if (!need(2))
            return 0;
        const uint16_t v = readU16(data_.data() + pos_);
        pos_ += 2;
        return v;
    }
    uint32_t u32()
    {
        if (!need(4))
            return 0;
        const uint32_t v = readU32(data_.data() + pos_);
        pos_ += 4;
        return v;
    }
    uint64_t u64()
    {
        const uint64_t low = u32();
        return low | (static_cast<uint64_t>(u32()) << 32);
    }
    std::string text()
    {
        const uint16_t length = u16();
        if (!need(length))
            return {};
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), length);
        pos_ += length;
        return s;
    }
    bool raw(void* p, size_t n)
    {
        if (!need(n))
            return false;
        std::memcpy(p, data_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    bool atEnd() const { return pos_ >= data_.size(); }

private:
    bool need(size_t n)
    {
        if (!ok_ || data_.size() - pos_ < n)
            ok_ = false;
        return ok_;
    }
    ByteSpan data_;
    size_t pos_ = 0;
    bool ok_ = true;
};

char lowerAscii(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

} // namespace

const char* root(Kind kind)
{
    return kind == Kind::Theme ? kThemesRoot : kSplashesRoot;
}

std::optional<DownloadName> parseDownloadName(std::string_view stem)
{
    // "... (Themezer-T1ED6C)" at the end
    constexpr std::string_view kTag = " (Themezer-";
    const size_t tag = stem.rfind(kTag);
    if (tag == std::string_view::npos || !stem.ends_with(')'))
        return std::nullopt;
    const std::string_view id = stem.substr(tag + kTag.size(), stem.size() - 1 - tag - kTag.size());
    if (id.empty() || id.find_first_of(" ()") != std::string_view::npos)
        return std::nullopt;
    const std::string_view rest = stem.substr(0, tag);
    // "<name> <kind> by <author>": the last " by " after a kind's word
    for (const std::string_view kind : {" Theme by ", " Splash by ", " Badge Set by "})
    {
        const size_t at = rest.rfind(kind);
        if (at == std::string_view::npos || at == 0)
            continue;
        DownloadName parts{std::string(rest.substr(0, at)), std::string(rest.substr(at + kind.size()))};
        if (parts.author.empty())
            return std::nullopt;
        return parts;
    }
    return std::nullopt;
}

std::string Item::displayName() const
{
    if (info && !info->name.empty())
        return info->name;
    const std::string_view stem = source.zip ? path::stem(source.fileName) : std::string_view(source.fileName);
    if (auto parts = parseDownloadName(stem))
        return std::move(parts->name);
    return std::string(stem);
}

std::string Item::author() const
{
    if (info && !info->author.empty())
        return info->author;
    const std::string_view stem = source.zip ? path::stem(source.fileName) : std::string_view(source.fileName);
    if (auto parts = parseDownloadName(stem))
        return std::move(parts->author);
    return {};
}

Expected<std::vector<Source>> scan(FileSystem& sd, std::string_view rootPath)
{
    TZ_CHECK(sd.createDirectories(rootPath));
    const std::vector<DirEntry> entries = TZ_TRY(sd.list(rootPath));
    std::vector<Source> sources;
    sources.reserve(entries.size());
    for (const DirEntry& entry : entries)
    {
        if (entry.name.empty() || entry.name.front() == '.')
            continue;
        const bool zip = !entry.isDirectory && path::hasExtension(entry.name, "zip");
        if (!entry.isDirectory && !zip)
            continue;
        sources.push_back(Source{path::join(rootPath, entry.name), entry.name, zip, zip ? entry.size : 0});
    }
    return sources;
}

Expected<Info> readInfo(FileSystem& sd, const Source& source, Kind kind, int language)
{
    const Package package = TZ_TRY(Package::open(sd, source.path, source.zip));
    Info info;
    if (auto data = package.read(kSmdhName, kSmdhSize * 2))
    {
        if (auto smdh = parseSmdh(*data))
        {
            const SmdhText& title = smdh->title(language);
            info.name = title.name;
            info.author = title.author;
            info.description = title.description;
            info.hasSmdh = true;
            // an all-black icon is no icon (dumps without one have it)
            const bool blank = std::all_of(smdh->largeIcon.begin(), smdh->largeIcon.end(),
                                           [](uint16_t pixel) { return pixel == 0; });
            if (!blank)
                info.icon = std::make_shared<const Icon>(smdh->largeIcon);
        }
    }
    info.hasPreview = package.has(kPreviewName);
    if (kind == Kind::Theme)
    {
        info.body = fileId(package, kBodyName);
        info.hasMusic = package.has(kMusicName);
    }
    else
    {
        info.top = fileId(package, splashes::kTopName);
        info.bottom = fileId(package, splashes::kBottomName);
    }
    return info;
}

Expected<uint32_t> computeCrc(FileSystem& sd, const Source& source, std::string_view name)
{
    const Package package = TZ_TRY(Package::open(sd, source.path, source.zip));
    if (auto known = package.knownCrc(name))
        return *known;
    auto stream = TZ_TRY(package.stream(name));
    uint32_t crc = 0;
    Bytes buffer(0x10000);
    for (;;)
    {
        const size_t got = TZ_TRY(stream->read(buffer));
        if (got == 0)
            break;
        crc = crc32(ByteSpan(buffer.data(), got), crc);
    }
    return crc;
}

std::vector<std::pair<Source, Info>> loadCache(FileSystem& sd, std::string_view cachePath, Kind kind)
{
    std::vector<std::pair<Source, Info>> entries;
    auto data = sd.readFile(cachePath, 64 * 1024 * 1024);
    if (!data || data->size() < 12 || std::memcmp(data->data(), kCacheMagic, 4) != 0)
        return entries;

    Parser in(ByteSpan(*data).subspan(4));
    if (in.u16() != kCacheVersion || in.u8() != static_cast<uint8_t>(kind))
        return entries;
    in.u8();
    const uint32_t count = in.u32();
    entries.reserve(std::min<uint32_t>(count, 10000));
    for (uint32_t i = 0; i < count && in.ok(); ++i)
    {
        Source source;
        source.path = in.text();
        source.fileName = std::string(path::filename(source.path));
        source.zip = in.u8() != 0;
        source.size = in.u64();

        Info info;
        const uint8_t flags = in.u8();
        info.hasSmdh = flags & 1;
        info.hasPreview = flags & 2;
        info.hasMusic = flags & 4;
        info.name = in.text();
        info.author = in.text();
        info.description = in.text();
        for (FileId* id : {&info.body, &info.top, &info.bottom})
        {
            id->size = in.u32();
            id->crc = in.u32();
        }
        if (flags & 8)
        {
            auto icon = std::make_shared<Icon>();
            if (!in.raw(icon->data(), sizeof(Icon)))
                break;
            info.icon = std::move(icon);
        }
        if (in.ok())
            entries.emplace_back(std::move(source), std::move(info));
    }
    return entries;
}

Expected<void> saveCache(FileSystem& sd, std::string_view cachePath, Kind kind, const std::vector<Item>& items)
{
    Writer out;
    out.raw(kCacheMagic, 4);
    out.u16(kCacheVersion);
    out.u8(static_cast<uint8_t>(kind));
    out.u8(0);
    const size_t countAt = out.data().size();
    out.u32(0);

    uint32_t count = 0;
    for (const Item& item : items)
    {
        if (!item.info)
            continue;
        const Info& info = *item.info;
        out.text(item.source.path);
        out.u8(item.source.zip ? 1 : 0);
        out.u64(item.source.size);
        out.u8(static_cast<uint8_t>((info.hasSmdh ? 1 : 0) | (info.hasPreview ? 2 : 0) | (info.hasMusic ? 4 : 0) |
                                    (info.icon ? 8 : 0)));
        out.text(info.name);
        out.text(info.author);
        out.text(info.description);
        for (const FileId* id : {&info.body, &info.top, &info.bottom})
        {
            out.u32(id->size);
            out.u32(id->crc);
        }
        if (info.icon)
            out.raw(info.icon->data(), sizeof(Icon));
        ++count;
    }
    writeU32(out.data().data() + countAt, count);

    TZ_CHECK(sd.createDirectories(path::parent(cachePath)));
    return sd.writeFile(cachePath, out.data());
}

int compareNatural(std::string_view a, std::string_view b)
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size())
    {
        if (isDigit(a[i]) && isDigit(b[j]))
        {
            // compare the whole numbers: skip leading zeros, then the longer one is larger
            size_t si = i;
            size_t sj = j;
            while (si < a.size() && a[si] == '0')
                ++si;
            while (sj < b.size() && b[sj] == '0')
                ++sj;
            size_t ei = si;
            size_t ej = sj;
            while (ei < a.size() && isDigit(a[ei]))
                ++ei;
            while (ej < b.size() && isDigit(b[ej]))
                ++ej;
            if (ei - si != ej - sj)
                return ei - si < ej - sj ? -1 : 1;
            if (const int c = a.substr(si, ei - si).compare(b.substr(sj, ej - sj)); c != 0)
                return c < 0 ? -1 : 1;
            i = ei;
            j = ej;
            continue;
        }
        const char x = lowerAscii(a[i]);
        const char y = lowerAscii(b[j]);
        if (x != y)
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return 0;
}

void sort(std::vector<Item>& items, Sort order)
{
    auto byName = [](const Item& x, const Item& y) {
        const int c = compareNatural(x.displayName(), y.displayName());
        return c != 0 ? c < 0 : compareNatural(x.source.fileName, y.source.fileName) < 0;
    };
    switch (order)
    {
    case Sort::Name:
        std::stable_sort(items.begin(), items.end(), byName);
        break;
    case Sort::Author:
        std::stable_sort(items.begin(), items.end(), [&](const Item& x, const Item& y) {
            const std::string_view ax = x.info ? std::string_view(x.info->author) : std::string_view();
            const std::string_view ay = y.info ? std::string_view(y.info->author) : std::string_view();
            // items without an author go last
            if (ax.empty() != ay.empty())
                return !ax.empty();
            const int c = compareNatural(ax, ay);
            return c != 0 ? c < 0 : byName(x, y);
        });
        break;
    case Sort::FileName:
        std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) {
            return compareNatural(x.source.fileName, y.source.fileName) < 0;
        });
        break;
    }
}

} // namespace tz::library
