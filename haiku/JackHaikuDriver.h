/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2004-2008 Grame

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef __JackHaikuDriver__
#define __JackHaikuDriver__

#include "JackAudioDriver.h"

#include <cstddef>

class BSoundPlayer;
struct media_raw_audio_format;

namespace Jack
{

/*!
\brief The Haiku audio driver, stage 1: playback via the Media Kit BSoundPlayer.

BSoundPlayer is a callback-based, playback-only convenience class that routes
through the system mixer. It is the "easy audible win" backend; the low-latency,
multi-channel, full-duplex backend (hmulti_audio) is a later stage. The Media
Kit drives the cycle: its buffer-fill hook runs one or more JACK process cycles
and interleaves the resulting playback port buffers into the supplied buffer.
*/

class JackHaikuDriver : public JackAudioDriver
{

private:
    BSoundPlayer* fSoundPlayer;
    // Interleaved playback buffer currently provided by the Media Kit hook;
    // valid only for the duration of one PlayBufferAux() call.
    jack_default_audio_sample_t* fOutputBuffer;

    static void PlayBuffer(void* cookie, void* buffer, size_t size, const media_raw_audio_format& format);
    void PlayBufferAux(void* buffer, size_t size);
    void UpdateLatencies();

public:
    JackHaikuDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
        : JackAudioDriver(name, alias, engine, table), fSoundPlayer(NULL), fOutputBuffer(NULL)
    {
    }
    virtual ~JackHaikuDriver()
    {
    }

    int Open(jack_nframes_t buffer_size,
             jack_nframes_t samplerate,
             bool capturing,
             bool playing,
             int inchannels,
             int outchannels,
             bool monitor,
             const char* capture_driver_name,
             const char* playback_driver_name,
             jack_nframes_t capture_latency,
             jack_nframes_t playback_latency);
    int Close();

    int Start();
    int Stop();

    // Output-only backend: no capture is performed.
    int Read()
    {
        return 0;
    }
    int Write();
};

} // namespace Jack

#endif
