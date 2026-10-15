// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// BCSTM streamed to the DSP. A player's thread reads the music a block at a time while the DSP plays it, keeping a
// few seconds queued in wave buffers; the DSP decodes DSP-ADPCM itself, so the thread only copies blocks and sleeps.
//
// Each channel of the music has an NDSP channel of its own (NDSP has 24). The two channels of stereo music start and
// pause in the same sound frame, so they never drift apart.
//
// Looping goes back to the block holding the loop start: the wave buffer after the loop starts at the loop start's
// DSP-ADPCM frame (its very sample for PCM), with the decoder state the file gives for it. A source is read once, from
// front to back, and the blocks from the loop start on are kept in memory as they go by: a theme's music is at most
// bcstm::kMaxSize, so that's at most ~3 MB of normal memory. Seeking back instead isn't something every source can do
// (a download), and where it works it can cost far more than it seems: a zip entry inflates again from its start.
// The one exception is the background music, read whole into memory by this file, which loops by seeking back.
//
// The main thread never waits for a player's thread: stopping a player silences and frees its channels at once, and
// its thread, which may be waiting for a download, finishes by itself and is joined later.

#include "sys/audio.hpp"

#include "core/bcstm.hpp"
#include "sys/log.hpp"
#include "sys/thread.hpp"

#include <3ds.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace tz::audio
{

namespace
{

constexpr int kNdspChannels = 24;
// Previews take the first free channels from 0; the background music has 20 to 23 to itself.
constexpr int kBackgroundChannels = 20;

// Per channel, wave buffers of a block each holding about this much sound, within these bounds: 7 blocks of
// DSP-ADPCM at 32 kHz.
constexpr uint64_t kQueueSeconds = 3;
constexpr uint64_t kMinBuffers = 4;
constexpr uint64_t kMaxBuffers = 16;
// Encoders write blocks of 0x2000 bytes; much bigger ones would make the queue take too much linear memory.
constexpr uint32_t kMaxBlockSize = 0x10000;
// The most kept in memory for a loop: a theme's whole music. Longer loops play once.
constexpr uint64_t kMaxLoopMemory = bcstm::kMaxSize;

// How often the thread looks for played buffers. A block lasts almost half a second.
constexpr s64 kPollNs = 10 * 1000000LL;
// After a channel is reset, the DSP may go on reading its buffers until NDSP sends it the next frame (one every
// 4.9 ms): two frames before they're freed.
constexpr u32 kDspCatchUpMs = 10;
// The background music carries on this long after the last preview stops, so it doesn't sound between two of them.
constexpr u64 kBackgroundResumeTicks = SYSCLOCK_ARM11 / 3;
constexpr float kBackgroundVolume = 0.5f;

// Fades, in seconds. A preview comes in at once, so its first notes aren't lost, and goes out softly; the
// background music makes way more slowly, and comes back slower still.
constexpr float kPreviewFadeIn = 0.1f;
constexpr float kPreviewFadeOut = 0.35f;
constexpr float kPauseFade = 0.2f;
constexpr float kBackgroundFadeIn = 0.9f;
constexpr float kBackgroundFadeOut = 0.3f;
// NDSP sends the DSP a frame of 160 samples at 32728 Hz: this many a second.
constexpr float kSoundFramesPerSecond = 32728.0f / 160.0f;
constexpr size_t kStackSize = 64 * 1024;

// A DSP-ADPCM frame: a byte of predictor and scale, then 14 samples of 4 bits.
constexpr uint32_t kAdpcmFrameSamples = 14;
constexpr uint32_t kAdpcmFrameBytes = 8;

// A player's state, shared by the Player and its thread, which can outlive it for a moment.
struct Shared
{
    Player::Options options;
    // The background music: channels of its own, paused by previews, and a FileInMemory for a source.
    bool background = false;
    std::unique_ptr<StreamReader> source; // until the thread takes it

    std::atomic<State> state{State::Loading};
    std::atomic<bool> paused{false}; // as setPaused() asks
    std::atomic<bool> stopping{false};
    std::atomic<bool> exited{false};
    std::atomic<bool> holdsBackground{false};
    // The Player is gone and its music fades out: stopping once the tick is reached.
    std::atomic<bool> fadingOut{false};
    std::atomic<u64> stopTick{0};

    Mutex mutex;
    ConditionVariable wake;
    bool woken = false;
    Error error;
    // The NDSP channels it holds. Whichever stops first, the thread or ~Player, gives them back, and the thread only
    // touches them with the mutex held, so a channel is never used once another player may have it.
    std::array<int, 2> channels{};
    int channelCount = 0;
};

std::atomic<bool> ready{false};
std::atomic<bool> exiting{false};
std::atomic<uint32_t> usedChannels{0};

// Players with interruptsBackground that haven't finished, and when the background music may carry on.
Mutex holdMutex;
int interruptions = 0;
u64 resumeTick = 0;

// The Players that exist, for exit().
Mutex playersMutex;
std::vector<Shared*> players;

// The threads of destroyed Players that hadn't finished yet.
struct Orphan
{
    Thread thread;
    std::shared_ptr<Shared> shared;
};
Mutex orphansMutex;
std::vector<Orphan> orphans;

Mutex backgroundMutex;
std::unique_ptr<Player> backgroundPlayer;

// Pausing and resuming. NDSP sends the DSP each channel's changes in turn, every sound frame (4.9 ms): two channels
// paused or resumed one after the other could be sent a frame apart, and stereo music would stay a frame out of step.
// So changes to playing channels are made in NDSP's frame callback, which runs on its thread right before it sends a
// frame: all of them reach the DSP in the same one.
Mutex pauseMutex;
uint32_t pausedChannels = 0; // as wanted
uint32_t pendingPauses = 0;  // channels whose pause changes at the next frame

// Volume fades go the same way, a step each sound frame, so they go on whatever the players' threads are doing (one
// may be waiting for a download).
struct ChannelFade
{
    std::array<float, 12> mix{}; // the channel's mix at full volume
    float gain = 1.0f;
    float target = 1.0f;
    float step = 0.0f;       // of gain, each frame
    bool pauseAtEnd = false; // paused once silent
};
std::array<ChannelFade, kNdspChannels> fades;
uint32_t fadingChannels = 0;

// pauseMutex must be held.
void applyMix(int channel)
{
    const ChannelFade& fade = fades[channel];
    float mix[12];
    for (size_t i = 0; i < 12; ++i)
        mix[i] = fade.mix[i] * fade.gain;
    ndspChnSetMix(channel, mix);
}

void onFrame(void*)
{
    // never makes NDSP's thread wait: when a player holds the lock, the changes go with the next frame
    if (!pauseMutex.try_lock())
        return;
    for (uint32_t pending = pendingPauses; pending != 0; pending &= pending - 1)
    {
        const int channel = std::countr_zero(pending);
        ndspChnSetPaused(channel, (pausedChannels >> channel) & 1);
    }
    pendingPauses = 0;
    for (uint32_t fading = fadingChannels; fading != 0; fading &= fading - 1)
    {
        const int channel = std::countr_zero(fading);
        ChannelFade& fade = fades[channel];
        fade.gain = fade.gain < fade.target ? std::min(fade.target, fade.gain + fade.step)
                                            : std::max(fade.target, fade.gain - fade.step);
        applyMix(channel);
        if (fade.gain != fade.target)
            continue;
        fadingChannels &= ~(1u << channel);
        if (fade.pauseAtEnd && fade.gain == 0)
        {
            pausedChannels |= 1u << channel;
            ndspChnSetPaused(channel, true);
        }
    }
    pauseMutex.unlock();
}

// A channel's mix at full volume, and how loud it starts: 0 for one that fades in.
void startMix(int channel, const float (&mix)[12], float gain)
{
    std::lock_guard lock(pauseMutex);
    ChannelFade& fade = fades[channel];
    std::copy(std::begin(mix), std::end(mix), fade.mix.begin());
    fade.gain = fade.target = gain;
    fade.pauseAtEnd = false;
    fadingChannels &= ~(1u << channel);
    applyMix(channel);
}

// Fades the player's channels to target over seconds, from where they are; when pauseAtEnd, they pause once silent.
// The channels must be the player's (shared.mutex held, or the thread's own).
void fadeChannels(const Shared& shared, float target, float seconds, bool pauseAtEnd)
{
    std::lock_guard lock(pauseMutex);
    for (int i = 0; i < shared.channelCount; ++i)
    {
        const int channel = shared.channels[i];
        ChannelFade& fade = fades[channel];
        fade.target = target;
        fade.step = 1.0f / std::max(1.0f, seconds * kSoundFramesPerSecond);
        fade.pauseAtEnd = pauseAtEnd;
        fadingChannels |= 1u << channel;
    }
}

// Pauses or resumes channels together, with the next sound frame.
void pauseWithNextFrame(const Shared& shared, bool paused)
{
    std::lock_guard lock(pauseMutex);
    for (int i = 0; i < shared.channelCount; ++i)
    {
        const uint32_t bit = 1u << shared.channels[i];
        pausedChannels = paused ? pausedChannels | bit : pausedChannels & ~bit;
        pendingPauses |= bit;
    }
}

// Pauses or resumes a channel at once: for one with nothing playing, before buffers are queued on it. A change still
// waiting for the next frame is dropped.
void pauseNow(int channel, bool paused)
{
    std::lock_guard lock(pauseMutex);
    const uint32_t bit = 1u << channel;
    pausedChannels = paused ? pausedChannels | bit : pausedChannels & ~bit;
    pendingPauses &= ~bit;
    ndspChnSetPaused(channel, paused);
}

// Before a channel is reset and given back: nothing waiting for the next frame may reach whoever takes it next.
void forgetPause(int channel)
{
    std::lock_guard lock(pauseMutex);
    pausedChannels &= ~(1u << channel);
    pendingPauses &= ~(1u << channel);
    fadingChannels &= ~(1u << channel);
}

int claimChannel(int first, int end)
{
    uint32_t used = usedChannels.load();
    for (;;)
    {
        int channel = first;
        while (channel < end && (used & (1u << channel)))
            ++channel;
        if (channel == end)
            return -1;
        if (usedChannels.compare_exchange_weak(used, used | (1u << channel)))
            return channel;
    }
}

// Silences the player's channels and gives them back. shared.mutex must be held.
void releaseChannels(Shared& shared)
{
    for (int i = 0; i < shared.channelCount; ++i)
    {
        forgetPause(shared.channels[i]);
        ndspChnWaveBufClear(shared.channels[i]);
        ndspChnReset(shared.channels[i]);
        usedChannels.fetch_and(~(1u << shared.channels[i]));
    }
    shared.channelCount = 0;
}

void requestStop(Shared& shared)
{
    std::lock_guard lock(shared.mutex);
    shared.stopping = true;
    releaseChannels(shared);
    shared.woken = true;
    shared.wake.notifyAll();
}

// Lets music that's playing fade out first; anything else stops at once.
void fadeOutAndStop(Shared& shared, float seconds)
{
    {
        std::lock_guard lock(shared.mutex);
        if (shared.state == State::Playing && shared.channelCount > 0 && !shared.stopping)
        {
            fadeChannels(shared, 0.0f, seconds, true);
            shared.stopTick = svcGetSystemTick() + static_cast<u64>((seconds + 0.05f) * SYSCLOCK_ARM11);
            shared.fadingOut = true;
            shared.woken = true;
            shared.wake.notifyAll();
            return;
        }
    }
    requestStop(shared);
}

void holdBackground(Shared& shared)
{
    if (shared.holdsBackground.exchange(true))
        return;
    std::lock_guard lock(holdMutex);
    ++interruptions;
}

void releaseBackground(Shared& shared)
{
    if (!shared.holdsBackground.exchange(false))
        return;
    std::lock_guard lock(holdMutex);
    --interruptions;
    resumeTick = svcGetSystemTick() + kBackgroundResumeTicks;
}

bool backgroundHeld()
{
    std::lock_guard lock(holdMutex);
    return interruptions > 0 || svcGetSystemTick() < resumeTick;
}

// NDSP's thread marks the wave buffers it has played: read the status from memory every time.
u8 statusOf(const ndspWaveBuf& buffer)
{
    const u8 status = *static_cast<const volatile u8*>(&buffer.status);
    std::atomic_thread_fence(std::memory_order_acquire);
    return status;
}

u16 formatOf(bcstm::Encoding encoding)
{
    switch (encoding)
    {
    case bcstm::Encoding::Pcm8:
        return NDSP_FORMAT_MONO_PCM8;
    case bcstm::Encoding::Pcm16:
        return NDSP_FORMAT_MONO_PCM16;
    case bcstm::Encoding::DspAdpcm:
        return NDSP_FORMAT_MONO_ADPCM;
    }
    return NDSP_FORMAT_MONO_PCM16;
}

const char* encodingName(bcstm::Encoding encoding)
{
    switch (encoding)
    {
    case bcstm::Encoding::Pcm8:
        return "PCM8";
    case bcstm::Encoding::Pcm16:
        return "PCM16";
    case bcstm::Encoding::DspAdpcm:
        return "DSP-ADPCM";
    }
    return "?";
}

// What a player's thread does, from reading the header to the last buffer played.
class Playback
{
public:
    Playback(Shared& shared, std::unique_ptr<StreamReader> source) : shared_(shared), source_(std::move(source)) {}
    ~Playback();
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    // Plays to the end of music that doesn't loop, or until the player is stopped.
    Expected<void> run();

private:
    enum class Context : uint8_t
    {
        None,  // the DSP goes on from the previous buffer
        Start, // the decoder state at the start of the music
        Loop,  // the decoder state at the loop start
    };

    Expected<void> readHeader();
    Expected<void> prepare();
    Expected<void> openChannels();
    Expected<void> refill();
    Expected<void> fill(uint32_t slot);
    Expected<void> readBlock(uint32_t block, int channel, uint8_t* out, uint32_t size);
    Expected<void> readAt(uint64_t offset, MutableByteSpan out);
    bool slotFree(uint32_t slot) const;
    bool queueEmpty() const;
    void applyPause(bool idle = false);
    void wait();
    uint32_t bytesBefore(uint32_t sample) const;

    Shared& shared_;
    std::unique_ptr<StreamReader> source_;
    Bytes header_; // the start of the file, which also holds the first block
    bcstm::Info info_;

    bool seekable_ = false; // loops by seeking back in the source instead of keeping the loop in memory
    bool loops_ = false;
    uint32_t loopBlock_ = 0; // the block holding the loop start
    uint32_t loopFirst_ = 0; // the loop start within it, on a frame for DSP-ADPCM

    // the next buffer to queue: a block, from which sample in it, with which decoder state
    uint32_t block_ = 0;
    uint32_t first_ = 0;
    Context context_ = Context::Start;
    bool ended_ = false; // everything is queued

    // Blocks from loopBlock_ on, kept as they're first read: the channels of each block in turn, blockSize apart.
    // cachedBlocks_ of them are there.
    std::unique_ptr<uint8_t[]> cache_;
    uint32_t cachedBlocks_ = 0;

    // Linear memory for the wave buffers: per slot, per channel, stride_ bytes each.
    uint8_t* memory_ = nullptr;
    size_t stride_ = 0;
    uint32_t slotCount_ = 0;
    uint32_t nextSlot_ = 0;
    bool queuedAny_ = false;
    std::array<std::array<ndspWaveBuf, 2>, kMaxBuffers> waveBufs_{};
    // NDSP copies a buffer's decoder state when it sends the buffer to the DSP, frames after it's queued: these stay.
    std::array<ndspAdpcmData, 2> startContext_{};
    std::array<ndspAdpcmData, 2> loopContext_{};

    bool holding_ = true; // paused until the queue is full
    bool channelsPaused_ = true;
};

Playback::~Playback()
{
    {
        std::lock_guard lock(shared_.mutex);
        releaseChannels(shared_);
    }
    if (memory_)
    {
        if (queuedAny_)
            sleepMs(kDspCatchUpMs);
        linearFree(memory_);
    }
    // the source goes with the members: a download stops there
}

Expected<void> Playback::run()
{
    TZ_CHECK(readHeader());
    if (shared_.stopping)
        return {};
    info_ = TZ_TRY(bcstm::parse(header_));
    TZ_CHECK(prepare());
    TZ_CHECK(openChannels());
    LOG_INFO("music: %s, %u channel(s), %u Hz, %s", encodingName(info_.encoding), unsigned(info_.channels),
             unsigned(info_.sampleRate),
             !loops_ ? "once" : seekable_ ? "looping from the source" : "looping from memory");

    while (!shared_.stopping)
    {
        if (shared_.fadingOut && svcGetSystemTick() >= shared_.stopTick)
            break;
        applyPause();
        TZ_CHECK(refill());
        applyPause();
        if (ended_ && queueEmpty())
            break;
        wait();
    }
    return {};
}

// The first kHeaderSize bytes, or the whole file when it's shorter.
Expected<void> Playback::readHeader()
{
    header_.resize(bcstm::kHeaderSize);
    size_t length = 0;
    while (length < header_.size() && !shared_.stopping)
    {
        const size_t got = TZ_TRY(source_->read(MutableByteSpan(header_).subspan(length)));
        if (got == 0)
            break;
        length += got;
    }
    header_.resize(length);
    return {};
}

Expected<void> Playback::prepare()
{
    if (info_.blockSize > kMaxBlockSize)
        return fail(Errc::Unsupported, "blocks of " + std::to_string(info_.blockSize) + " bytes");

    loopBlock_ = info_.loopStart / info_.blockSamples;
    loopFirst_ = info_.loopStart % info_.blockSamples;
    if (info_.encoding == bcstm::Encoding::DspAdpcm)
        loopFirst_ -= loopFirst_ % kAdpcmFrameSamples; // a wave buffer starts on a frame

    // The background music's source is the whole file in memory (FileInMemory), where going back costs nothing.
    seekable_ = shared_.background;

    loops_ = info_.looping;
    if (loops_ && !seekable_)
    {
        const uint32_t lastBlock = (info_.loopEnd - 1) / info_.blockSamples;
        const uint64_t size = uint64_t(lastBlock - loopBlock_ + 1) * info_.channels * info_.blockSize;
        if (size <= kMaxLoopMemory)
            cache_.reset(new (std::nothrow) uint8_t[static_cast<size_t>(size)]);
        if (!cache_)
        {
            LOG_WARN("music: no memory to keep its loop (%llu bytes), so it plays once",
                     static_cast<unsigned long long>(size));
            loops_ = false;
        }
    }

    const uint64_t blocks = (info_.sampleRate * kQueueSeconds + info_.blockSamples - 1) / info_.blockSamples;
    slotCount_ = static_cast<uint32_t>(std::clamp<uint64_t>(blocks, kMinBuffers, kMaxBuffers));

    for (int c = 0; c < info_.channels; ++c)
    {
        const bcstm::AdpcmChannel& adpcm = info_.adpcm[c];
        startContext_[c] = ndspAdpcmData{adpcm.predictorScale, adpcm.history1, adpcm.history2};
        loopContext_[c] = ndspAdpcmData{adpcm.loopPredictorScale, adpcm.loopHistory1, adpcm.loopHistory2};
    }
    return {};
}

Expected<void> Playback::openChannels()
{
    stride_ = (size_t(info_.blockSize) + 0x1F) & ~size_t(0x1F);
    memory_ = static_cast<uint8_t*>(linearAlloc(stride_ * slotCount_ * info_.channels));
    if (!memory_)
        return fail(Errc::OutOfMemory, "no linear memory for the music");

    const int first = shared_.background ? kBackgroundChannels : 0;
    const int end = shared_.background ? kNdspChannels : kBackgroundChannels;
    const float volume = std::max(shared_.options.volume, 0.0f);

    std::lock_guard lock(shared_.mutex);
    if (shared_.stopping)
        return {};
    for (int c = 0; c < info_.channels; ++c)
    {
        const int channel = claimChannel(first, end);
        if (channel < 0)
            return fail(Errc::Busy, "no free DSP channel"); // those claimed are given back with the others
        shared_.channels[c] = channel;
        shared_.channelCount = c + 1;

        ndspChnReset(channel);
        pauseNow(channel, true); // until the queue is full: then the channels start together
        ndspChnSetInterp(channel, NDSP_INTERP_POLYPHASE);
        ndspChnSetRate(channel, static_cast<float>(info_.sampleRate));
        ndspChnSetFormat(channel, formatOf(info_.encoding));

        // mono on both front speakers, stereo one channel each; nothing to the back or the aux outputs. Silent until
        // it fades in.
        float mix[12] = {};
        if (info_.channels == 1)
            mix[0] = mix[1] = volume;
        else
            mix[c] = volume;
        startMix(channel, mix, 0.0f);

        if (info_.encoding == bcstm::Encoding::DspAdpcm)
        {
            u16 coefficients[16];
            for (size_t i = 0; i < 16; ++i)
                coefficients[i] = static_cast<u16>(info_.adpcm[c].coefficients[i]);
            ndspChnSetAdpcmCoefs(channel, coefficients);
        }
    }
    return {};
}

// Queues blocks in the buffers the DSP has played. While holding, the channels stay paused until the queue is full,
// or everything is queued.
Expected<void> Playback::refill()
{
    while (!ended_ && slotFree(nextSlot_))
    {
        if (shared_.stopping)
            return {};
        TZ_CHECK(fill(nextSlot_));
        nextSlot_ = (nextSlot_ + 1) % slotCount_;
    }
    if (holding_)
    {
        holding_ = false;
        State loading = State::Loading;
        shared_.state.compare_exchange_strong(loading, State::Playing);
    }
    return {};
}

Expected<void> Playback::fill(uint32_t slot)
{
    const uint32_t block = block_;
    const bool last = block == info_.blockCount - 1;
    const uint32_t bytes = last ? info_.lastBlockSize : info_.blockSize;
    const uint64_t start = uint64_t(block) * info_.blockSamples;
    // up to the end of the block, or of the music when it ends (or loops) inside it
    const uint32_t end =
        static_cast<uint32_t>(std::min<uint64_t>(last ? info_.lastBlockSamples : info_.blockSamples,
                                                 info_.loopEnd - start));
    const uint32_t skip = bytesBefore(first_);

    for (int c = 0; c < info_.channels; ++c)
    {
        uint8_t* data = memory_ + (size_t(slot) * info_.channels + c) * stride_;
        TZ_CHECK(readBlock(block, c, data, bytes));
        DSP_FlushDataCache(data, bytes);

        ndspWaveBuf& buffer = waveBufs_[slot][c];
        buffer = ndspWaveBuf{};
        buffer.data_vaddr = data + skip;
        buffer.nsamples = end - first_;
        if (info_.encoding == bcstm::Encoding::DspAdpcm && context_ == Context::Start)
            buffer.adpcm_data = &startContext_[c];
        else if (info_.encoding == bcstm::Encoding::DspAdpcm && context_ == Context::Loop)
            buffer.adpcm_data = &loopContext_[c];
    }

    // The source fell behind while this block was read, and the DSP has played everything queued: hold until the
    // queue is full again, so the channels restart together and the music doesn't stutter a block at a time.
    if (!holding_ && queueEmpty())
    {
        holding_ = true;
        applyPause(true);
    }

    {
        std::lock_guard lock(shared_.mutex);
        if (shared_.stopping)
            return {};
        for (int c = 0; c < info_.channels; ++c)
            ndspChnWaveBufAdd(shared_.channels[c], &waveBufs_[slot][c]);
        queuedAny_ = true;
    }

    first_ = 0;
    context_ = Context::None;
    if (start + end < info_.loopEnd)
        ++block_;
    else if (loops_)
    {
        block_ = loopBlock_;
        first_ = loopFirst_;
        context_ = Context::Loop;
    }
    else
        ended_ = true;
    return {};
}

// One channel's part of a block: from the loop kept in memory, or from the source.
Expected<void> Playback::readBlock(uint32_t block, int channel, uint8_t* out, uint32_t size)
{
    uint8_t* kept = nullptr;
    if (cache_ && block >= loopBlock_)
    {
        kept = cache_.get() + (size_t(block - loopBlock_) * info_.channels + channel) * info_.blockSize;
        if (block - loopBlock_ < cachedBlocks_)
        {
            std::memcpy(out, kept, size);
            return {};
        }
    }

    // every block before the last is blockSize per channel; the last one's channels are lastBlockPaddedSize apart
    const uint64_t stride = block == info_.blockCount - 1 ? info_.lastBlockPaddedSize : info_.blockSize;
    const uint64_t offset =
        info_.dataOffset + uint64_t(block) * info_.blockSize * info_.channels + uint64_t(channel) * stride;
    TZ_CHECK(readAt(offset, MutableByteSpan(out, size)));

    if (kept)
    {
        // the first time through, blocks come in order
        std::memcpy(kept, out, size);
        if (channel == info_.channels - 1)
            cachedBlocks_ = block - loopBlock_ + 1;
    }
    return {};
}

// Reads at an offset in the file: what the header read holds comes from there, the rest from the source, skipping
// ahead to it (over the padding between the last block's channels), or going back to the loop for the background
// music.
Expected<void> Playback::readAt(uint64_t offset, MutableByteSpan out)
{
    if (offset < header_.size())
    {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(out.size(), header_.size() - offset));
        std::memcpy(out.data(), header_.data() + offset, count);
        out = out.subspan(count);
        offset += count;
    }
    if (out.empty())
        return {};
    if (source_->position() != offset)
        TZ_CHECK(source_->seek(offset));
    return source_->readExactly(out);
}

bool Playback::slotFree(uint32_t slot) const
{
    for (int c = 0; c < info_.channels; ++c)
    {
        const u8 status = statusOf(waveBufs_[slot][c]);
        if (status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING)
            return false;
    }
    return true;
}

bool Playback::queueEmpty() const
{
    for (uint32_t slot = 0; slot < slotCount_; ++slot)
    {
        if (!slotFree(slot))
            return false;
    }
    return true;
}

// Pauses or resumes the channels as the player should be: held while the queue fills, paused by setPaused(), and
// for the background music, while previews play. Together with the next sound frame, or at once for channels that
// have played everything queued.
void Playback::applyPause(bool idle)
{
    // silenced on purpose (paused, or the background music making way), it fades out before it pauses; waiting for
    // blocks, it pauses at once, with nothing left to play
    const bool silenced = shared_.paused || (shared_.background && backgroundHeld()) || shared_.fadingOut;
    const bool paused = holding_ || silenced;
    if (paused == channelsPaused_)
        return;
    std::lock_guard lock(shared_.mutex);
    if (shared_.stopping)
        return;
    if (paused && silenced)
    {
        // (a player that's gone is fading out already)
        if (!shared_.fadingOut)
            fadeChannels(shared_, 0.0f, shared_.background ? kBackgroundFadeOut : kPauseFade, true);
    }
    else
    {
        if (idle)
        {
            for (int i = 0; i < shared_.channelCount; ++i)
                pauseNow(shared_.channels[i], paused);
        }
        else
            pauseWithNextFrame(shared_, paused);
        if (!paused)
            fadeChannels(shared_, 1.0f, shared_.background ? kBackgroundFadeIn : kPreviewFadeIn, false);
    }
    channelsPaused_ = paused;
}

void Playback::wait()
{
    std::lock_guard lock(shared_.mutex);
    if (!shared_.woken && !shared_.stopping)
        shared_.wake.waitFor(shared_.mutex, kPollNs);
    shared_.woken = false;
}

// Where a sample starts in a channel's block. For DSP-ADPCM, sample is on a frame.
uint32_t Playback::bytesBefore(uint32_t sample) const
{
    switch (info_.encoding)
    {
    case bcstm::Encoding::Pcm8:
        return sample;
    case bcstm::Encoding::Pcm16:
        return sample * 2;
    case bcstm::Encoding::DspAdpcm:
        return sample / kAdpcmFrameSamples * kAdpcmFrameBytes;
    }
    return 0;
}

void finish(Shared& shared, const Expected<void>& result)
{
    // a preview that has finished lets the background music carry on, as one that's destroyed does
    releaseBackground(shared);
    std::lock_guard lock(shared.mutex);
    if (result)
        shared.state = State::Finished;
    else
    {
        shared.error = result.error();
        shared.state = State::Failed;
        if (!shared.stopping)
            log::writeError("music", shared.error);
    }
    shared.exited = true;
}

void threadMain(Shared& shared)
{
    Expected<void> result;
    {
        Playback playback(shared, std::move(shared.source));
        result = playback.run();
    } // its channels and buffers are freed, and the source destroyed
    finish(shared, result);
}

// Takes the thread of a destroyed player: joins it when it's done, or keeps it to join once it is. It may still be
// waiting for its source (a download), and the main thread doesn't wait for the network.
void adopt(Thread thread, std::shared_ptr<Shared> shared)
{
    if (!thread.running())
        return;
    if (shared->exited)
    {
        thread.join();
        return;
    }
    std::lock_guard lock(orphansMutex);
    orphans.push_back(Orphan{std::move(thread), std::move(shared)});
}

// Joins the orphaned threads that are done, or all of them, waiting.
void reapOrphans(bool all)
{
    std::vector<Orphan> done;
    {
        std::lock_guard lock(orphansMutex);
        for (size_t i = 0; i < orphans.size();)
        {
            if (!all && !orphans[i].shared->exited)
            {
                ++i;
                continue;
            }
            done.push_back(std::move(orphans[i]));
            if (i + 1 < orphans.size())
                orphans[i] = std::move(orphans.back());
            orphans.pop_back();
        }
    }
    // destroying them joins them, outside the lock
}

// A file read whole into memory the first time it's read from: on the player's thread, so the main thread never
// waits for the SD card. It can seek anywhere, so looping it keeps no second copy.
class FileInMemory final : public StreamReader
{
public:
    explicit FileInMemory(std::string path) : path_(std::move(path)) {}

    uint64_t size() const override { return size_; }

    Expected<size_t> read(MutableByteSpan out) override
    {
        TZ_CHECK(load());
        const size_t count = std::min<size_t>(out.size(), size_ - position_);
        if (count > 0)
            std::memcpy(out.data(), data_.get() + position_, count);
        position_ += count;
        return count;
    }

    Expected<void> seek(uint64_t position) override
    {
        TZ_CHECK(load());
        if (position > size_)
            return fail(Errc::InvalidData, "seek past the end");
        position_ = static_cast<size_t>(position);
        return {};
    }

    uint64_t position() const override { return position_; }

private:
    Expected<void> load()
    {
        if (data_)
            return {};
        std::FILE* file = std::fopen(path_.c_str(), "rb");
        if (!file)
            return fail(Errc::NotFound, "open " + path_);
        long length = -1;
        if (std::fseek(file, 0, SEEK_END) == 0)
            length = std::ftell(file);
        if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0)
        {
            std::fclose(file);
            return fail(Errc::Io, "size of " + path_);
        }
        if (static_cast<uint64_t>(length) > bcstm::kMaxSize)
        {
            std::fclose(file);
            return fail(Errc::TooLarge, path_ + " of " + std::to_string(length) + " bytes");
        }
        std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[static_cast<size_t>(length) + 1]);
        if (!data)
        {
            std::fclose(file);
            return fail(Errc::OutOfMemory, "reading " + path_);
        }
        const size_t got = std::fread(data.get(), 1, static_cast<size_t>(length), file);
        std::fclose(file);
        if (got != static_cast<size_t>(length))
            return fail(Errc::Io, "read " + path_);
        data_ = std::move(data);
        size_ = got;
        return {};
    }

    std::string path_;
    std::unique_ptr<uint8_t[]> data_;
    size_t size_ = 0;
    size_t position_ = 0;
};

} // namespace

struct Player::Impl
{
    std::shared_ptr<Shared> shared;
    Thread thread;

    static std::unique_ptr<Player> start(std::unique_ptr<StreamReader> source, Options options, bool background);
};

std::unique_ptr<Player> Player::Impl::start(std::unique_ptr<StreamReader> source, Options options, bool background)
{
    if (!ready)
        return nullptr;
    reapOrphans(false);

    auto impl = std::make_unique<Impl>();
    impl->shared = std::make_shared<Shared>();
    Shared& shared = *impl->shared;
    shared.options = options;
    shared.background = background;
    shared.source = std::move(source);
    if (options.interruptsBackground && !background)
        holdBackground(shared); // from now: the background music pauses while this one loads, too

    if (shared.source)
    {
        impl->thread = Thread::start([owner = impl->shared] { threadMain(*owner); }, priority::kAudio, 1, kStackSize);
        if (!impl->thread.running())
            finish(shared, fail(Errc::OutOfMemory, "no thread for the music"));
    }
    else
        finish(shared, fail(Errc::InvalidData, "no music to play"));

    {
        std::lock_guard lock(playersMutex);
        players.push_back(&shared);
    }
    return std::unique_ptr<Player>(new Player(std::move(impl)));
}

Player::Player(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Player::~Player()
{
    Shared& shared = *impl_->shared;
    {
        std::lock_guard lock(playersMutex);
        std::erase(players, &shared);
    }
    // its music fades out on its thread, which then ends by itself; when the app closes, everything stops at once
    if (exiting)
        requestStop(shared);
    else
        fadeOutAndStop(shared, shared.background ? kBackgroundFadeOut : kPreviewFadeOut);
    releaseBackground(shared);
    adopt(std::move(impl_->thread), impl_->shared);
    reapOrphans(false);
}

std::unique_ptr<Player> Player::play(std::unique_ptr<StreamReader> source, Options options)
{
    return Impl::start(std::move(source), options, false);
}

State Player::state() const
{
    return impl_->shared->state;
}

Error Player::error() const
{
    std::lock_guard lock(impl_->shared->mutex);
    return impl_->shared->error;
}

void Player::setPaused(bool paused)
{
    Shared& shared = *impl_->shared;
    shared.paused = paused;
    std::lock_guard lock(shared.mutex);
    shared.woken = true;
    shared.wake.notifyAll();
}

bool init()
{
    if (ready)
        return true;
    const Result result = ndspInit();
    if (R_FAILED(result))
    {
        LOG_WARN("no DSP (%08lX): no music without sdmc:/3ds/dspfirm.cdc", static_cast<unsigned long>(result));
        return false;
    }
    // the HOME Menu plays theme music in stereo, whatever the system's sound setting
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetCallback(onFrame, nullptr);
    exiting = false;
    ready = true;
    return true;
}

void exit()
{
    if (!ready)
        return;
    exiting = true;
    stopBackgroundMusic();
    {
        // players the app still holds go quiet now; their threads end by themselves
        std::lock_guard lock(playersMutex);
        for (Shared* shared : players)
            requestStop(*shared);
    }
    {
        // and so do those fading out
        std::lock_guard lock(orphansMutex);
        for (Orphan& orphan : orphans)
            requestStop(*orphan.shared);
    }
    reapOrphans(true);
    ready = false;
    ndspSetCallback(nullptr, nullptr);
    ndspExit();
}

bool available()
{
    return ready;
}

void startBackgroundMusic(const char* path)
{
    stopBackgroundMusic();
    Player::Options options;
    options.volume = kBackgroundVolume;
    options.interruptsBackground = false;
    std::unique_ptr<Player> player = Player::Impl::start(std::make_unique<FileInMemory>(path), options, true);
    std::lock_guard lock(backgroundMutex);
    backgroundPlayer = std::move(player);
}

void stopBackgroundMusic()
{
    std::unique_ptr<Player> player;
    {
        std::lock_guard lock(backgroundMutex);
        player = std::move(backgroundPlayer);
    }
    // destroyed here, outside the lock
}

bool backgroundMusicRunning()
{
    std::lock_guard lock(backgroundMutex);
    return backgroundPlayer && backgroundPlayer->state() != State::Failed;
}

} // namespace tz::audio
