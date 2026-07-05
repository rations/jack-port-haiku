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

#ifndef __JackHaikuMidiDriver__
#define __JackHaikuMidiDriver__

#include "JackMidiDriver.h"
#include "JackPosixThread.h"
#include "ringbuffer.h"

#include <MidiConsumer.h>
#include <MidiProducer.h>
#include <OS.h>

namespace Jack
{

#define HAIKU_MIDI_MAX_PORTS 32

class JackHaikuMidiDriver;

/*!
\brief One JACK capture port: receives from a midi2-kit hardware producer.

The midi2 kit delivers events on its own per-consumer thread via Data();
that thread only writes into the lock-free ring buffer, which the JACK
process cycle (Read) drains. Record layout in the ring:
[bigtime_t time][uint32 length][length bytes].
*/
class JackHaikuMidiInput : public BMidiLocalConsumer
{
private:
    jack_ringbuffer_t* fRing;
    BMidiProducer* fSource;

public:
    JackHaikuMidiInput(const char* name, BMidiProducer* source);
    virtual ~JackHaikuMidiInput();

    bool IsOk() const
    {
        return fRing != NULL;
    }
    jack_ringbuffer_t* Ring()
    {
        return fRing;
    }
    BMidiProducer* Source()
    {
        return fSource;
    }

    virtual void Data(uchar* data, size_t length, bool atomic, bigtime_t time);
};

/*!
\brief One JACK playback port: sends to a midi2-kit hardware consumer.

SprayData() locks, allocates and calls write_port, so it must never run on
the JACK process thread: Write() only copies events into the ring buffer,
and the driver's writer thread drains every ring and sprays.
*/
class JackHaikuMidiOutput
{
private:
    jack_ringbuffer_t* fRing;
    BMidiLocalProducer* fProducer;
    BMidiConsumer* fDestination;

public:
    JackHaikuMidiOutput(const char* name, BMidiConsumer* destination);
    ~JackHaikuMidiOutput();

    bool IsOk() const
    {
        return fRing != NULL && fConnected;
    }
    jack_ringbuffer_t* Ring()
    {
        return fRing;
    }
    void Drain();

private:
    bool fConnected;
};

/*!
\brief JACK MIDI slave driver bridging Haiku's midi2 kit (midi_server
endpoints, including usb_midi hardware ports) into the JACK graph.

Loaded with "jackd -d <master> -X haikumidi". Hardware MIDI inputs
(BMidiProducer endpoints) become system_midi:capture_N ports; hardware
outputs (BMidiConsumer endpoints) become system_midi:playback_N.
*/
class JackHaikuMidiDriver : public JackMidiDriver, public JackRunnableInterface
{
private:
    JackHaikuMidiInput* fInputs[HAIKU_MIDI_MAX_PORTS];
    JackHaikuMidiOutput* fOutputs[HAIKU_MIDI_MAX_PORTS];
    sem_id fOutputReady;
    JackPosixThread fWriterThread;
    volatile bool fRunning;

public:
    JackHaikuMidiDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table);
    virtual ~JackHaikuMidiDriver();

    int Open(bool capturing,
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

    // Writer thread body (JackRunnableInterface).
    bool Execute();
};

} // namespace Jack

#endif
