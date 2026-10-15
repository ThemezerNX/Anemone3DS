// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Two threads. The camera thread takes the outer camera's frames as they come, 30 a second, and keeps the newest for
// latestFrame(); the decode thread looks for QR codes with quirc in one of them about ten times a second.

#include "services/qr.hpp"

#include "quirc/quirc_internal.h"
#include "sys/log.hpp"
#include "sys/thread.hpp"

#include <3ds.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

namespace tz
{

namespace
{

constexpr size_t kPixels = static_cast<size_t>(QrScanner::kWidth) * QrScanner::kHeight;
constexpr u32 kFrameBytes = kPixels * sizeof(uint16_t);

// The camera thread goes on the system core when the app may use it, and otherwise shares the app core with the main
// thread. Either way it runs just above the main thread: once a frame is in, the next transfer has to be set up before
// the camera port's small buffer overflows. Besides that it only copies the frame, a millisecond or so.
constexpr int kSystemCore = 1;
constexpr int kAppCore = -2;
constexpr int kCapturePriority = priority::kMain - 1;
constexpr size_t kCaptureStack = 32 * 1024;

// quirc spends tens of milliseconds of the app core on a picture; ten pictures a second still find a code quickly.
constexpr s64 kDecodeIntervalNs = 100'000'000;
// quirc's flood fill recurses at most 128 levels in our copy (third_party/quirc/README.md): well within this.
constexpr size_t kDecodeStack = 64 * 1024;

// How long stopping waits for the frame in flight, in 1 ms steps; a frame takes 33.
constexpr int kStopPolls = 500;

// Version 40, the biggest QR code, is 177 cells across.
constexpr int kMaxGridSize = QUIRC_MAX_VERSION * 4 + 17;

// What quirc hands back for a code, too big for a thread's stack.
struct DecodeScratch
{
    quirc_code code;
    quirc_data data;
    uint8_t cells[QUIRC_MAX_BITMAP];
};

// Logs a camera call that failed; true if it worked.
bool check(Result result, const char* what)
{
    if (R_FAILED(result))
        LOG_ERROR("qr: couldn't %s (%08lX)", what, static_cast<unsigned long>(result));
    return R_SUCCEEDED(result);
}

// camInit() asks srv for cam:u, then starts the camera driver.
Error cameraError(Result result)
{
    // srv refuses services that aren't in the title's access list, and some ways of starting homebrew don't give it
    // cam:u
    if (R_MODULE(result) == RM_SRV || R_DESCRIPTION(result) == RD_NOT_AUTHORIZED)
        return Error{Errc::NotAllowed, static_cast<int32_t>(result), "no access to cam:u"};
    // the driver didn't start: something else is using the camera
    return Error{Errc::Busy, static_cast<int32_t>(result), "camera driver didn't start"};
}

// quirc reads 8-bit grey. (2R + 5G + B) / 8 of the channels widened to 8 bits is close to the usual luma weights
// (0.30, 0.59, 0.11), keeps white at 255 and needs no division.
void toGrey(const uint16_t* rgb565, uint8_t* grey)
{
    for (size_t i = 0; i < kPixels; ++i)
    {
        const uint32_t pixel = rgb565[i];
        const uint32_t r = pixel >> 11;
        const uint32_t g = (pixel >> 5) & 0x3F;
        const uint32_t b = pixel & 0x1F;
        const uint32_t r8 = (r << 3) | (r >> 2);
        const uint32_t g8 = (g << 2) | (g >> 4);
        const uint32_t b8 = (b << 3) | (b >> 2);
        grey[i] = static_cast<uint8_t>((2 * r8 + 5 * g8 + b8) >> 3);
    }
}

// Transposes scratch.code's cells. quirc turns a code so that its finder patterns are top left, top right and bottom
// left, which leaves a mirrored code (seen in a mirror, or through the back of a see-through sheet) transposed. quirc
// 1.2 calls this quirc_flip.
void transpose(DecodeScratch& scratch)
{
    quirc_code& code = scratch.code;
    std::memset(scratch.cells, 0, sizeof(scratch.cells));
    for (int y = 0; y < code.size; ++y)
    {
        for (int x = 0; x < code.size; ++x)
        {
            const int from = x * code.size + y;
            if (code.cell_bitmap[from >> 3] & (1 << (from & 7)))
            {
                const int to = y * code.size + x;
                scratch.cells[to >> 3] |= static_cast<uint8_t>(1 << (to & 7));
            }
        }
    }
    std::memcpy(code.cell_bitmap, scratch.cells, sizeof(scratch.cells));
}

// NULs and white space, which some generators leave after the text.
bool isPadding(uint8_t c)
{
    return c == '\0' || c == ' ' || (c >= '\t' && c <= '\r');
}

std::string payloadText(const quirc_data& data)
{
    size_t length = static_cast<size_t>(data.payload_len);
    while (length > 0 && isPadding(data.payload[length - 1]))
        --length;
    return std::string(reinterpret_cast<const char*>(data.payload), length);
}

// The text of the first code quirc found that decodes.
std::optional<std::string> readCodes(const quirc* qr, DecodeScratch& scratch)
{
    const int count = quirc_count(qr);
    for (int i = 0; i < count; ++i)
    {
        // quirc 1.1 fills the cell bitmap without checking that the grid fits it, and a misread timing pattern can
        // make a grid bigger than any QR code
        if (qr->grids[i].grid_size > kMaxGridSize)
            continue;
        quirc_extract(qr, i, &scratch.code);
        quirc_decode_error_t error = quirc_decode(&scratch.code, &scratch.data);
        if (error != QUIRC_SUCCESS)
        {
            // it may be mirrored; another try costs little next to finding the code
            transpose(scratch);
            error = quirc_decode(&scratch.code, &scratch.data);
        }
        if (error != QUIRC_SUCCESS)
            continue;
        std::string text = payloadText(scratch.data);
        if (!text.empty())
            return text;
    }
    return std::nullopt;
}

} // namespace

struct QrScanner::Impl
{
    ~Impl();

    Expected<void> open();

    // The camera thread.
    void capture();
    bool configureCamera(Handle& bufferError, s16& transferUnit);
    bool receiveInto(Handle& received, uint16_t* buffer, s16 transferUnit);
    void publish(const uint16_t* frame);

    // The decode thread.
    void decode();
    // Waits up to timeoutNs for the stop signal; true once it is given.
    bool stopping(s64 timeoutNs) const;

    // Sticky, so that both threads see it.
    Handle stopEvent = 0;

    // The camera writes frames here by DMA, taking turns, so one is read while the next comes in.
    uint16_t* dma[2] = {};

    // The newest frame for latestFrame(), how many there have been and how many it has seen. The main thread only
    // tries the lock, so it never waits for the camera thread.
    Mutex frameMutex;
    std::unique_ptr<uint16_t[]> latest;
    uint32_t frames = 0;
    uint32_t framesShown = 0;

    // The decode thread's frame: the camera thread copies the next one in when it is wanted, and leaves it alone until
    // it is wanted again. No lock, as the decode thread runs below the main thread and mustn't hold one the main
    // thread could wait for.
    std::unique_ptr<uint16_t[]> decodeFrame;
    std::atomic<bool> decodeWants{true};

    // The decode thread's own.
    quirc* qr = nullptr;
    std::unique_ptr<DecodeScratch> scratch;

    // Written once, before found is set.
    std::string resultText;
    std::atomic<bool> found{false};

    bool cameraOpen = false;
    Thread captureThread;
    Thread decodeThread;
};

QrScanner::Impl::~Impl()
{
    // Both threads watch the stop event, and the camera thread turns the camera off on its way out.
    if (stopEvent)
        svcSignalEvent(stopEvent);
    decodeThread.join();
    captureThread.join();

    // the driver goes before the buffers, so that nothing can still be writing to them
    if (cameraOpen)
        camExit();
    for (uint16_t* buffer : dma)
    {
        if (buffer)
            linearFree(buffer);
    }
    if (qr)
        quirc_destroy(qr);
    if (stopEvent)
        svcCloseHandle(stopEvent);
}

// On failure the destructor undoes whatever was done.
Expected<void> QrScanner::Impl::open()
{
    const Result created = svcCreateEvent(&stopEvent, RESET_STICKY);
    if (R_FAILED(created))
    {
        stopEvent = 0;
        return fail(Errc::OutOfMemory, "stop event", created);
    }

    for (uint16_t*& buffer : dma)
        buffer = static_cast<uint16_t*>(linearAlloc(kFrameBytes));
    latest.reset(new (std::nothrow) uint16_t[kPixels]);
    decodeFrame.reset(new (std::nothrow) uint16_t[kPixels]);
    scratch.reset(new (std::nothrow) DecodeScratch);
    qr = quirc_new();
    if (!dma[0] || !dma[1] || !latest || !decodeFrame || !scratch || !qr || quirc_resize(qr, kWidth, kHeight) < 0)
        return fail(Errc::OutOfMemory, "QR scanner buffers");

    const Result opened = camInit();
    if (R_FAILED(opened))
        return fail(cameraError(opened));
    cameraOpen = true;

    captureThread = Thread::start([this] { capture(); }, kCapturePriority, kSystemCore, kCaptureStack);
    if (!captureThread.running())
        return fail(Errc::OutOfMemory, "camera thread");
    decodeThread = Thread::start([this] { decode(); }, priority::kWorker, kAppCore, kDecodeStack);
    if (!decodeThread.running())
        return fail(Errc::OutOfMemory, "QR decode thread");
    return {};
}

void QrScanner::Impl::capture()
{
    // What the thread waits for: the stop signal, the end of a frame's transfer, the camera port's buffer overflowing.
    Handle events[3] = {stopEvent, 0, 0};
    Handle& received = events[1];
    Handle& bufferError = events[2];
    s16 transferUnit = 0;
    int filling = 0; // the DMA buffer the camera writes to

    bool capturing = configureCamera(bufferError, transferUnit) && receiveInto(received, dma[filling], transferUnit) &&
                     check(CAMU_StartCapture(PORT_CAM1), "start capturing");
    while (capturing)
    {
        s32 index = -1;
        if (!check(svcWaitSynchronizationN(&index, events, 3, false, U64_MAX), "wait for the camera") || index == 0)
            break;

        // each transfer has an event of its own
        svcCloseHandle(received);
        received = 0;
        if (index == 1)
        {
            // the camera carries on into the other buffer while this frame is handed over
            const uint16_t* frame = dma[filling];
            filling ^= 1;
            capturing = receiveInto(received, dma[filling], transferUnit);
            // The CPU's caches may still hold lines of this buffer from before the transfer; dropping them makes sure
            // the frame is read as the camera wrote it. The buffers are whole cache lines, so nothing else is lost.
            GSPGPU_InvalidateDataCache(frame, kFrameBytes);
            publish(frame);
        }
        else
        {
            // The next transfer wasn't set up in time, the port's buffer overflowed and capturing stopped: start
            // again. Clearing the event matters only if the camera module made it sticky.
            svcClearEvent(bufferError);
            CAMU_ClearBuffer(PORT_CAM1);
            capturing = receiveInto(received, dma[filling], transferUnit) &&
                        check(CAMU_StartCapture(PORT_CAM1), "restart capturing");
        }
    }

    // Off again, after the frame in flight, so that nothing is being written when the buffers are freed.
    CAMU_StopCapture(PORT_CAM1);
    bool busy = true;
    for (int i = 0; i < kStopPolls && R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy; ++i)
        sleepMs(1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    if (received)
        svcCloseHandle(received);
    if (bufferError)
        svcCloseHandle(bufferError);
}

bool QrScanner::Impl::configureCamera(Handle& bufferError, s16& transferUnit)
{
    // The top screen's size, in the GPU's RGB565. The outer camera's pictures come upright and unmirrored, rows from
    // the top, as latestFrame() promises.
    if (!check(CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A), "set the picture size") ||
        !check(CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A), "set the picture format"))
        return false;

    // Picture quality: worth asking for, not worth giving up over.
    check(CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30), "set the frame rate");
    check(CAMU_SetNoiseFilter(SELECT_OUT1, true), "turn on the noise filter");
    check(CAMU_SetAutoExposure(SELECT_OUT1, true), "turn on auto exposure");
    check(CAMU_SetAutoWhiteBalance(SELECT_OUT1, true), "turn on auto white balance");
    // a little sharpening: crisper module edges make the finder patterns easier for quirc to find
    check(CAMU_SetSharpness(SELECT_OUT1, 1), "set the sharpness");

    if (!check(CAMU_Activate(SELECT_OUT1), "turn on the camera"))
        return false;
    if (!check(CAMU_GetBufferErrorInterruptEvent(&bufferError, PORT_CAM1), "get the buffer error event"))
    {
        bufferError = 0; // not a handle when the call failed
        return false;
    }

    // Whole pictures, moved in the biggest pieces the port allows.
    u32 maxBytes = 0;
    if (!check(CAMU_SetTrimming(PORT_CAM1, false), "turn off trimming") ||
        !check(CAMU_GetMaxBytes(&maxBytes, kWidth, kHeight), "get the transfer size"))
        return false;
    // CAMU_SetReceiving takes it as an s16
    if (maxBytes == 0 || maxBytes > static_cast<u32>(INT16_MAX))
    {
        LOG_ERROR("qr: unexpected camera transfer size %lu", static_cast<unsigned long>(maxBytes));
        return false;
    }
    transferUnit = static_cast<s16>(maxBytes);
    return check(CAMU_SetTransferBytes(PORT_CAM1, maxBytes, kWidth, kHeight), "set the transfer size") &&
           check(CAMU_ClearBuffer(PORT_CAM1), "clear the camera buffer");
}

// Sets up the transfer of the next frame into buffer.
bool QrScanner::Impl::receiveInto(Handle& received, uint16_t* buffer, s16 transferUnit)
{
    if (check(CAMU_SetReceiving(&received, buffer, PORT_CAM1, kFrameBytes, transferUnit), "receive a frame"))
        return true;
    received = 0; // not a handle when the call failed
    return false;
}

void QrScanner::Impl::publish(const uint16_t* frame)
{
    {
        std::lock_guard lock(frameMutex);
        std::memcpy(latest.get(), frame, kFrameBytes);
        ++frames;
    }
    if (decodeWants.load(std::memory_order_acquire))
    {
        std::memcpy(decodeFrame.get(), frame, kFrameBytes);
        decodeWants.store(false, std::memory_order_release);
    }
}

void QrScanner::Impl::decode()
{
    while (!stopping(kDecodeIntervalNs))
    {
        if (decodeWants.load(std::memory_order_acquire))
            continue; // no frame since the last one
        toGrey(decodeFrame.get(), quirc_begin(qr, nullptr, nullptr));
        // quirc has its own copy now, so the next frame can come in while it looks
        decodeWants.store(true, std::memory_order_release);

        quirc_end(qr);
        std::optional<std::string> text = readCodes(qr, *scratch);
        if (!text)
            continue;

        LOG_INFO("qr: read a code of %zu bytes", text->size());
        resultText = std::move(*text);
        found.store(true, std::memory_order_release);
        // The first code is the answer. The camera stays on for the picture until the scanner goes.
        return;
    }
}

bool QrScanner::Impl::stopping(s64 timeoutNs) const
{
    // Anything but a timeout ends the wait for good, a failed wait included, so a bad handle can't leave the thread
    // spinning.
    return R_DESCRIPTION(svcWaitSynchronization(stopEvent, timeoutNs)) != RD_TIMEOUT;
}

Expected<std::unique_ptr<QrScanner>> QrScanner::start()
{
    auto impl = std::make_unique<Impl>();
    TZ_CHECK(impl->open());
    return std::unique_ptr<QrScanner>(new QrScanner(std::move(impl)));
}

QrScanner::QrScanner(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

// Impl's destructor stops the threads and the camera.
QrScanner::~QrScanner() = default;

bool QrScanner::latestFrame(std::vector<uint16_t>& frame)
{
    // Sized first, so that the camera thread never waits for an allocation. While the camera thread is copying a
    // frame in, this one reports nothing new rather than wait; the next call gets it.
    frame.resize(kPixels);
    std::unique_lock lock(impl_->frameMutex, std::try_to_lock);
    if (!lock.owns_lock() || impl_->frames == impl_->framesShown)
        return false;
    std::memcpy(frame.data(), impl_->latest.get(), kFrameBytes);
    impl_->framesShown = impl_->frames;
    return true;
}

std::optional<std::string> QrScanner::result() const
{
    if (!impl_->found.load(std::memory_order_acquire))
        return std::nullopt;
    return impl_->resultText;
}

} // namespace tz
