/*
Copyright (C) 2009 Grame

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

#include "JackHaikuMidiDriver.h"
#include "JackDriverLoader.h"
#include "JackEngineControl.h"
#include "JackError.h"
#include "JackGraphManager.h"
#include "driver_interface.h"

#include <MidiRoster.h>

#include <string.h>

namespace Jack
{

// Per-port ring size. MIDI is low-bandwidth; 16 KiB absorbs several seconds
// of dense traffic including reasonable sysex dumps. Oversized events are
// dropped whole (never split) so record boundaries stay intact.
#define HAIKU_MIDI_RING_SIZE 16384

struct HaikuMidiRecordHeader {
    bigtime_t time;
    uint32 length;
};

// ---------------------------------------------------------------------------
// Input: midi2 event thread -> ring -> Read() on the JACK cycle
// ---------------------------------------------------------------------------

JackHaikuMidiInput::JackHaikuMidiInput(const char* name, BMidiProducer* source)
    : BMidiLocalConsumer(name), fSource(source)
{
    fRing = jack_ringbuffer_create(HAIKU_MIDI_RING_SIZE);
}

JackHaikuMidiInput::~JackHaikuMidiInput()
{
    if (fSource != NULL) {
        fSource->Disconnect(this);
        fSource->Release();
    }
    if (fRing != NULL) {
        jack_ringbuffer_free(fRing);
    }
}

void JackHaikuMidiInput::Data(uchar* data, size_t length, bool atomic, bigtime_t time)
{
    // midi2-kit thread context: lock-free single-producer write only.
    // Non-atomic (partial sysex) delivery is not supported yet; dropping it
    // whole is safer than forwarding a fragment as a complete event.
    if (!atomic || fRing == NULL) {
        return;
    }
    HaikuMidiRecordHeader header;
    header.time = time;
    header.length = (uint32)length;
    if (jack_ringbuffer_write_space(fRing) < sizeof(header) + length) {
        return; // full: drop, never block the kit's event thread
    }
    jack_ringbuffer_write(fRing, (const char*)&header, sizeof(header));
    jack_ringbuffer_write(fRing, (const char*)data, length);
}

// ---------------------------------------------------------------------------
// Output: Write() on the JACK cycle -> ring -> writer thread -> SprayData
// ---------------------------------------------------------------------------

JackHaikuMidiOutput::JackHaikuMidiOutput(const char* name, BMidiConsumer* destination)
    : fProducer(NULL), fDestination(destination), fConnected(false)
{
    fRing = jack_ringbuffer_create(HAIKU_MIDI_RING_SIZE);
    fProducer = new BMidiLocalProducer(name);
    if (fRing != NULL && fProducer->Connect(destination) == B_OK) {
        fConnected = true;
    }
}

JackHaikuMidiOutput::~JackHaikuMidiOutput()
{
    if (fProducer != NULL) {
        if (fConnected) {
            fProducer->Disconnect(fDestination);
        }
        fProducer->Release();
    }
    if (fDestination != NULL) {
        fDestination->Release();
    }
    if (fRing != NULL) {
        jack_ringbuffer_free(fRing);
    }
}

void JackHaikuMidiOutput::Drain()
{
    // Writer-thread context: locking and blocking are fine here.
    HaikuMidiRecordHeader header;
    uchar data[HAIKU_MIDI_RING_SIZE];

    while (jack_ringbuffer_read_space(fRing) >= sizeof(header)) {
        jack_ringbuffer_read(fRing, (char*)&header, sizeof(header));
        if (header.length > sizeof(data) || jack_ringbuffer_read_space(fRing) < header.length) {
            // Corrupted stream should be impossible (single reader/writer);
            // reset rather than spin on garbage.
            jack_ringbuffer_reset(fRing);
            return;
        }
        jack_ringbuffer_read(fRing, (char*)data, header.length);
        fProducer->SprayData(data, header.length, true, header.time);
    }
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

JackHaikuMidiDriver::JackHaikuMidiDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
    : JackMidiDriver(name, alias, engine, table), fOutputReady(-1), fWriterThread(this), fRunning(false)
{
    memset(fInputs, 0, sizeof(fInputs));
    memset(fOutputs, 0, sizeof(fOutputs));
}

JackHaikuMidiDriver::~JackHaikuMidiDriver()
{
}

int JackHaikuMidiDriver::Open(bool capturing,
                              bool playing,
                              int inchannels,
                              int outchannels,
                              bool monitor,
                              const char* capture_driver_name,
                              const char* playback_driver_name,
                              jack_nframes_t capture_latency,
                              jack_nframes_t playback_latency)
{
    int num_inputs = 0;
    int num_outputs = 0;
    int32 id;
    char name[REAL_JACK_PORT_NAME_SIZE + 1];

    // Every remote producer endpoint (a hardware MIDI input published by the
    // midi_server, e.g. usb_midi ports) becomes a JACK capture port.
    id = 0;
    BMidiProducer* producer;
    while ((producer = BMidiRoster::NextProducer(&id)) != NULL && num_inputs < HAIKU_MIDI_MAX_PORTS) {
        if (!producer->IsRemote() || !producer->IsValid()) {
            producer->Release();
            continue;
        }
        snprintf(name, sizeof(name), "jack_midi in: %s", producer->Name());
        JackHaikuMidiInput* input = new JackHaikuMidiInput(name, producer);
        if (!input->IsOk() || producer->Connect(input) != B_OK) {
            jack_error("JackHaikuMidiDriver: cannot connect to '%s'", producer->Name());
            input->Release(); // BMidiLocalConsumer is reference counted
            continue;
        }
        jack_info("JackHaikuMidiDriver: capture %d <- '%s'", num_inputs + 1, producer->Name());
        fInputs[num_inputs++] = input;
    }

    // Every remote consumer endpoint (a hardware MIDI output) becomes a JACK
    // playback port.
    id = 0;
    BMidiConsumer* consumer;
    while ((consumer = BMidiRoster::NextConsumer(&id)) != NULL && num_outputs < HAIKU_MIDI_MAX_PORTS) {
        if (!consumer->IsRemote() || !consumer->IsValid()) {
            consumer->Release();
            continue;
        }
        snprintf(name, sizeof(name), "jack_midi out: %s", consumer->Name());
        JackHaikuMidiOutput* output = new JackHaikuMidiOutput(name, consumer);
        if (!output->IsOk()) {
            jack_error("JackHaikuMidiDriver: cannot connect to '%s'", consumer->Name());
            delete output;
            continue;
        }
        jack_info("JackHaikuMidiDriver: playback %d -> '%s'", num_outputs + 1, consumer->Name());
        fOutputs[num_outputs++] = output;
    }

    if (num_inputs == 0 && num_outputs == 0) {
        jack_error("JackHaikuMidiDriver: no MIDI endpoints (is a device plugged in?)");
        // Not fatal: stay loaded with zero ports so a later restart can pick
        // devices up; hotplug support is a flagged TODO.
    }

    fOutputReady = create_sem(0, "jack haiku midi output");
    if (fOutputReady < 0) {
        jack_error("JackHaikuMidiDriver: create_sem failed");
        Close();
        return -1;
    }

    return JackMidiDriver::Open(capturing, playing, num_inputs, num_outputs, monitor,
                                capture_driver_name, playback_driver_name,
                                capture_latency, playback_latency);
}

int JackHaikuMidiDriver::Close()
{
    int res = JackMidiDriver::Close();

    for (int i = 0; i < HAIKU_MIDI_MAX_PORTS; i++) {
        if (fInputs[i] != NULL) {
            fInputs[i]->Release();
            fInputs[i] = NULL;
        }
        delete fOutputs[i];
        fOutputs[i] = NULL;
    }
    if (fOutputReady >= 0) {
        delete_sem(fOutputReady);
        fOutputReady = -1;
    }
    return res;
}

int JackHaikuMidiDriver::Start()
{
    if (JackMidiDriver::Start() < 0) {
        return -1;
    }
    fRunning = true;
    if (fPlaybackChannels > 0 && fWriterThread.StartSync() < 0) {
        jack_error("JackHaikuMidiDriver: cannot start writer thread");
        fRunning = false;
        return -1;
    }
    return 0;
}

int JackHaikuMidiDriver::Stop()
{
    fRunning = false;
    if (fOutputReady >= 0) {
        release_sem(fOutputReady); // wake the writer so it can exit
    }
    if (fPlaybackChannels > 0) {
        fWriterThread.Kill();
    }
    return JackMidiDriver::Stop();
}

bool JackHaikuMidiDriver::Execute()
{
    // Writer thread: sleep until Write() published events, then spray them
    // from a context where midi2's locks/allocations are harmless.
    while (fRunning) {
        if (acquire_sem(fOutputReady) != B_OK) {
            break;
        }
        for (int i = 0; i < fPlaybackChannels; i++) {
            if (fOutputs[i] != NULL) {
                fOutputs[i]->Drain();
            }
        }
    }
    return false;
}

int JackHaikuMidiDriver::Read()
{
    HaikuMidiRecordHeader header;

    for (int i = 0; i < fCaptureChannels; i++) {
        JackMidiBuffer* buffer = GetInputBuffer(i);
        if (buffer == NULL || fInputs[i] == NULL) {
            continue;
        }
        // The engine does not clear driver MIDI buffers between cycles;
        // without this every period re-delivers all previous events.
        buffer->Reset(fEngineControl->fBufferSize);
        jack_ringbuffer_t* ring = fInputs[i]->Ring();
        while (jack_ringbuffer_read_space(ring) >= sizeof(header)) {
            jack_ringbuffer_read(ring, (char*)&header, sizeof(header));
            if (header.length > jack_ringbuffer_read_space(ring)) {
                jack_ringbuffer_reset(ring);
                break;
            }
            // TODO (flagged): map header.time (system_time us) to a frame
            // offset inside this period instead of 0. Ordering is preserved;
            // intra-period timing is quantized to the period until then.
            jack_midi_data_t* dst = buffer->ReserveEvent(0, header.length);
            if (dst == NULL) {
                // JACK buffer full: consume and count, do not stall the ring.
                jack_ringbuffer_read_advance(ring, header.length);
                buffer->lost_events++;
                continue;
            }
            jack_ringbuffer_read(ring, (char*)dst, header.length);
        }
    }
    return 0;
}

int JackHaikuMidiDriver::Write()
{
    bool published = false;

    for (int i = 0; i < fPlaybackChannels; i++) {
        JackMidiBuffer* buffer = GetOutputBuffer(i);
        if (buffer == NULL || fOutputs[i] == NULL || !buffer->IsValid()) {
            continue;
        }
        jack_ringbuffer_t* ring = fOutputs[i]->Ring();
        for (unsigned int j = 0; j < buffer->event_count; j++) {
            JackMidiEvent* event = &buffer->events[j];
            HaikuMidiRecordHeader header;
            header.time = 0; // "now"; sub-period scheduling is a flagged TODO
            header.length = event->size;
            if (jack_ringbuffer_write_space(ring) < sizeof(header) + event->size) {
                break; // full: drop the rest of this cycle's events
            }
            jack_ringbuffer_write(ring, (const char*)&header, sizeof(header));
            jack_ringbuffer_write(ring, (const char*)event->GetData(buffer), event->size);
            published = true;
        }
    }

    if (published && fOutputReady >= 0) {
        // Non-blocking wake of the writer thread (B_DO_NOT_RESCHEDULE keeps
        // the RT thread from yielding here).
        release_sem_etc(fOutputReady, 1, B_DO_NOT_RESCHEDULE);
    }
    return 0;
}

} // namespace Jack

#ifdef __cplusplus
extern "C" {
#endif

SERVER_EXPORT jack_driver_desc_t* driver_get_descriptor()
{
    jack_driver_desc_t* desc;
    jack_driver_desc_filler_t filler;

    desc = jack_driver_descriptor_construct(
        "haikumidi", JackDriverSlave, "Haiku midi2 kit MIDI backend", &filler);

    return desc;
}

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    Jack::JackHaikuMidiDriver* driver = new Jack::JackHaikuMidiDriver("system_midi", "haikumidi", engine, table);
    if (driver->Open(true, true, 0, 0, false, "in", "out", 0, 0) == 0) {
        return driver;
    } else {
        delete driver;
        return NULL;
    }
}

#ifdef __cplusplus
}
#endif
