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

#include "JackHmultiDriver.h"
#include "JackDriverLoader.h"
#include "JackEngineControl.h"
#include "JackError.h"
#include "JackGraphManager.h"
#include "JackThreadedDriver.h"
#include "driver_interface.h"
#include "memops.h"

#include <MediaDefs.h>
#include <MediaRoster.h>
#include <OS.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Jack
{

static const char* kHmultiRoot = "/dev/audio/hmulti";

// shutdown_media_server()/launch_media_server() post a Deskbar progress
// notification when no callback is supplied; that builds a BBitmap and needs an
// app_server connection, which the headless jackd server does not have (it would
// abort). Passing a no-op callback selects the non-notifying code path. Per the
// Media Kit contract this callback must currently always return true.
static bool media_progress_noop(int /*stage*/, const char* /*message*/, void* /*cookie*/)
{
    return true;
}

// Map a plain frame rate to the multi_audio single-bit rate selector. Returns 0
// if the rate has no selector (caller must reject it rather than guess).
static uint32 rate_to_multi(jack_nframes_t rate)
{
    switch (rate) {
    case 8000:
        return B_SR_8000;
    case 11025:
        return B_SR_11025;
    case 12000:
        return B_SR_12000;
    case 16000:
        return B_SR_16000;
    case 22050:
        return B_SR_22050;
    case 24000:
        return B_SR_24000;
    case 32000:
        return B_SR_32000;
    case 44100:
        return B_SR_44100;
    case 48000:
        return B_SR_48000;
    case 64000:
        return B_SR_64000;
    case 88200:
        return B_SR_88200;
    case 96000:
        return B_SR_96000;
    case 176400:
        return B_SR_176400;
    case 192000:
        return B_SR_192000;
    case 384000:
        return B_SR_384000;
    default:
        return 0;
    }
}

// Pick the best integer hardware format supported by both directions we use.
// Float is intentionally not selected: the shared memops converters target
// integer hardware buffers, and most Haiku drivers expose integer formats.
static uint32 select_format(uint32 mask)
{
    if (mask & B_FMT_32BIT) {
        return B_FMT_32BIT;
    }
    // 24-bit samples travel MSB-justified in a 32-bit container: the
    // multi_audio contract maps B_FMT_24BIT to B_AUDIO_INT with 24 valid
    // bits, and USB Audio Type I subslots are left-justified. This is the
    // same layout as the memops "d32u24" converters.
    if (mask & B_FMT_24BIT) {
        return B_FMT_24BIT;
    }
    if (mask & B_FMT_16BIT) {
        return B_FMT_16BIT;
    }
    return 0;
}

static size_t format_bytes(uint32 format)
{
    switch (format) {
    case B_FMT_32BIT:
    case B_FMT_24BIT:
        return 4;
    case B_FMT_16BIT:
        return 2;
    default:
        return 0;
    }
}

// Recursively search /dev/audio/hmulti for the first usable device node.
int JackHmultiDriver::DiscoverDevice(char* path, size_t size)
{
    // The hmulti tree is shallow (root/driver/index); a single level of
    // recursion via an explicit stack would suffice, but a bounded recursive
    // walk keeps it simple and matches the multi_audio media add-on.
    DIR* dir = opendir(kHmultiRoot);
    if (dir == NULL) {
        jack_error("JackHmultiDriver: cannot open %s: %s", kHmultiRoot, strerror(errno));
        return -1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char driverDir[B_PATH_NAME_LENGTH];
        snprintf(driverDir, sizeof(driverDir), "%s/%s", kHmultiRoot, entry->d_name);

        DIR* sub = opendir(driverDir);
        if (sub == NULL) {
            continue;
        }
        struct dirent* node;
        while ((node = readdir(sub)) != NULL) {
            if (strcmp(node->d_name, ".") == 0 || strcmp(node->d_name, "..") == 0) {
                continue;
            }
            snprintf(path, size, "%s/%s", driverDir, node->d_name);
            closedir(sub);
            closedir(dir);
            return 0;
        }
        closedir(sub);
    }

    closedir(dir);
    jack_error("JackHmultiDriver: no device found under %s", kHmultiRoot);
    return -1;
}

int JackHmultiDriver::OpenDevice(const char* device)
{
    char path[B_PATH_NAME_LENGTH];
    if (device != NULL && device[0] != '\0' && strcmp(device, "hmulti") != 0) {
        strncpy(path, device, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else if (DiscoverDevice(path, sizeof(path)) != 0) {
        return -1;
    }

    // The reference multi_audio add-on opens the device O_WRONLY even for
    // duplex; record still flows through the buffer exchange.
    fDevice = open(path, O_WRONLY);
    if (fDevice < 0 && BMediaRoster::IsRunning()) {
        // The media services own the hmulti device exclusively (the multi_audio
        // add-on opens it at boot), so the first open fails while they are up.
        // Perform the standard, reversible handoff the Media preferences use:
        // shut the media services down, take the device, and relaunch them in
        // Close(). This mirrors stopping PulseAudio before starting JACK.
        jack_info("JackHmultiDriver: %s is busy; handing it off from the media services", path);
        if (shutdown_media_server(B_INFINITE_TIMEOUT, media_progress_noop, NULL) == B_OK) {
            fMediaServerStopped = true;
            // The device is released asynchronously as the add-on server exits,
            // so retry the open briefly. Not the RT path, so snoozing is fine.
            for (int i = 0; i < 20 && fDevice < 0; i++) {
                snooze(50000);
                fDevice = open(path, O_WRONLY);
            }
        }
    }
    if (fDevice < 0) {
        jack_error("JackHmultiDriver: cannot open %s: %s", path, strerror(errno));
        RestoreMediaServer();
        return -1;
    }
    jack_info("JackHmultiDriver: opened %s", path);

    // Describe the device. request_channel_count bounds how many channel_infos
    // the driver may write into our array.
    memset(&fDescription, 0, sizeof(fDescription));
    fDescription.info_size = sizeof(fDescription);
    fDescription.request_channel_count = HMULTI_MAX_CHANNELS;
    fDescription.channels = fChannelInfo;
    if (ioctl(fDevice, B_MULTI_GET_DESCRIPTION, &fDescription, sizeof(fDescription)) != 0) {
        jack_error("JackHmultiDriver: B_MULTI_GET_DESCRIPTION failed: %s", strerror(errno));
        goto fail;
    }

    // Validate the driver-reported channel counts before we rely on them.
    if (fDescription.output_channel_count < 0 || fDescription.input_channel_count < 0 || fDescription.output_channel_count + fDescription.input_channel_count > HMULTI_MAX_CHANNELS) {
        jack_error("JackHmultiDriver: device reports too many channels (out %d, in %d)",
                   (int)fDescription.output_channel_count, (int)fDescription.input_channel_count);
        goto fail;
    }

    jack_info("JackHmultiDriver: '%s' (%d out, %d in)", fDescription.friendly_name,
              (int)fDescription.output_channel_count, (int)fDescription.input_channel_count);

    return 0;

fail:
    close(fDevice);
    fDevice = -1;
    RestoreMediaServer();
    return -1;
}

// Relaunch the media services if this driver stopped them in OpenDevice, so a
// failed or closed session leaves the system's audio exactly as it found it.
void JackHmultiDriver::RestoreMediaServer()
{
    if (fMediaServerStopped) {
        jack_info("JackHmultiDriver: restarting media services");
        launch_media_server(B_INFINITE_TIMEOUT, media_progress_noop, NULL);
        fMediaServerStopped = false;
    }
}

int JackHmultiDriver::SetupBuffers(jack_nframes_t buffer_size, int playback_channels, int capture_channels)
{
    for (uint32 i = 0; i < HMULTI_MAX_BUFFERS; i++) {
        fPlayBuffers[i] = &fPlayBufferDescs[i * HMULTI_MAX_CHANNELS];
        fRecordBuffers[i] = &fRecordBufferDescs[i * HMULTI_MAX_CHANNELS];
    }

    // Ask for the caller's period; the driver is free to return a different
    // (typically fixed) buffer size, which the caller then adopts as the JACK
    // period. Hardware that honours the request (e.g. hda) yields low latency;
    // hardware that pins its DMA buffer (e.g. auich) dictates the period.
    memset(&fBufferList, 0, sizeof(fBufferList));
    fBufferList.info_size = sizeof(fBufferList);
    fBufferList.request_playback_buffers = fRequestedBuffers;
    fBufferList.request_playback_channels = playback_channels;
    fBufferList.request_playback_buffer_size = buffer_size;
    fBufferList.playback_buffers = fPlayBuffers;
    fBufferList.request_record_buffers = fRequestedBuffers;
    fBufferList.request_record_channels = capture_channels;
    fBufferList.request_record_buffer_size = buffer_size;
    fBufferList.record_buffers = fRecordBuffers;

    if (ioctl(fDevice, B_MULTI_GET_BUFFERS, &fBufferList, sizeof(fBufferList)) != 0) {
        jack_error("JackHmultiDriver: B_MULTI_GET_BUFFERS failed: %s", strerror(errno));
        return -1;
    }

    // Bounds-check what the driver returned before indexing the buffer arrays.
    if (fBufferList.return_playback_buffers < 1 || fBufferList.return_playback_buffers > HMULTI_MAX_BUFFERS || fBufferList.return_record_buffers > HMULTI_MAX_BUFFERS || fBufferList.return_playback_channels < playback_channels || (capture_channels > 0 && fBufferList.return_record_channels < capture_channels)) {
        jack_error("JackHmultiDriver: unexpected buffer geometry (pb %d x %d, rec %d x %d)",
                   (int)fBufferList.return_playback_buffers,
                   (int)fBufferList.return_playback_channels,
                   (int)fBufferList.return_record_buffers,
                   (int)fBufferList.return_record_channels);
        return -1;
    }

    // One buffer exchange drives exactly one JACK period, so capture and
    // playback must share a single buffer size. JACK has one period for both
    // directions; a device that offered different sizes could not be clocked
    // coherently, so reject that rather than guess.
    if (capture_channels > 0 && fBufferList.return_record_buffer_size != fBufferList.return_playback_buffer_size) {
        jack_error("JackHmultiDriver: playback/record buffer sizes differ (%u vs %u)",
                   (unsigned)fBufferList.return_playback_buffer_size,
                   (unsigned)fBufferList.return_record_buffer_size);
        return -1;
    }

    jack_info("JackHmultiDriver: %d playback buffers x %d ch x %u frames",
              (int)fBufferList.return_playback_buffers,
              (int)fBufferList.return_playback_channels,
              (unsigned)fBufferList.return_playback_buffer_size);

    return 0;
}

void JackHmultiDriver::ZeroPlaybackBuffers()
{
    for (int32 b = 0; b < fBufferList.return_playback_buffers; b++) {
        for (int c = 0; c < fPlaybackChannels; c++) {
            char* base = fBufferList.playback_buffers[b][c].base;
            size_t stride = fBufferList.playback_buffers[b][c].stride;
            for (jack_nframes_t f = 0; f < fEngineControl->fBufferSize; f++) {
                memset(base + f * stride, 0, fSampleBytes);
            }
        }
    }
}

int JackHmultiDriver::Open(jack_nframes_t buffer_size,
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
    if (OpenDevice(playback_driver_name) != 0) {
        return -1;
    }

    // Default to the device's channel counts when the user did not pin them.
    if (!capturing) {
        inchannels = 0;
    } else if (inchannels == 0 || inchannels > fDescription.input_channel_count) {
        inchannels = fDescription.input_channel_count;
    }
    if (!playing) {
        outchannels = 0;
    } else if (outchannels == 0 || outchannels > fDescription.output_channel_count) {
        outchannels = fDescription.output_channel_count;
    }

    // Verify the requested rate is one the device offers (never assume).
    uint32 rateBit = rate_to_multi(samplerate);
    if (rateBit == 0 || (fDescription.output_rates & rateBit) == 0 || (inchannels > 0 && (fDescription.input_rates & rateBit) == 0)) {
        jack_error("JackHmultiDriver: device does not support %u Hz", (unsigned)samplerate);
        goto fail;
    }

    fSampleFormat = select_format(fDescription.output_formats & fDescription.input_formats);
    if (fSampleFormat == 0) {
        // Fall back to the playback formats if capture is unused or disjoint.
        fSampleFormat = select_format(fDescription.output_formats);
    }
    fSampleBytes = format_bytes(fSampleFormat);
    if (fSampleBytes == 0) {
        jack_error("JackHmultiDriver: no supported sample format (out 0x%x, in 0x%x)",
                   (unsigned)fDescription.output_formats, (unsigned)fDescription.input_formats);
        goto fail;
    }

    // Enable every channel we will use and lock to the internal clock.
    {
        multi_channel_enable enable;
        uint32 enableBits = 0;
        memset(&enable, 0, sizeof(enable));
        enable.info_size = sizeof(enable);
        enable.enable_bits = (uchar*)&enableBits;
        int channelCount = fDescription.output_channel_count + fDescription.input_channel_count;
        for (int i = 0; i < channelCount; i++) {
            B_SET_CHANNEL(&enableBits, i, true);
        }
        enable.lock_source = B_MULTI_LOCK_INTERNAL;
        if (ioctl(fDevice, B_MULTI_SET_ENABLED_CHANNELS, &enable, sizeof(enable)) != 0) {
            jack_error("JackHmultiDriver: B_MULTI_SET_ENABLED_CHANNELS failed: %s", strerror(errno));
            goto fail;
        }
    }

    // Negotiate the global format, then read it back to confirm.
    memset(&fFormatInfo, 0, sizeof(fFormatInfo));
    fFormatInfo.info_size = sizeof(fFormatInfo);
    fFormatInfo.output.rate = rateBit;
    fFormatInfo.output.format = fSampleFormat;
    fFormatInfo.input.rate = rateBit;
    fFormatInfo.input.format = fSampleFormat;
    if (ioctl(fDevice, B_MULTI_SET_GLOBAL_FORMAT, &fFormatInfo, sizeof(fFormatInfo)) != 0) {
        jack_error("JackHmultiDriver: B_MULTI_SET_GLOBAL_FORMAT failed: %s", strerror(errno));
        goto fail;
    }
    if (ioctl(fDevice, B_MULTI_GET_GLOBAL_FORMAT, &fFormatInfo, sizeof(fFormatInfo)) != 0) {
        jack_error("JackHmultiDriver: B_MULTI_GET_GLOBAL_FORMAT failed: %s", strerror(errno));
        goto fail;
    }
    if (fFormatInfo.output.rate != rateBit || fFormatInfo.output.format != fSampleFormat) {
        jack_error("JackHmultiDriver: device negotiated a different format than requested");
        goto fail;
    }

    // Set the DMA buffers up before registering with the engine: the device may
    // pin its own buffer size, and that size (not the requested one) becomes the
    // JACK period, since one buffer exchange clocks exactly one cycle.
    if (SetupBuffers(buffer_size, outchannels, inchannels) != 0) {
        goto fail;
    }

    {
        jack_nframes_t device_period = fBufferList.return_playback_buffer_size;
        if (device_period != buffer_size) {
            jack_info("JackHmultiDriver: device fixed the period at %u frames (requested %u); "
                      "adopting the device buffer size",
                      (unsigned)device_period, (unsigned)buffer_size);
        }

        // Register with the engine using the device's real period so the whole
        // graph runs at the buffer size the hardware actually clocks.
        if (JackAudioDriver::Open(device_period, samplerate, capturing, playing, inchannels,
                                  outchannels, monitor, capture_driver_name, playback_driver_name,
                                  capture_latency, playback_latency) != 0) {
            goto fail;
        }
    }

    return 0;

fail:
    close(fDevice);
    fDevice = -1;
    RestoreMediaServer();
    return -1;
}

int JackHmultiDriver::Close()
{
    jack_log("JackHmultiDriver::Close");
    int res = JackAudioDriver::Close();
    if (fDevice >= 0) {
        close(fDevice);
        fDevice = -1;
    }
    // Relaunch the media services only after our handle is closed, so the
    // add-on server can reclaim the device.
    RestoreMediaServer();
    return res;
}

int JackHmultiDriver::Start()
{
    jack_log("JackHmultiDriver::Start");
    if (JackAudioDriver::Start() < 0) {
        return -1;
    }

    // Start from silence: the first exchange submits whatever is in the
    // playback buffers, so they must be cleared before the cycle begins.
    ZeroPlaybackBuffers();

    memset(&fBufferInfo, 0, sizeof(fBufferInfo));
    fBufferInfo.info_size = sizeof(fBufferInfo);
    fPlaybackCycle = 0;
    return 0;
}

int JackHmultiDriver::Stop()
{
    jack_log("JackHmultiDriver::Stop");
    if (fDevice >= 0) {
        ioctl(fDevice, B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
    }
    return JackAudioDriver::Stop();
}

int JackHmultiDriver::Read()
{
    // Blocks until the next DMA swap: this is what paces the JACK graph.
    if (ioctl(fDevice, B_MULTI_BUFFER_EXCHANGE, &fBufferInfo, sizeof(fBufferInfo)) != 0) {
        // A signal interrupting the blocking exchange means the server is
        // shutting the thread down, not that the device failed.
        if (errno == B_INTERRUPTED) {
            jack_log("JackHmultiDriver: buffer exchange interrupted, stopping");
        } else {
            jack_error("JackHmultiDriver: B_MULTI_BUFFER_EXCHANGE failed: %s", strerror(errno));
        }
        return -1;
    }

    JackDriver::CycleIncTime();

    const jack_nframes_t frames = fEngineControl->fBufferSize;

    // Buffer-cycle convention, verified empirically on usb_audio (hardware
    // loopback + jack_iodelay, which reads garbage/zero if this is wrong):
    // the kernel completion handler releases the ready-semaphore, then
    // advances its current buffer and queues the next transfer. So at exchange
    // return, record_buffer_cycle is the buffer the hardware just finished
    // filling (freshest capture -- read it directly), while the playback slot
    // safe to write is the one behind the reported cycle (the reported one is
    // already in flight to the hardware).
    const int32 playBuffers = fBufferList.return_playback_buffers;
    if (fBufferInfo.playback_buffer_cycle >= 0 && fBufferInfo.playback_buffer_cycle < playBuffers) {
        fPlaybackCycle = (fBufferInfo.playback_buffer_cycle - 1 + playBuffers) % playBuffers;
    }

    // Pull captured audio for this cycle into the JACK input ports.
    if (fCaptureChannels > 0) {
        const int32 recBuffers = fBufferList.return_record_buffers;
        int32 recCycle = fBufferInfo.record_buffer_cycle;
        if (recCycle >= 0 && recCycle < recBuffers) {
            for (int c = 0; c < fCaptureChannels; c++) {
                char* src = fBufferList.record_buffers[recCycle][c].base;
                size_t stride = fBufferList.record_buffers[recCycle][c].stride;
                jack_default_audio_sample_t* dst = GetInputBuffer(c);
                if (fSampleFormat == B_FMT_16BIT) {
                    sample_move_dS_s16(dst, src, frames, stride);
                } else if (fSampleFormat == B_FMT_24BIT) {
                    sample_move_dS_s32u24(dst, src, frames, stride);
                } else {
                    sample_move_dS_s32(dst, src, frames, stride);
                }
            }
        }
    }

    return 0;
}

int JackHmultiDriver::Write()
{
    const jack_nframes_t frames = fEngineControl->fBufferSize;

    for (int c = 0; c < fPlaybackChannels; c++) {
        char* dst = fBufferList.playback_buffers[fPlaybackCycle][c].base;
        size_t stride = fBufferList.playback_buffers[fPlaybackCycle][c].stride;
        jack_default_audio_sample_t* src = GetOutputBuffer(c);
        if (fSampleFormat == B_FMT_16BIT) {
            sample_move_d16_sS(dst, src, frames, stride, NULL);
        } else if (fSampleFormat == B_FMT_24BIT) {
            sample_move_d32u24_sS(dst, src, frames, stride, NULL);
        } else {
            sample_move_d32_sS(dst, src, frames, stride, NULL);
        }
    }

    return 0;
}

void JackHmultiDriver::UpdateLatencies()
{
    jack_latency_range_t range;

    for (int i = 0; i < fCaptureChannels; i++) {
        range.min = range.max = fEngineControl->fBufferSize + fCaptureLatency;
        fGraphManager->GetPort(fCapturePortList[i])->SetLatencyRange(JackCaptureLatency, &range);
    }
    for (int i = 0; i < fPlaybackChannels; i++) {
        range.min = range.max = (fEngineControl->fSyncMode)
                                    ? fEngineControl->fBufferSize
                                    : fEngineControl->fBufferSize * 2;
        range.min += fPlaybackLatency;
        range.max += fPlaybackLatency;
        fGraphManager->GetPort(fPlaybackPortList[i])->SetLatencyRange(JackPlaybackLatency, &range);
    }
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

    desc = jack_driver_descriptor_construct(
        "hmulti", JackDriverMaster, "Haiku low-latency hmulti_audio backend", &filler);

    value.ui = 0U;
    jack_driver_descriptor_add_parameter(desc, &filler, "inchannels", 'i', JackDriverParamUInt, &value, NULL, "Number of capture channels (0 = device max)", NULL);
    jack_driver_descriptor_add_parameter(desc, &filler, "outchannels", 'o', JackDriverParamUInt, &value, NULL, "Number of playback channels (0 = device max)", NULL);

    value.ui = 48000U;
    jack_driver_descriptor_add_parameter(desc, &filler, "rate", 'r', JackDriverParamUInt, &value, NULL, "Sample rate", NULL);

    value.ui = 1024U;
    jack_driver_descriptor_add_parameter(desc, &filler, "period", 'p', JackDriverParamUInt, &value, NULL, "Frames per period", NULL);

    value.ui = 8U;
    jack_driver_descriptor_add_parameter(desc, &filler, "nperiods", 'n', JackDriverParamUInt, &value, NULL, "Number of periods (buffers) to request; device may override", NULL);

    value.i = 1;
    jack_driver_descriptor_add_parameter(desc, &filler, "capture", 'C', JackDriverParamBool, &value, NULL, "Enable capture", NULL);
    jack_driver_descriptor_add_parameter(desc, &filler, "playback", 'P', JackDriverParamBool, &value, NULL, "Enable playback", NULL);

    value.str[0] = '\0';
    jack_driver_descriptor_add_parameter(desc, &filler, "device", 'd', JackDriverParamString, &value, NULL, "Device path (default: first under /dev/audio/hmulti)", NULL);

    return desc;
}

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    jack_nframes_t srate = 48000;
    jack_nframes_t frames_per_interrupt = 1024;
    int nperiods = HMULTI_MAX_BUFFERS;
    int chan_in = 0;
    int chan_out = 0;
    bool capture = true;
    bool playback = true;
    const char* device = "hmulti";
    const JSList* node;
    const jack_driver_param_t* param;

    for (node = params; node; node = jack_slist_next(node)) {
        param = (const jack_driver_param_t*)node->data;

        switch (param->character) {

        case 'i':
            chan_in = (int)param->value.ui;
            break;

        case 'o':
            chan_out = (int)param->value.ui;
            break;

        case 'r':
            srate = param->value.ui;
            break;

        case 'p':
            frames_per_interrupt = (unsigned int)param->value.ui;
            break;

        case 'n':
            nperiods = (int)param->value.ui;
            break;

        case 'C':
            capture = param->value.i;
            break;

        case 'P':
            playback = param->value.i;
            break;

        case 'd':
            device = param->value.str;
            break;
        }
    }

    Jack::JackHmultiDriver* hmulti_driver = new Jack::JackHmultiDriver("system", "hmulti", engine, table);
    hmulti_driver->SetRequestedBuffers(nperiods);
    Jack::JackDriverClientInterface* threaded_driver = new Jack::JackThreadedDriver(hmulti_driver);
    if (hmulti_driver->Open(frames_per_interrupt, srate, capture, playback, chan_in, chan_out, false,
                            device, device, 0, 0) == 0) {
        return threaded_driver;
    } else {
        delete threaded_driver;
        return NULL;
    }
}

#ifdef __cplusplus
}
#endif
