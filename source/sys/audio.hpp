// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Music: theme previews and the app's own background music, played as BCSTM through the DSP.

#pragma once

#include "core/io.hpp"

#include <memory>

namespace tz::audio
{

// Starts the DSP for the app's lifetime. Without the DSP firmware (sdmc:/3ds/dspfirm.cdc, which DSP1 dumps) there is
// no sound; available() then stays false and the app says so where it would play music.
bool init();
void exit();
bool available();

enum class State : uint8_t
{
    Loading, // reading the start of the music (or waiting for its download)
    Playing,
    Finished,
    Failed,
};

// Plays one BCSTM from a stream, on a thread of its own, reading ahead only as far as it plays: a 3 MB theme song
// starts after its first blocks are read, whether from the SD card, a zip or a download in progress.
class Player
{
public:
    struct Options
    {
        float volume = 1.0f;
        // Pauses the background music while this one plays.
        bool interruptsBackground = true;
    };

    // Never blocks: reading and playing happen on the player's thread. nullptr without the DSP.
    static std::unique_ptr<Player> play(std::unique_ptr<StreamReader> source, Options options);
    ~Player(); // stops at once and frees its DSP channels

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    State state() const;
    // Why it failed, when state() is Failed.
    Error error() const;
    void setPaused(bool paused);

    struct Impl;

private:
    explicit Player(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Themezer's own music, from a BCSTM in the RomFS, looping at half volume. It pauses while a Player with
// interruptsBackground plays, and carries on a moment after the last one stops.
void startBackgroundMusic(const char* path);
void stopBackgroundMusic();
bool backgroundMusicRunning();

} // namespace tz::audio
