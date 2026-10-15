// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "sys/net.hpp"

#include "core/path.hpp"
#include "core/utf.hpp"
#include "sys/log.hpp"
#include "sys/thread.hpp"

#include <3ds.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <curl/curl.h>
#include <malloc.h>
#include <mutex>
#include <optional>
#include <sys/socket.h>

namespace tz::net
{

namespace
{

constexpr u32 kSocBufferSize = 0x100000;
// The 3DS's default TCP receive buffer of 8 KiB holds Wi-Fi downloads to about 360 KB/s; 32 KiB nearly doubles that.
// 64 KiB and more stall transfers instead: the 3DS doesn't scale the TCP window, and its field can't hold that much.
constexpr int kReceiveBufferSize = 0x8000;
constexpr long kCurlBufferSize = 64 * 1024;
constexpr long kMaxRedirects = 5;
constexpr long kConnectTimeoutSeconds = 15;
// A transfer that receives less than a byte a second for this long has failed.
constexpr long kStallSeconds = 30;
constexpr const char* kCaBundlePath = "romfs:/cacert.pem";
constexpr long kMaxCaBundleSize = 1024 * 1024;
// The body of an error response is read and dropped, so that its connection can serve the next request, up to this
// size; a longer one closes the connection instead.
constexpr uint64_t kMaxErrorBodySize = 64 * 1024;
// How long a transfer waits for its sockets at a time, in milliseconds, before and after its response begins; see
// perform().
constexpr int kAwaitingResponsePollMs = 10;
constexpr int kReceivingPollMs = 100;
constexpr size_t kStreamStackSize = 128 * 1024;

// Certificates
//
// Servers are verified against romfs:/cacert.pem, Mozilla's list of certificate authorities, which every build takes
// fresh from curl.se. While that list is current, verification is strict: a server that can't be verified is an
// error, so nobody can slip a fake theme or update in between.
//
// A list that is more than three years old means the app hasn't been updated in that long, and it may never be
// again. Servers move to certificate authorities an old list doesn't know, and the app would stop working. So once
// the list is that old, a request whose server can't be verified is made again without verification, and so is every
// request after it. That includes updates: installing one by hand with FBI's QR scanner doesn't verify servers at
// all (it turns verification off for both its httpc and its curl downloads), so this is no weaker than the way
// around it, and an abandoned app can still be updated if Themezer ever returns to it.
constexpr int64_t kCaBundleLifetimeSeconds = 1096LL * 24 * 60 * 60;

// What init() starts, shared by every thread.
struct State
{
    Mutex mutex;
    bool started = false;
    u32* socBuffer = nullptr;
    // romfs:/cacert.pem, read once and handed to every transfer; curl still parses it for each new connection
    Bytes caBundle;
    // when the list was made, from its header (or this build's date)
    int64_t caBundleTime = 0;
    // every Client, so that exit() can close their connections before curl and the socket service stop
    std::vector<Client::Impl*> clients;
};

State state;
// set when the outdated list failed to verify a server, for every request after
std::atomic<bool> verificationOff{false};

// online() asks the ac service. acInit() counts its users, so starting it here is fine whether or not the app has too.
Mutex acMutex;
bool acTried = false;
bool acStarted = false;

std::string_view trim(std::string_view text)
{
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t start = text.find_first_not_of(kSpace);
    if (start == std::string_view::npos)
        return {};
    return text.substr(start, text.find_last_not_of(kSpace) - start + 1);
}

char toLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// "My%20Theme" -> "My Theme"; a '%' without two hex digits after it stays as it is.
std::string percentDecode(std::string_view text)
{
    std::string decoded;
    decoded.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            const int high = hexDigit(text[i + 1]);
            const int low = hexDigit(text[i + 2]);
            if (high >= 0 && low >= 0)
            {
                decoded += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        decoded += text[i];
    }
    return decoded;
}

std::optional<uint64_t> parseNumber(std::string_view text)
{
    uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc() || stop != end)
        return std::nullopt;
    return value;
}

// "HTTP/1.1 404 Not Found" -> 404; 0 when there is no status.
long parseStatus(std::string_view line)
{
    const size_t space = line.find(' ');
    if (space == std::string_view::npos)
        return 0;
    const std::optional<uint64_t> status = parseNumber(line.substr(space + 1, 3));
    return status ? static_cast<long>(*status) : 0;
}

// The type of a Content-Type, which is all that is compared: "Application/JSON; charset=utf-8" -> "application/json".
std::string mediaType(std::string_view value)
{
    std::string type(trim(value.substr(0, value.find(';'))));
    for (char& c : type)
        c = toLower(c);
    return type;
}

// Whether a response's content type is one of those accepted ("image/*" takes them all). A server that names none is
// given the benefit of the doubt.
bool acceptsType(const std::vector<std::string>& accepted, const std::string& type)
{
    if (accepted.empty() || type.empty())
        return true;
    for (const std::string& entry : accepted)
    {
        const std::string wanted = mediaType(entry);
        if (wanted == type || wanted == "*/*")
            return true;
        if (wanted.ends_with("/*") && type.starts_with(std::string_view(wanted).substr(0, wanted.size() - 1)))
            return true;
    }
    return false;
}

// Only ever a name, never a path: what follows the last '/' or '\'.
std::string_view lastComponent(std::string_view name)
{
    const size_t slash = name.find_last_of("/\\");
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

// The value of filename* (RFC 8187): a charset, a language and the percent-encoded name, as in UTF-8''My%20Theme.zip.
// "" for charsets other than UTF-8 and ISO-8859-1.
std::string decodeExtendedValue(std::string_view value)
{
    const size_t charsetEnd = value.find('\'');
    const size_t languageEnd = charsetEnd == std::string_view::npos ? charsetEnd : value.find('\'', charsetEnd + 1);
    if (languageEnd == std::string_view::npos)
        return {};
    const std::string_view charset = value.substr(0, charsetEnd);
    std::string decoded = percentDecode(value.substr(languageEnd + 1));
    if (path::equalsIgnoreCase(charset, "UTF-8"))
        return decoded;
    if (path::equalsIgnoreCase(charset, "ISO-8859-1"))
    {
        std::string utf8;
        for (char c : decoded)
            utf::appendUtf8(utf8, static_cast<unsigned char>(c));
        return utf8;
    }
    return {};
}

// The file name in a Content-Disposition header (RFC 6266): attachment; filename="My Theme.zip";
// filename*=UTF-8''My%20Theme.zip. filename* comes first when both are there, as it can hold any character.
std::string dispositionFilename(std::string_view header)
{
    std::string plain;
    std::string extended;
    size_t i = 0;
    while (i < header.size())
    {
        // a parameter: name=value or name="quoted value", after the disposition type
        while (i < header.size() && (header[i] == ';' || header[i] == ' ' || header[i] == '\t'))
            ++i;
        const size_t nameStart = i;
        while (i < header.size() && header[i] != '=' && header[i] != ';')
            ++i;
        const std::string_view name = trim(header.substr(nameStart, i - nameStart));
        if (i >= header.size() || header[i] != '=')
            continue; // the type itself, or a parameter without a value

        ++i;
        while (i < header.size() && (header[i] == ' ' || header[i] == '\t'))
            ++i;
        std::string value;
        if (i < header.size() && header[i] == '"')
        {
            for (++i; i < header.size() && header[i] != '"'; ++i)
            {
                if (header[i] == '\\' && i + 1 < header.size())
                    ++i;
                value += header[i];
            }
            while (i < header.size() && header[i] != ';')
                ++i;
        }
        else
        {
            const size_t valueStart = i;
            while (i < header.size() && header[i] != ';')
                ++i;
            value = trim(header.substr(valueStart, i - valueStart));
        }

        if (path::equalsIgnoreCase(name, "filename*"))
            extended = decodeExtendedValue(value);
        else if (path::equalsIgnoreCase(name, "filename"))
            plain = std::move(value);
    }
    return extended.empty() ? plain : extended;
}

// The last segment of a URL's path, decoded: "https://host/a/My%20Theme.zip?raw=1" -> "My Theme.zip".
std::string urlFilename(std::string_view url)
{
    url = url.substr(0, url.find_first_of("?#"));
    const size_t scheme = url.find("://");
    const size_t pathStart = url.find('/', scheme == std::string_view::npos ? 0 : scheme + 3);
    if (pathStart == std::string_view::npos)
        return {};
    return percentDecode(url.substr(url.rfind('/') + 1));
}

// Whether a name has anything but dots and spaces in it ("", "." and ".." don't).
bool usableName(std::string_view name)
{
    return std::any_of(name.begin(), name.end(), [](char c) {
        return c != '.' && c != ' ' && static_cast<unsigned char>(c) >= 0x20 && c != 0x7F;
    });
}

std::string downloadFilename(std::string_view disposition, std::string_view url)
{
    const std::string named = dispositionFilename(disposition);
    if (usableName(lastComponent(named)))
        return path::sanitizeFilename(lastComponent(named));
    const std::string fromUrl = urlFilename(url);
    if (usableName(lastComponent(fromUrl)))
        return path::sanitizeFilename(lastComponent(fromUrl));
    return {};
}

// Grows a download's buffer. Without exceptions a failed allocation ends the app, so this checks that the memory is
// there first; false when it isn't.
bool reserveBytes(Bytes& bytes, uint64_t capacity)
{
    if (capacity <= bytes.capacity())
        return true;
    if (capacity > SIZE_MAX)
        return false;
    // volatile, or the compiler drops an allocation that is freed unused, and the check with it
    void* volatile probe = std::malloc(static_cast<size_t>(capacity));
    if (!probe)
        return false;
    std::free(probe);
    bytes.reserve(static_cast<size_t>(capacity));
    return true;
}

// Appends to a download's buffer: room for all of it at once when its size is known, else in doubling steps.
bool appendBytes(Bytes& bytes, ByteSpan data, uint64_t expectedSize, uint64_t maxSize)
{
    const uint64_t needed = uint64_t{bytes.size()} + data.size();
    if (needed > bytes.capacity())
    {
        uint64_t capacity = expectedSize;
        if (capacity < needed)
            capacity = std::max<uint64_t>(uint64_t{bytes.capacity()} * 2, 0x10000);
        capacity = std::max(needed, std::min(capacity, maxSize));
        if (!reserveBytes(bytes, capacity))
            return false;
    }
    bytes.insert(bytes.end(), data.begin(), data.end());
    return true;
}

// A request, and what curl's callbacks learn of its responses. Redirects and interim responses (100 Continue) come
// first, each with its own status line and headers; only the last one has a body, which is checked before it arrives.
struct Transfer
{
    explicit Transfer(const Request& request) : request(request) {}

    const Request& request;
    Response response;
    // why a callback stopped the transfer
    std::optional<Error> error;
    bool sinkStopped = false;

    // the response being received
    long status = 0;
    bool hasLocation = false;
    std::optional<uint64_t> contentLength;
    std::string contentType;
    std::string disposition;
    // its headers are complete and it is the one with the body
    bool finalResponse = false;
    // an error status: its body is read and dropped
    bool discard = false;
    uint64_t received = 0;

    // Whether a response is yet to begin, on a new connection or after a redirect; waits are short until it does.
    bool awaitingResponse = true;

    void beginResponse(long code)
    {
        status = code;
        hasLocation = false;
        contentLength.reset();
        contentType.clear();
        disposition.clear();
        finalResponse = false;
        discard = false;
        received = 0;
        response.body.clear();
        awaitingResponse = false;
    }

    // At the empty line after a response's headers. False stops the transfer.
    bool endHeaders()
    {
        if (status >= 100 && status < 200)
            return true; // an interim response: the real one follows
        if (status >= 300 && status < 400 && hasLocation)
        {
            // curl follows it and ignores its body; the next response may come from another server
            awaitingResponse = true;
            return true;
        }
        return acceptBody();
    }

    // Checks the response with the body, before any of it arrives. False stops the transfer.
    bool acceptBody()
    {
        finalResponse = true;
        if (status < 200 || status >= 300)
        {
            discard = true;
            return !contentLength || *contentLength <= kMaxErrorBodySize;
        }
        if (!acceptsType(request.accept, contentType))
        {
            error = Error{Errc::InvalidData, 0, "content type \"" + contentType + "\" from " + request.url};
            return false;
        }
        if (contentLength)
        {
            if (*contentLength > request.maxSize)
            {
                error = Error{Errc::TooLarge, 0, std::to_string(*contentLength) + " bytes from " + request.url};
                return false;
            }
            if (request.progress)
                request.progress->total.store(*contentLength, std::memory_order_relaxed);
            if (!request.sink && !reserveBytes(response.body, *contentLength))
            {
                error = Error{Errc::OutOfMemory, 0, std::to_string(*contentLength) + " bytes from " + request.url};
                return false;
            }
        }
        return true;
    }
};

size_t onHeader(char* data, size_t size, size_t count, void* user)
{
    Transfer& transfer = *static_cast<Transfer*>(user);
    const size_t length = size * count;
    const std::string_view line = trim(std::string_view(data, length));

    if (line.starts_with("HTTP/"))
    {
        transfer.beginResponse(parseStatus(line));
        return length;
    }
    if (transfer.finalResponse)
        return length; // trailers after a chunked body
    if (line.empty())
        return transfer.endHeaders() ? length : 0;

    const size_t colon = line.find(':');
    if (colon == std::string_view::npos)
        return length;
    const std::string_view name = trim(line.substr(0, colon));
    const std::string_view value = trim(line.substr(colon + 1));
    if (path::equalsIgnoreCase(name, "Content-Type"))
        transfer.contentType = mediaType(value);
    else if (path::equalsIgnoreCase(name, "Content-Length"))
        transfer.contentLength = parseNumber(value);
    else if (path::equalsIgnoreCase(name, "Content-Disposition"))
        transfer.disposition = value;
    else if (path::equalsIgnoreCase(name, "Location"))
        transfer.hasLocation = !value.empty();
    return length;
}

size_t onBody(char* data, size_t size, size_t count, void* user)
{
    Transfer& transfer = *static_cast<Transfer*>(user);
    const Request& request = transfer.request;
    const size_t length = size * count;

    // curl hands the end of the headers to onHeader first; this covers a response that somehow skipped it
    if (!transfer.finalResponse && !transfer.acceptBody())
        return CURL_WRITEFUNC_ERROR;
    if (transfer.discard)
    {
        transfer.received += length;
        return transfer.received <= kMaxErrorBodySize ? length : CURL_WRITEFUNC_ERROR;
    }

    if (transfer.received > request.maxSize || length > request.maxSize - transfer.received)
    {
        transfer.error = Error{Errc::TooLarge, 0, "more than " + std::to_string(request.maxSize) + " bytes from " +
                                                      request.url};
        return CURL_WRITEFUNC_ERROR;
    }
    const ByteSpan bytes(reinterpret_cast<const uint8_t*>(data), length);
    if (request.sink)
    {
        if (!request.sink(bytes))
        {
            transfer.sinkStopped = true;
            return CURL_WRITEFUNC_ERROR;
        }
    }
    else if (!appendBytes(transfer.response.body, bytes, transfer.contentLength.value_or(0), request.maxSize))
    {
        transfer.error = Error{Errc::OutOfMemory, 0, std::to_string(transfer.received + length) + " bytes from " +
                                                         request.url};
        return CURL_WRITEFUNC_ERROR;
    }

    transfer.received += length;
    if (request.progress)
        request.progress->received.store(transfer.received, std::memory_order_relaxed);
    return length;
}

int onSocket(void* user, curl_socket_t socket, curlsocktype purpose)
{
    if (purpose == CURLSOCKTYPE_IPCXN)
    {
        // a new connection, which the next waits are for; see perform()
        static_cast<Transfer*>(user)->awaitingResponse = true;
        const int size = kReceiveBufferSize;
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    }
    return CURL_SOCKOPT_OK;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const CancelToken* cancel = static_cast<Transfer*>(user)->request.cancel;
    return cancel && cancel->cancelled() ? 1 : 0;
}

// The CA certificates curl verifies servers against.
void setCertificates(CURL* easy)
{
    if (!state.caBundle.empty())
    {
        curl_blob blob{state.caBundle.data(), state.caBundle.size(), CURL_BLOB_NOCOPY};
        if (curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &blob) == CURLE_OK)
        {
            // only these: this build of curl would also read sdmc:/config/ssl/cacert.pem by default
            curl_easy_setopt(easy, CURLOPT_CAINFO, static_cast<char*>(nullptr));
            curl_easy_setopt(easy, CURLOPT_CAPATH, static_cast<char*>(nullptr));
            return;
        }
    }
    curl_easy_setopt(easy, CURLOPT_CAINFO, kCaBundlePath);
    curl_easy_setopt(easy, CURLOPT_CAPATH, static_cast<char*>(nullptr));
}

void configure(CURL* easy, Transfer& transfer, curl_slist* headers, char* errorText, bool verify)
{
    // back to curl's defaults; the handle keeps its connections, DNS cache and TLS sessions
    curl_easy_reset(easy);
    errorText[0] = '\0';
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errorText);
    curl_easy_setopt(easy, CURLOPT_URL, transfer.request.url.c_str());
    // GitHub's API refuses requests without one
    curl_easy_setopt(easy, CURLOPT_USERAGENT, APP_TITLE "/" APP_VERSION);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, kMaxRedirects);
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, kStallSeconds);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, kCurlBufferSize);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, onSocket);
    curl_easy_setopt(easy, CURLOPT_SOCKOPTDATA, &transfer);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
    setCertificates(easy);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &transfer);
}

// Runs a transfer with the multi interface rather than curl_easy_perform(), to decide how long it waits. The 3DS
// doesn't wake poll() when a non-blocking connect() completes (and libctru's POLLOUT and POLLERR values are off), so
// curl would sleep out its whole wait, a second, before sending a request on a new connection. Until a response
// begins, the waits here last 10 ms; after that, data wakes poll() as it should and they can be longer. A list page
// went from about 1.1 s to 100 ms this way.
CURLcode perform(CURL* easy, CURLM* multi, Transfer& transfer)
{
    const CancelToken* cancel = transfer.request.cancel;
    CURLMcode status = curl_multi_add_handle(multi, easy);
    bool cancelled = false;
    int running = 0;
    while (status == CURLM_OK)
    {
        status = curl_multi_perform(multi, &running);
        if (status != CURLM_OK || running == 0)
            break;
        // also checked here, as curl doesn't report progress in every state
        if (cancel && cancel->cancelled())
        {
            cancelled = true;
            break;
        }
        const int timeoutMs = transfer.awaitingResponse ? kAwaitingResponsePollMs : kReceivingPollMs;
        status = curl_multi_poll(multi, nullptr, 0, timeoutMs, nullptr);
    }

    CURLcode code = status == CURLM_OUT_OF_MEMORY ? CURLE_OUT_OF_MEMORY : CURLE_FAILED_INIT;
    if (cancelled)
        code = CURLE_ABORTED_BY_CALLBACK;
    int queued = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &queued))
    {
        if (message->msg == CURLMSG_DONE && message->easy_handle == easy)
            code = message->data.result;
    }
    // stops the transfer if it is still running; a finished one leaves its connection in the multi handle's cache
    curl_multi_remove_handle(multi, easy);
    return code;
}

Error curlError(CURLcode code, const char* errorText, const Transfer& transfer)
{
    Error error{Errc::Network, static_cast<int32_t>(code), {}};
    switch (code)
    {
    // the server's certificate, which often fails because of the console's clock
    case CURLE_PEER_FAILED_VERIFICATION: // also CURLE_SSL_CACERT
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_ISSUER_ERROR:
        error.code = Errc::Tls;
        break;
    // only HTTPS is allowed, also where a redirect leads
    case CURLE_UNSUPPORTED_PROTOCOL:
        error.code = Errc::Unsupported;
        break;
    case CURLE_URL_MALFORMAT:
        error.code = Errc::InvalidData;
        break;
    case CURLE_TOO_MANY_REDIRECTS:
        error.code = Errc::Http;
        error.system = static_cast<int32_t>(transfer.status);
        break;
    case CURLE_OUT_OF_MEMORY:
        error.code = Errc::OutOfMemory;
        break;
    case CURLE_ABORTED_BY_CALLBACK:
        error.code = Errc::Cancelled;
        break;
    default:
        break;
    }
    error.detail = "curl " + std::to_string(static_cast<int>(code)) + ": " +
                   (errorText[0] ? errorText : curl_easy_strerror(code)) + " (" + transfer.request.url + ")";
    return error;
}

Expected<Response> finish(CURL* easy, Transfer& transfer, CURLcode code, const char* errorText)
{
    const Request& request = transfer.request;
    // whatever else happened, the one who asked has lost interest
    if (request.cancel && request.cancel->cancelled())
        return fail(Errc::Cancelled, "cancelled: " + request.url);
    if (transfer.error)
        return fail(std::move(*transfer.error));
    if (transfer.finalResponse && transfer.discard)
    {
        return fail(Errc::Http, "HTTP " + std::to_string(transfer.status) + " from " + request.url,
                    static_cast<int32_t>(transfer.status));
    }
    if (transfer.sinkStopped)
        return fail(Errc::Cancelled, "the receiver stopped the download of " + request.url);
    if (code != CURLE_OK)
    {
        Error error = curlError(code, errorText, transfer);
        if (error.code == Errc::Network && !online())
            error.code = Errc::NoNetwork; // the connection went with the Wi-Fi
        return fail(std::move(error));
    }
    if (!transfer.finalResponse)
        return fail(Errc::InvalidData, "no response from " + request.url);

    Response response = std::move(transfer.response);
    response.status = transfer.status;
    response.contentType = transfer.contentType;
    char* url = nullptr;
    curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url);
    response.url = url ? url : request.url;
    response.filename = downloadFilename(transfer.disposition, response.url);
    return response;
}

Bytes readCaBundle()
{
    Bytes data;
    FILE* file = std::fopen(kCaBundlePath, "rb");
    if (!file)
    {
        LOG_WARN("no %s: secure connections will fail", kCaBundlePath);
        return data;
    }
    long size = -1;
    if (std::fseek(file, 0, SEEK_END) == 0)
        size = std::ftell(file);
    if (size > 0 && size <= kMaxCaBundleSize && std::fseek(file, 0, SEEK_SET) == 0)
    {
        data.resize(static_cast<size_t>(size));
        if (std::fread(data.data(), 1, data.size(), file) != data.size())
            data.clear();
    }
    std::fclose(file);
    if (data.empty())
        LOG_WARN("couldn't read %s: curl will try it itself", kCaBundlePath);
    return data;
}

// Days since 1970-01-01 of a date in the Gregorian calendar.
int64_t daysFromCivil(int64_t year, int month, int day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yearOfEra = year - era * 400;
    const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

int monthNumber(const char* name)
{
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i)
        if (std::strncmp(name, kMonths[i], 3) == 0)
            return i + 1;
    return 0;
}

// When the CA list was made: curl writes "## Certificate data from Mozilla as of: Fri Sep 25 03:12:01 2026 GMT" at
// its top. Without that line, the date this app was built.
int64_t caBundleTime(const Bytes& bundle)
{
    const std::string_view text(reinterpret_cast<const char*>(bundle.data()), std::min<size_t>(bundle.size(), 4096));
    const size_t at = text.find("as of: ");
    if (at != std::string_view::npos)
    {
        char weekday[4] = {};
        char month[4] = {};
        int day = 0, hour = 0, minute = 0, second = 0, year = 0;
        const std::string line(text.substr(at + 7, 40));
        if (std::sscanf(line.c_str(), "%3s %3s %d %d:%d:%d %d", weekday, month, &day, &hour, &minute, &second,
                        &year) == 7 &&
            monthNumber(month))
            return daysFromCivil(year, monthNumber(month), day) * 86400 + hour * 3600 + minute * 60 + second;
    }
    // __DATE__ is "Oct 15 2026"
    char month[4] = {};
    int day = 1, year = 2026;
    std::sscanf(__DATE__, "%3s %d %d", month, &day, &year);
    return daysFromCivil(year, std::max(1, monthNumber(month)), day) * 86400;
}

bool caBundleOutdated()
{
    // the console's clock counts seconds since 1970 in its own time zone, close enough for years
    return static_cast<int64_t>(time(nullptr)) > state.caBundleTime + kCaBundleLifetimeSeconds;
}

Expected<void> start()
{
    std::lock_guard lock(state.mutex);
    if (state.started)
        return {};

    state.socBuffer = static_cast<u32*>(memalign(0x1000, kSocBufferSize));
    if (!state.socBuffer)
        return fail(Errc::OutOfMemory, "socket service buffer");
    const Result result = socInit(state.socBuffer, kSocBufferSize);
    if (R_FAILED(result))
    {
        std::free(state.socBuffer);
        state.socBuffer = nullptr;
        return fail(Errc::Network, "socInit", result);
    }
    const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK)
    {
        socExit();
        std::free(state.socBuffer);
        state.socBuffer = nullptr;
        return fail(Errc::Network, std::string("curl_global_init: ") + curl_easy_strerror(code), code);
    }
    state.caBundle = readCaBundle();
    state.caBundleTime = caBundleTime(state.caBundle);
    state.started = true;
    return {};
}

} // namespace

struct Client::Impl
{
    CURL* easy = nullptr;
    // runs the transfers, and keeps their connections open for the next one
    CURLM* multi = nullptr;
    char errorText[CURL_ERROR_SIZE] = {};

    Expected<void> open()
    {
        std::lock_guard lock(state.mutex);
        if (easy && multi)
            return {};
        close();
        easy = curl_easy_init();
        multi = curl_multi_init();
        if (!easy || !multi)
        {
            close();
            return fail(Errc::OutOfMemory, "curl handles");
        }
        return {};
    }

    // With state.mutex held.
    void close()
    {
        // the easy handle isn't in the multi handle between transfers, so it can go first, as curl asks
        if (easy)
            curl_easy_cleanup(easy);
        if (multi)
            curl_multi_cleanup(multi);
        easy = nullptr;
        multi = nullptr;
    }
};

Client::Client() : impl_(std::make_unique<Impl>())
{
    std::lock_guard lock(state.mutex);
    state.clients.push_back(impl_.get());
}

Client::~Client()
{
    std::lock_guard lock(state.mutex);
    impl_->close();
    std::erase(state.clients, impl_.get());
}

Expected<Response> Client::get(const Request& request)
{
    if (request.cancel && request.cancel->cancelled())
        return fail(Errc::Cancelled, "cancelled: " + request.url);
    // checked first, so that the error says what to do about it
    if (!online())
        return fail(Errc::NoNetwork, "Wi-Fi isn't connected");
    TZ_CHECK(start());
    TZ_CHECK(impl_->open());
    if (request.progress)
    {
        request.progress->received.store(0, std::memory_order_relaxed);
        request.progress->total.store(0, std::memory_order_relaxed);
    }

    curl_slist* headers = nullptr;
    if (!request.accept.empty())
    {
        std::string accept = "Accept: ";
        for (size_t i = 0; i < request.accept.size(); ++i)
        {
            if (i > 0)
                accept += ", ";
            accept += request.accept[i];
        }
        headers = curl_slist_append(nullptr, accept.c_str());
    }

    const bool verify = !verificationOff.load(std::memory_order_relaxed);
    Expected<Response> result = [&] {
        Transfer transfer(request);
        configure(impl_->easy, transfer, headers, impl_->errorText, verify);
        const CURLcode code = perform(impl_->easy, impl_->multi, transfer);
        return finish(impl_->easy, transfer, code, impl_->errorText);
    }();

    // the CA list is outdated and doesn't know this server's certificate: see "Certificates" above
    if (!result && result.error().code == Errc::Tls && verify && caBundleOutdated())
    {
        if (!verificationOff.exchange(true))
            LOG_WARN("the CA certificates are over three years old: connecting without verifying from now on");
        if (request.progress)
        {
            request.progress->received.store(0, std::memory_order_relaxed);
            request.progress->total.store(0, std::memory_order_relaxed);
        }
        Transfer transfer(request);
        configure(impl_->easy, transfer, headers, impl_->errorText, false);
        const CURLcode code = perform(impl_->easy, impl_->multi, transfer);
        result = finish(impl_->easy, transfer, code, impl_->errorText);
    }
    curl_slist_free_all(headers);
    return result;
}

namespace
{

// The stream threads' Client, kept for the next stream: it can reuse the connection, or at least the TLS session,
// which spares it a full TLS handshake, a slow thing for the 3DS.
Mutex streamClientMutex;
std::unique_ptr<Client> idleStreamClient;

// A download on its own thread, kept whole in memory while it is read.
class DownloadStream final : public StreamReader
{
public:
    DownloadStream(std::string url, uint64_t maxSize) : maxSize_(maxSize)
    {
        // not on the Network lane, so that pages and icons keep loading meanwhile
        thread_ = Thread::start([this, url = std::move(url)] { download(url); }, priority::kNetwork, -2,
                                kStreamStackSize);
        if (!thread_.running())
        {
            error_ = Error{Errc::OutOfMemory, 0, "download thread"};
            done_ = true;
        }
    }

    ~DownloadStream() override
    {
        cancel_.cancel();
        thread_.join();
    }

    DownloadStream(const DownloadStream&) = delete;
    DownloadStream& operator=(const DownloadStream&) = delete;

    // The size the server announced, and once complete the size received; 0 until then.
    uint64_t size() const override { return progress_.total.load(std::memory_order_relaxed); }

    Expected<size_t> read(MutableByteSpan out) override
    {
        if (out.empty())
            return size_t{0};
        std::lock_guard lock(mutex_);
        while (position_ >= data_.size() && !done_)
            changed_.wait(mutex_);
        if (position_ < data_.size())
        {
            const size_t length = static_cast<size_t>(std::min<uint64_t>(out.size(), data_.size() - position_));
            std::copy_n(data_.begin() + static_cast<ptrdiff_t>(position_), length, out.begin());
            position_ += length;
            return length;
        }
        if (error_)
            return fail(*error_);
        return size_t{0};
    }

    // Forward waits for the data; back is always possible, as everything received is kept.
    Expected<void> seek(uint64_t target) override
    {
        std::lock_guard lock(mutex_);
        while (data_.size() < target && !done_)
            changed_.wait(mutex_);
        if (target <= data_.size())
        {
            position_ = target;
            return {};
        }
        if (error_)
            return fail(*error_);
        return fail(Errc::InvalidData, "seek past the end");
    }

    uint64_t position() const override { return position_; }

private:
    void download(const std::string& url)
    {
        std::unique_ptr<Client> client;
        {
            std::lock_guard lock(streamClientMutex);
            client = std::move(idleStreamClient);
        }
        if (!client)
            client = std::make_unique<Client>();

        Request request;
        request.url = url;
        request.maxSize = maxSize_;
        request.cancel = &cancel_;
        request.progress = &progress_;
        request.sink = [this](ByteSpan data) { return append(data); };
        const Expected<Response> result = client->get(request);

        {
            std::lock_guard lock(streamClientMutex);
            if (!idleStreamClient)
                idleStreamClient = std::move(client);
        }
        client.reset();

        std::lock_guard lock(mutex_);
        if (!result && !error_)
            error_ = result.error();
        if (result && progress_.total.load(std::memory_order_relaxed) == 0)
            progress_.total.store(data_.size(), std::memory_order_relaxed);
        done_ = true;
        changed_.notifyAll();
    }

    // The download's sink, on its thread.
    bool append(ByteSpan data)
    {
        std::lock_guard lock(mutex_);
        if (cancel_.cancelled())
            return false;
        if (!appendBytes(data_, data, progress_.total.load(std::memory_order_relaxed), maxSize_))
        {
            error_ = Error{Errc::OutOfMemory, 0, std::to_string(data_.size() + data.size()) + " bytes to stream"};
            return false;
        }
        changed_.notifyAll();
        return true;
    }

    const uint64_t maxSize_;
    CancelToken cancel_;
    Progress progress_;

    // shared with the download thread
    Mutex mutex_;
    ConditionVariable changed_;
    Bytes data_;
    bool done_ = false;
    std::optional<Error> error_;

    // the reader's
    uint64_t position_ = 0;

    Thread thread_;
};

} // namespace

std::unique_ptr<StreamReader> stream(std::string url, uint64_t maxSize)
{
    return std::make_unique<DownloadStream>(std::move(url), maxSize);
}

bool init()
{
    const Expected<void> started = start();
    if (!started)
        log::writeError("start the network", started.error());
    return started.has_value();
}

void exit()
{
    // the kept stream Client first: it takes state.mutex as it goes
    std::unique_ptr<Client> idle;
    {
        std::lock_guard lock(streamClientMutex);
        idle = std::move(idleStreamClient);
    }
    idle.reset();

    {
        std::lock_guard lock(state.mutex);
        // the Clients that remain can be destroyed later, or start everything again with their next request
        for (Client::Impl* client : state.clients)
            client->close();
        if (state.started)
        {
            curl_global_cleanup();
            socExit();
            std::free(state.socBuffer);
            state.socBuffer = nullptr;
            state.caBundle = Bytes();
            state.started = false;
        }
    }

    std::lock_guard lock(acMutex);
    if (acStarted)
        acExit();
    acStarted = false;
    acTried = false;
}

bool online()
{
    {
        std::lock_guard lock(acMutex);
        if (!acTried)
        {
            acTried = true;
            acStarted = R_SUCCEEDED(acInit());
        }
    }
    u32 status = 0;
    // when the system can't tell, the connection attempt will
    return R_FAILED(ACU_GetWifiStatus(&status)) || status != 0;
}

std::string urlEncode(std::string_view text)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size());
    for (char c : text)
    {
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved)
        {
            encoded += c;
            continue;
        }
        const auto byte = static_cast<unsigned char>(c);
        encoded += '%';
        encoded += kHex[byte >> 4];
        encoded += kHex[byte & 0x0F];
    }
    return encoded;
}

} // namespace tz::net
