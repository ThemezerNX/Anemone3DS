// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "services/updater.hpp"

#include "core/path.hpp"
#include "core/version.hpp"
#include "sys/fs.hpp"
#include "sys/log.hpp"

#include <3ds.h>
#include <jansson.h>
#include <mbedtls/sha256.h>
#include <memory>

namespace tz::update
{

namespace
{

constexpr uint64_t kMaxReleaseSize = 1024 * 1024;
// for an asset the release gives no size for
constexpr uint64_t kMaxAssetSize = 64 * 1024 * 1024;
constexpr std::string_view kSdmcPrefix = "sdmc:/";
constexpr char kHexDigits[] = "0123456789abcdef";

struct JsonDelete
{
    void operator()(json_t* json) const { json_decref(json); }
};
using JsonPtr = std::unique_ptr<json_t, JsonDelete>;

// A string field; "" when it is missing, null or something else.
std::string text(json_t* object, const char* key)
{
    json_t* value = json_object_get(object, key);
    if (!json_is_string(value))
        return {};
    return std::string(json_string_value(value), json_string_length(value));
}

// GitHub's "digest" of an asset, "sha256:<hex>": the hex in lowercase, or "" when it is something else.
std::string sha256Hex(std::string_view digest)
{
    constexpr std::string_view kPrefix = "sha256:";
    if (!digest.starts_with(kPrefix) || digest.size() != kPrefix.size() + 64)
        return {};
    std::string hex(digest.substr(kPrefix.size()));
    for (char& c : hex)
    {
        if (c >= 'A' && c <= 'F')
            c = static_cast<char>(c - 'A' + 'a');
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return {};
    }
    return hex;
}

Asset readAsset(json_t* object)
{
    Asset asset;
    asset.url = text(object, "browser_download_url");
    json_t* size = json_object_get(object, "size");
    if (json_is_integer(size) && json_integer_value(size) > 0)
        asset.size = static_cast<uint64_t>(json_integer_value(size));
    asset.sha256 = sha256Hex(text(object, "digest"));
    return asset;
}

// A release in the format of GitHub's API. Drafts and pre-releases give nullopt: they aren't for everyone.
Expected<std::optional<Release>> parseRelease(ByteSpan json)
{
    json_error_t error;
    const JsonPtr root(json_loadb(reinterpret_cast<const char*>(json.data()), json.size(), 0, &error));
    if (!root)
        return fail(Errc::InvalidData, std::string("release: ") + error.text);
    if (!json_is_object(root.get()))
        return fail(Errc::InvalidData, "release: not an object");
    if (json_is_true(json_object_get(root.get(), "draft")) || json_is_true(json_object_get(root.get(), "prerelease")))
        return std::nullopt;

    const std::string tag = text(root.get(), "tag_name");
    std::string_view version = tag;
    if (version.starts_with('v') || version.starts_with('V'))
        version.remove_prefix(1);
    if (!Version::parse(version))
        return fail(Errc::InvalidData, "release tag \"" + tag + "\"");

    Release release;
    release.version = std::string(version);
    release.notes = text(root.get(), "body");
    json_t* assets = json_object_get(root.get(), "assets");
    size_t index = 0;
    json_t* object = nullptr;
    json_array_foreach(assets, index, object)
    {
        const std::string name = text(object, "name");
        Asset* target = nullptr;
        if (path::hasExtension(name, "cia"))
            target = &release.cia;
        else if (path::hasExtension(name, "3dsx"))
            target = &release.threedsx;
        // the first of each kind
        if (!target || !target->url.empty())
            continue;
        Asset asset = readAsset(object);
        if (!asset.url.empty())
            *target = std::move(asset);
    }
    return release;
}

bool is3dsxPath(std::string_view path)
{
    return path.starts_with(kSdmcPrefix) && path::hasExtension(path, "3dsx");
}

// The size and SHA-256 of a download, worked out as it arrives.
class Digest
{
public:
    Digest()
    {
        mbedtls_sha256_init(&context_);
        (void)mbedtls_sha256_starts_ret(&context_, 0);
    }
    ~Digest() { mbedtls_sha256_free(&context_); }
    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;

    void update(ByteSpan data)
    {
        (void)mbedtls_sha256_update_ret(&context_, data.data(), data.size());
        size_ += data.size();
    }

    // Whether it is the file the release announced.
    Expected<void> verify(const Asset& asset)
    {
        if (asset.size != 0 && size_ != asset.size)
        {
            return fail(Errc::InvalidData,
                        "downloaded " + std::to_string(size_) + " bytes instead of " + std::to_string(asset.size));
        }
        if (asset.sha256.empty())
            return {};
        unsigned char hash[32];
        (void)mbedtls_sha256_finish_ret(&context_, hash);
        std::string hex;
        for (unsigned char byte : hash)
        {
            hex += kHexDigits[byte >> 4];
            hex += kHexDigits[byte & 0x0F];
        }
        if (hex != asset.sha256)
            return fail(Errc::InvalidData, "SHA-256 " + hex + " instead of " + asset.sha256);
        return {};
    }

private:
    mbedtls_sha256_context context_;
    uint64_t size_ = 0;
};

net::Request assetRequest(const Asset& asset, net::Progress* progress, const CancelToken& cancel)
{
    net::Request request;
    request.url = asset.url;
    // of any content type (GitHub says application/octet-stream): the size and the digest tell whether it's right
    request.maxSize = asset.size != 0 ? asset.size : kMaxAssetSize;
    request.cancel = &cancel;
    request.progress = progress;
    return request;
}

// How a download into an install went: the destination's error comes first, as it's why the download stopped.
Expected<void> downloadResult(const Expected<net::Response>& response, const std::optional<Error>& writeError,
                              Digest& digest, const Asset& asset)
{
    if (writeError)
        return fail(*writeError);
    if (!response)
        return fail(response.error());
    return digest.verify(asset);
}

// Streams the CIA into the system as it downloads. The title only changes with AM_FinishCiaInstall(), once all of it
// is there and checked; anything else cancels the install.
Expected<void> installCia(net::Client& client, const Asset& asset, net::Progress* progress, const CancelToken& cancel)
{
    Result result = amInit();
    if (R_FAILED(result))
        return fail(Errc::NotAllowed, "amInit", result);
    Handle cia = 0;
    result = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
    if (R_FAILED(result))
    {
        amExit();
        return fail(fsError(result, "start the CIA install"));
    }

    Digest digest;
    uint64_t offset = 0;
    std::optional<Error> writeError;
    net::Request request = assetRequest(asset, progress, cancel);
    request.sink = [&](ByteSpan data) {
        size_t done = 0;
        while (done < data.size())
        {
            u32 written = 0;
            const Result wrote = FSFILE_Write(cia, &written, offset + done, data.data() + done, data.size() - done, 0);
            if (R_FAILED(wrote) || written == 0)
            {
                writeError = R_FAILED(wrote) ? fsError(wrote, "write the CIA") : Error{Errc::Io, 0, "CIA not written"};
                return false;
            }
            done += written;
        }
        digest.update(data);
        offset += data.size();
        return true;
    };

    Expected<void> installed = downloadResult(client.get(request), writeError, digest, asset);
    if (installed)
    {
        // either call closes the handle
        result = AM_FinishCiaInstall(cia);
        if (R_FAILED(result))
            installed = fail(fsError(result, "finish the CIA install"));
    }
    else
    {
        AM_CancelCIAInstall(cia);
    }
    amExit();
    return installed;
}

// Downloads into a new file, complete and checked or failed.
Expected<void> downloadFile(Archive& sd, const std::string& path, net::Client& client, const Asset& asset,
                            net::Progress* progress, const CancelToken& cancel)
{
    const std::unique_ptr<File> file = TZ_TRY(sd.open(path, OpenMode::Create));
    Digest digest;
    uint64_t offset = 0;
    std::optional<Error> writeError;
    net::Request request = assetRequest(asset, progress, cancel);
    request.sink = [&](ByteSpan data) {
        if (Expected<void> written = file->writeAt(offset, data); !written)
        {
            writeError = written.error();
            return false;
        }
        digest.update(data);
        offset += data.size();
        return true;
    };
    TZ_CHECK(downloadResult(client.get(request), writeError, digest, asset));
    return file->flush();
}

// The new 3dsx is written next to the running one, then swapped in, so that a failure leaves the running one as it
// was: <path>.new, then <path> becomes <path>.old and <path>.new becomes <path>.
Expected<void> install3dsx(net::Client& client, const Asset& asset, std::string_view appPath, net::Progress* progress,
                           const CancelToken& cancel)
{
    // "sdmc:/3ds/Themezer.3dsx" -> "/3ds/Themezer.3dsx"
    const std::string target(appPath.substr(kSdmcPrefix.size() - 1));
    const std::string fresh = target + ".new";
    const std::string old = target + ".old";
    const std::unique_ptr<Archive> sd = TZ_TRY(Archive::openSdmc());

    // a leftover of an update that failed
    if (sd->exists(fresh))
        TZ_CHECK(sd->removeFile(fresh));
    if (Expected<void> downloaded = downloadFile(*sd, fresh, client, asset, progress, cancel); !downloaded)
    {
        (void)sd->removeFile(fresh);
        return downloaded;
    }

    if (sd->exists(old))
        (void)sd->removeFile(old);
    if (Expected<void> moved = sd->rename(target, old); !moved)
    {
        (void)sd->removeFile(fresh);
        return moved;
    }
    if (Expected<void> placed = sd->rename(fresh, target); !placed)
    {
        // the running version back where it was
        (void)sd->rename(old, target);
        (void)sd->removeFile(fresh);
        return placed;
    }
    // The running app may still read its RomFS from the old file, so the system can refuse; it's only a leftover then,
    // which the next update removes.
    if (Expected<void> removed = sd->removeFile(old); !removed)
        log::writeError("remove the previous 3dsx", removed.error());
    return {};
}

} // namespace

InstallKind installKind(const char* appPath)
{
    if (!envIsHomebrew())
        return InstallKind::Cia;
    if (appPath && is3dsxPath(appPath))
        return InstallKind::ThreeDsx;
    return InstallKind::None;
}

Expected<std::optional<Release>> check(net::Client& client, const CancelToken& cancel)
{
    // a development build would be "updated" to the release it was made after
    if (std::string_view(APP_VERSION).find("-dev") != std::string_view::npos)
        return std::nullopt;
    const std::optional<Version> current = Version::parse(APP_VERSION);
    if (!current)
    {
        LOG_WARN("the update check can't compare version %s", APP_VERSION);
        return std::nullopt;
    }
    // a build can leave the update check out with UPDATE_URL=""
    if (std::string_view(UPDATE_URL).empty())
        return std::nullopt;

    net::Request request;
    request.url = UPDATE_URL;
    request.accept = {"application/json", "application/vnd.github+json"};
    request.maxSize = kMaxReleaseSize;
    request.cancel = &cancel;
    Expected<net::Response> response = client.get(request);
    if (!response)
    {
        // what GitHub answers while there is no release yet
        if (response.error().code == Errc::Http && response.error().system == 404)
            return std::nullopt;
        return fail(std::move(response).error());
    }

    std::optional<Release> latest = TZ_TRY(parseRelease(response->body));
    // parseRelease() made sure the version parses
    if (!latest || !(*Version::parse(latest->version) > *current))
        return std::nullopt;
    return latest;
}

Expected<void> install(net::Client& client, const Release& release, InstallKind kind, const char* appPath,
                       net::Progress* progress, const CancelToken& cancel)
{
    switch (kind)
    {
    case InstallKind::Cia:
        if (release.cia.url.empty())
            return fail(Errc::NotFound, "the release has no CIA");
        return installCia(client, release.cia, progress, cancel);
    case InstallKind::ThreeDsx:
        if (release.threedsx.url.empty())
            return fail(Errc::NotFound, "the release has no 3dsx");
        if (!appPath || !is3dsxPath(appPath))
            return fail(Errc::Unsupported, std::string("can't replace ") + (appPath ? appPath : "an unknown 3dsx"));
        return install3dsx(client, release.threedsx, appPath, progress, cancel);
    case InstallKind::None:
        break;
    }
    return fail(Errc::Unsupported, "this copy of the app can't update itself");
}

} // namespace tz::update
