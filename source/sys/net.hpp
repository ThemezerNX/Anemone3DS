// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// HTTPS downloads with curl. Servers are verified against the CA certificates in romfs:/cacert.pem.

#pragma once

#include "core/io.hpp"
#include "core/cancel.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tz::net
{

// Starts the socket service and curl the first time it is needed; false when that fails.
bool init();
void exit();
// Whether the console is connected to a network at all.
bool online();

struct Progress
{
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> total{0}; // 0 until the server says
};

struct Request
{
    std::string url;
    // The content types accepted, like "application/zip". Anything else fails with InvalidData. Empty: anything.
    std::vector<std::string> accept;
    uint64_t maxSize = 32 * 1024 * 1024;
    const CancelToken* cancel = nullptr;
    Progress* progress = nullptr;
    // Takes the body as it arrives instead of collecting it in Response::body; returning false stops the download.
    std::function<bool(ByteSpan)> sink;
};

struct Response
{
    long status = 0;
    std::string contentType; // lowercase, without parameters: "application/json"
    // The file name from Content-Disposition, else the last part of the URL, made safe for the SD card. May be empty.
    std::string filename;
    std::string url; // after redirects
    Bytes body;
};

// One per thread, and kept: it keeps the connection to a server open for the next request.
// Errors: NoNetwork, Network (connecting, timeouts), Tls (the certificate, often because of the console's clock),
// Http (status 400 and up, in Error::system), TooLarge, InvalidData (an unexpected content type), Cancelled.
class Client
{
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Expected<Response> get(const Request& request);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// A download read while it continues, for music previews. It runs on a thread of its own, so that the Network lane's
// pages and icons keep loading; read() waits for data. Destroying the stream cancels the download.
std::unique_ptr<StreamReader> stream(std::string url, uint64_t maxSize);

// Percent-encodes everything but unreserved characters, for query parameters.
std::string urlEncode(std::string_view text);

} // namespace tz::net
