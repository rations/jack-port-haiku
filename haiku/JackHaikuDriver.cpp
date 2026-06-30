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

#include "JackHaikuDriver.h"
#include "JackCompilerDeps.h"
#include "JackDriverLoader.h"
#include "JackEngineControl.h"
#include "JackError.h"
#include "JackGraphManager.h"
#include "JackTime.h"
#include "driver_interface.h"

#include <MediaDefs.h>
#include <SoundPlayer.h>

#include <string.h>

using namespace std;

namespace Jack
{

void JackHaikuDriver::PlayBuffer(void* cookie, void* buffer, size_t size, const media_raw_audio_format& /*format*/)
{
    static_cast<JackHaikuDriver*>(cookie)->PlayBufferAux(buffer, size);
}

void JackHaikuDriver::PlayBufferAux(void* buffer, size_t size)
{
    // Runs on the Media Kit's playback thread; route its logging into JACK.
    set_threaded_log_function();

    const int channels = fPlaybackChannels;
    const jack_nframes_t period = fEngineControl->fBufferSize;
    const size_t frame_bytes = channels * sizeof(jack_default_audio_sample_t);
    const jack_nframes_t total_frames = size / frame_bytes;
    jack_default_audio_sample_t* out = (jack_default_audio_sample_t*)buffer;

    // The Media Kit hands us a fixed-size buffer that we requested to be a
    // multiple of the JACK period: run one process cycle per period and
    // interleave the playback ports into the corresponding slice.
    jack_nframes_t done = 0;
    while (done + period <= total_frames) {
        fOutputBuffer = out + done * channels;
        CycleTakeBeginTime();
        if (Process() < 0) {
            break;
        }
        done += period;
    }

    // Output silence for any frames we could not fill (remainder or on error),
    // so the speakers get clean audio rather than stale/garbage samples.
    if (done < total_frames) {
        memset(out + done * channels, 0, (total_frames - done) * frame_bytes);
    }
}

int JackHaikuDriver::Write()
{
    const int channels = fPlaybackChannels;
    const jack_nframes_t frames = fEngineControl->fBufferSize;

    // Interleave each JACK output port into the Media Kit's interleaved buffer.
    for (int c = 0; c < channels; c++) {
        jack_default_audio_sample_t* src = GetOutputBuffer(c);
        jack_default_audio_sample_t* dst = fOutputBuffer + c;
        for (jack_nframes_t f = 0; f < frames; f++) {
            dst[f * channels] = src[f];
        }
    }
    return 0;
}

void JackHaikuDriver::UpdateLatencies()
{
    jack_latency_range_t range;

    for (int i = 0; i < fPlaybackChannels; i++) {
        range.min = range.max = (fEngineControl->fSyncMode)
                                    ? fEngineControl->fBufferSize
                                    : fEngineControl->fBufferSize * 2;
        range.min += fPlaybackLatency;
        range.max += fPlaybackLatency;
        fGraphManager->GetPort(fPlaybackPortList[i])->SetLatencyRange(JackPlaybackLatency, &range);
    }
}

int JackHaikuDriver::Open(jack_nframes_t buffer_size,
                          jack_nframes_t samplerate,
                          bool capturing,
                          bool playing,
                          int inchannels,
                          int outchannels,
                          bool monitor,
                          const char* capture_driver_name,
                          const char* playback_driver_name,
                          jack_nframes_t capture_latency,
                          jack_nframes_t playback_latency)
{
    if (capturing) {
        jack_info("JackHaikuDriver: BSoundPlayer is playback-only, ignoring capture request");
    }
    if (outchannels == 0) {
        outchannels = 2;
    }

    // Generic JackAudioDriver Open: playback only, no capture ports.
    if (JackAudioDriver::Open(buffer_size, samplerate, false, true, 0, outchannels, monitor,
                              capture_driver_name, playback_driver_name, capture_latency, playback_latency) != 0) {
        return -1;
    }

    media_raw_audio_format format;
    format.frame_rate = (float)samplerate;
    format.channel_count = outchannels;
    format.format = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order = B_MEDIA_HOST_ENDIAN;
    format.buffer_size = buffer_size * outchannels * sizeof(jack_default_audio_sample_t);

    fSoundPlayer = new BSoundPlayer(&format, "jackd", PlayBuffer, NULL, this);

    status_t err = fSoundPlayer->InitCheck();
    if (err != B_OK) {
        jack_error("JackHaikuDriver::Open: BSoundPlayer init failed err = %s", strerror(err));
        delete fSoundPlayer;
        fSoundPlayer = NULL;
        JackAudioDriver::Close();
        return -1;
    }

    // The media server may adapt the format it actually delivers. Our
    // interleaving (Write) and per-period processing assume the channel count
    // and 32-bit float sample type we asked for, so verify what was negotiated
    // rather than trusting that the request was honored.
    media_raw_audio_format neg = fSoundPlayer->Format();
    if (neg.format != media_raw_audio_format::B_AUDIO_FLOAT || (int)neg.channel_count != outchannels) {
        jack_error("JackHaikuDriver::Open: unexpected negotiated format (channels = %u, format = 0x%x); expected %d float channels",
                   neg.channel_count, (unsigned)neg.format, outchannels);
        delete fSoundPlayer;
        fSoundPlayer = NULL;
        JackAudioDriver::Close();
        return -1;
    }

    return 0;
}

int JackHaikuDriver::Close()
{
    jack_log("JackHaikuDriver::Close");
    JackAudioDriver::Close();
    if (fSoundPlayer) {
        fSoundPlayer->Stop();
        delete fSoundPlayer;
        fSoundPlayer = NULL;
    }
    return 0;
}

int JackHaikuDriver::Start()
{
    jack_log("JackHaikuDriver::Start");
    if (JackAudioDriver::Start() < 0) {
        return -1;
    }

    status_t err = fSoundPlayer->Start();
    if (err != B_OK) {
        jack_error("JackHaikuDriver::Start: BSoundPlayer start failed err = %s", strerror(err));
        JackAudioDriver::Stop();
        return -1;
    }
    fSoundPlayer->SetHasData(true);
    return 0;
}

int JackHaikuDriver::Stop()
{
    jack_log("JackHaikuDriver::Stop");
    if (fSoundPlayer) {
        fSoundPlayer->SetHasData(false);
        fSoundPlayer->Stop();
    }
    return JackAudioDriver::Stop();
}

} // namespace Jack

#ifdef __cplusplus
extern "C" {
#endif

SERVER_EXPORT jack_driver_desc_t* driver_get_descriptor()
{
    jack_driver_desc_t* desc;
    jack_driver_desc_filler_t filler;
    jack_driver_param_value_t value;

    desc = jack_driver_descriptor_construct("haiku", JackDriverMaster, "Haiku Media Kit (BSoundPlayer) audio backend", &filler);

    value.ui = 2U;
    jack_driver_descriptor_add_parameter(desc, &filler, "channels", 'c', JackDriverParamUInt, &value, NULL, "Number of playback channels", NULL);
    jack_driver_descriptor_add_parameter(desc, &filler, "outchannels", 'o', JackDriverParamUInt, &value, NULL, "Number of playback channels", NULL);

    value.ui = 44100U;
    jack_driver_descriptor_add_parameter(desc, &filler, "rate", 'r', JackDriverParamUInt, &value, NULL, "Sample rate", NULL);

    value.ui = 1024U;
    jack_driver_descriptor_add_parameter(desc, &filler, "period", 'p', JackDriverParamUInt, &value, NULL, "Frames per period", NULL);

    value.i = 0;
    jack_driver_descriptor_add_parameter(desc, &filler, "monitor", 'm', JackDriverParamBool, &value, NULL, "Provide monitor ports for the output", NULL);

    return desc;
}

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    jack_nframes_t srate = 44100;
    jack_nframes_t frames_per_interrupt = 1024;
    int chan_out = 2;
    bool monitor = false;
    const JSList* node;
    const jack_driver_param_t* param;

    for (node = params; node; node = jack_slist_next(node)) {
        param = (const jack_driver_param_t*)node->data;

        switch (param->character) {

        case 'c':
        case 'o':
            chan_out = (int)param->value.ui;
            break;

        case 'r':
            srate = param->value.ui;
            break;

        case 'p':
            frames_per_interrupt = (unsigned int)param->value.ui;
            break;

        case 'm':
            monitor = param->value.i;
            break;
        }
    }

    Jack::JackDriverClientInterface* driver = new Jack::JackHaikuDriver("system", "haiku", engine, table);
    if (driver->Open(frames_per_interrupt, srate, false, true, 0, chan_out, monitor, "haiku", "haiku", 0, 0) == 0) {
        return driver;
    } else {
        delete driver;
        return NULL;
    }
}

#ifdef __cplusplus
}
#endif
