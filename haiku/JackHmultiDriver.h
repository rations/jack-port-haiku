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

#ifndef __JackHmultiDriver__
#define __JackHmultiDriver__

#include "JackAudioDriver.h"

#include <hmulti_audio.h>

namespace Jack
{

// Storage bounds for the driver-owned copies of the multi_audio structures.
// The driver reports its own counts; we only need arrays at least that large
// and validate the returned counts against these before indexing (Rule: every
// driver-supplied size is bounds-checked before use).
#define HMULTI_MAX_CHANNELS 32
#define HMULTI_MAX_BUFFERS 8

/*!
\brief The Haiku audio driver, stage 2: low-latency I/O via the hmulti_audio
device (/dev/audio/hmulti).

Unlike the BSoundPlayer backend (JackHaikuDriver), this talks to the kernel
audio driver directly, bypassing the system mixer to get low latency, more than
two channels, and full-duplex capture. It is thread-driven like the ALSA
backend: the driver is wrapped in a JackThreadedDriver whose RT thread loops
Process() = Read() -> graph -> Write(). The blocking B_MULTI_BUFFER_EXCHANGE
ioctl in Read() paces the JACK cycle (one exchange == one period).

The hmulti buffers are per-channel with a byte stride, and the hardware sample
format is integer (commonly 16- or 32-bit), so Read()/Write() convert between
JACK's 32-bit float and the negotiated format with the shared memops routines.
*/

class JackHmultiDriver : public JackAudioDriver
{

private:
    int fDevice;

    // Driver-owned copies of the multi_audio negotiation state. Allocated once
    // (never on the RT path) and reused for every buffer exchange.
    multi_description fDescription;
    multi_channel_info fChannelInfo[HMULTI_MAX_CHANNELS];
    multi_format_info fFormatInfo;
    multi_buffer_list fBufferList;
    multi_buffer_info fBufferInfo;

    buffer_desc fPlayBufferDescs[HMULTI_MAX_BUFFERS * HMULTI_MAX_CHANNELS];
    buffer_desc fRecordBufferDescs[HMULTI_MAX_BUFFERS * HMULTI_MAX_CHANNELS];
    buffer_desc* fPlayBuffers[HMULTI_MAX_BUFFERS];
    buffer_desc* fRecordBuffers[HMULTI_MAX_BUFFERS];

    // Negotiated hardware sample format (a single B_FMT_* bit) and its width.
    uint32 fSampleFormat;
    size_t fSampleBytes;

    // Playback cycle chosen by the most recent Read(); filled by Write().
    int32 fPlaybackCycle;

    // The buffer cycle processed by the previous Read(), used to collapse the
    // duplicate buffer-exchange returns a duplex device produces (one per
    // stream) into a single JACK cycle per period. -1 until the first Read().
    int32 fLastExchangeCycle;

    // The record buffer read by the previous Read(). Capture is consumed
    // sequentially from this cursor, not from the reported (freshest) cycle:
    // a device-clocked driver occasionally completes two record buffers in
    // one period (or none), and reading only the freshest would skip one
    // buffer of audio at each such burst. -1 until the first capture read.
    int32 fRecordReadCycle;

    // Buffer (period) count requested from the device; the device may return
    // a different count, which SetupBuffers adopts after bounds-checking.
    int32 fRequestedBuffers;

    // True if Open() shut the media services down to take the device; Close()
    // then relaunches them (see OpenDevice/RestoreMediaServer).
    bool fMediaServerStopped;

    int OpenDevice(const char* device);
    int DiscoverDevice(char* path, size_t size);
    int SetupBuffers(jack_nframes_t buffer_size, int playback_channels, int capture_channels);
    void ZeroPlaybackBuffers();
    void RestoreMediaServer();
    void UpdateLatencies();

public:
    JackHmultiDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
        : JackAudioDriver(name, alias, engine, table),
          fDevice(-1),
          fSampleFormat(0),
          fSampleBytes(0),
          fPlaybackCycle(0),
          fLastExchangeCycle(-1),
          fRecordReadCycle(-1),
          fRequestedBuffers(HMULTI_MAX_BUFFERS),
          fMediaServerStopped(false)
    {
    }
    virtual ~JackHmultiDriver()
    {
    }

    // Must be called before Open; clamped to [2, HMULTI_MAX_BUFFERS].
    void SetRequestedBuffers(int32 buffers)
    {
        if (buffers < 2) {
            buffers = 2;
        }
        if (buffers > HMULTI_MAX_BUFFERS) {
            buffers = HMULTI_MAX_BUFFERS;
        }
        fRequestedBuffers = buffers;
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

    int Read();
    int Write();

    // The hmulti buffer size is fixed at Open by what the device accepts.
    // TODO (Phase 3): renegotiate buffers to support runtime resize.
    bool IsFixedBufferSize()
    {
        return true;
    }
};

} // namespace Jack

#endif
