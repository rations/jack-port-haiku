/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2004-2008 Grame

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/

#include "JackPosixThread.h"
#include "JackError.h"
#include "JackTime.h"
#include "JackGlobals.h"
#include <signal.h>
#include <string.h> // for memset
#include <unistd.h> // for _POSIX_PRIORITY_SCHEDULING check

#if defined(__linux__) && !defined(SCHED_RESET_ON_FORK)
# define SCHED_RESET_ON_FORK 0x40000000
#endif

/* Real-time scheduling differs across POSIX systems: most provide SCHED_FIFO,
   while Haiku provides only SCHED_RR (real-time priority band 100..120).
   Select a policy the platform actually supports at runtime, and clamp the
   engine's requested priority into that policy's valid range, so the same
   engine priorities work everywhere. */
static int jack_rt_policy()
{
    return (sched_get_priority_min(SCHED_FIFO) != -1) ? SCHED_FIFO : SCHED_RR;
}

static int jack_clamp_rt_priority(int policy, int priority)
{
    int min = sched_get_priority_min(policy);
    int max = sched_get_priority_max(policy);
    if (min != -1 && priority < min) {
        priority = min;
    }
    if (max != -1 && priority > max) {
        priority = max;
    }
    return priority;
}

namespace Jack
{

void* JackPosixThread::ThreadHandler(void* arg)
{
    JackPosixThread* obj = (JackPosixThread*)arg;
    JackRunnableInterface* runnable = obj->fRunnable;
    int err;

    /* Block async signals: POSIX allows a process-directed signal to be
       delivered to any thread that does not block it. If it is delivered to
       an internal JACK thread and the application's handler then calls
       jack_client_close(), the close runs on the very thread it needs to
       cancel and join, deadlocking the process. Masking here forces delivery
       to an application thread. Cancellation is unaffected: implementations
       deliver pthread_cancel() through a signal that cannot be blocked. */
    sigset_t set;
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    if ((err = pthread_setcanceltype(obj->fCancellation, NULL)) != 0) {
        jack_error("pthread_setcanceltype err = %s", strerror(err));
    }

    // Signal creation thread when started with StartSync
    jack_log("JackPosixThread::ThreadHandler : start");
    obj->fStatus = kIniting;

    // Call Init method
    if (!runnable->Init()) {
        jack_error("Thread init fails: thread quits");
        return 0;
    }

    obj->fStatus = kRunning;

    // If Init succeed, start the thread loop
    bool res = true;
    while (obj->fStatus == kRunning && res) {
        res = runnable->Execute();
    }

    jack_log("JackPosixThread::ThreadHandler : exit");
    pthread_exit(0);
    return 0; // never reached
}

int JackPosixThread::Start()
{
    fStatus = kStarting;

    // Check if the thread was correctly started
    if (StartImp(&fThread, fPriority, fRealTime, ThreadHandler, this) < 0) {
        fStatus = kIdle;
        return -1;
    } else {
        return 0;
    }
}

int JackPosixThread::StartSync()
{
    fStatus = kStarting;

    if (StartImp(&fThread, fPriority, fRealTime, ThreadHandler, this) < 0) {
        fStatus = kIdle;
        return -1;
    } else {
        int count = 0;
        while (fStatus == kStarting && ++count < 1000) {
            JackSleep(1000);
        }
        return (count == 1000) ? -1 : 0;
    }
}

int JackPosixThread::StartImp(jack_native_thread_t* thread, int priority, int realtime, void*(*start_routine)(void*), void* arg)
{
    pthread_attr_t attributes;
    struct sched_param rt_param;
    pthread_attr_init(&attributes);
    int res;

    if ((res = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_JOINABLE))) {
        jack_error("Cannot request joinable thread creation for thread res = %d", res);
        return -1;
    }

    if ((res = pthread_attr_setscope(&attributes, PTHREAD_SCOPE_SYSTEM))) {
        jack_error("Cannot set scheduling scope for thread res = %d", res);
        return -1;
    }

    if (realtime) {

        jack_log("JackPosixThread::StartImp : create RT thread");

#ifdef HAVE_PTHREAD_ATTR_SETSCHEDPOLICY
        if ((res = pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED))) {
            jack_error("Cannot request explicit scheduling for RT thread res = %d", res);
            return -1;
        }

        if ((res = pthread_attr_setschedpolicy(&attributes, jack_rt_policy()))) {
            jack_error("Cannot set RR scheduling class for RT thread res = %d", res);
            return -1;
        }

        memset(&rt_param, 0, sizeof(rt_param));
        rt_param.sched_priority = jack_clamp_rt_priority(jack_rt_policy(), priority);

        if ((res = pthread_attr_setschedparam(&attributes, &rt_param))) {
            jack_error("Cannot set scheduling priority for RT thread res = %d", res);
            return -1;
        }
#endif

    } else {
        jack_log("JackPosixThread::StartImp : create non RT thread");
#ifdef HAVE_PTHREAD_ATTR_SETSCHEDPOLICY
        if ((res = pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED))) {
            jack_log("Cannot request explicit scheduling for non RT thread res = %d", res);
        }
#endif
    }

    if ((res = pthread_attr_setstacksize(&attributes, THREAD_STACK))) {
        jack_error("Cannot set thread stack size res = %d", res);
        return -1;
    }

    if ((res = JackGlobals::fJackThreadCreator(thread, &attributes, start_routine, arg))) {
        jack_error("Cannot create thread res = %d", res);
        return -1;
    }

#ifndef HAVE_PTHREAD_ATTR_SETSCHEDPOLICY
    // Platforms without attribute-level scheduling (e.g. Haiku) apply the
    // real-time policy and priority once the thread exists.
    if (realtime) {
        int policy = jack_rt_policy();
        memset(&rt_param, 0, sizeof(rt_param));
        rt_param.sched_priority = jack_clamp_rt_priority(policy, priority);
        if ((res = pthread_setschedparam(*thread, policy, &rt_param))) {
            jack_error("Cannot set RT scheduling priority for thread res = %d", res);
            return -1;
        }
    }
#endif

    pthread_attr_destroy(&attributes);
    return 0;
}

int JackPosixThread::Kill()
{
    if (fThread != (jack_native_thread_t)NULL) { // If thread has been started
        jack_log("JackPosixThread::Kill");
        if (pthread_equal(pthread_self(), fThread)) {
            /* Called from the thread itself (e.g. jack_client_close() on this
               thread): a self pthread_cancel() would kill the thread at the
               next cancellation point in the middle of the caller's cleanup.
               Skip it and let the caller finish; the thread exits when the
               caller returns. */
            jack_error("JackPosixThread::Kill from thread itself, skipped");
            fStatus = kIdle;
            fThread = (jack_native_thread_t)NULL;
            return 0;
        }
        void* status;
        pthread_cancel(fThread);
        pthread_join(fThread, &status);
        fStatus = kIdle;
        fThread = (jack_native_thread_t)NULL;
        return 0;
    } else {
        return -1;
    }
}

int JackPosixThread::Stop()
{
    if (fThread != (jack_native_thread_t)NULL) { // If thread has been started
        jack_log("JackPosixThread::Stop");
        void* status;
        fStatus = kIdle; // Request for the thread to stop
        pthread_join(fThread, &status);
        fThread = (jack_native_thread_t)NULL;
        return 0;
    } else {
        return -1;
    }
}

int JackPosixThread::KillImp(jack_native_thread_t thread)
{
    if (thread != (jack_native_thread_t)NULL) { // If thread has been started
        jack_log("JackPosixThread::Kill");
        void* status;
        pthread_cancel(thread);
        pthread_join(thread, &status);
        return 0;
    } else {
        return -1;
    }
}

int JackPosixThread::StopImp(jack_native_thread_t thread)
{
    if (thread != (jack_native_thread_t)NULL) { // If thread has been started
        jack_log("JackPosixThread::Stop");
        void* status;
        pthread_join(thread, &status);
        return 0;
    } else {
        return -1;
    }
}

int JackPosixThread::AcquireRealTime()
{
    return (fThread != (jack_native_thread_t)NULL) ? AcquireRealTimeImp(fThread, fPriority) : -1;
}

int JackPosixThread::AcquireSelfRealTime()
{
    return AcquireRealTimeImp(pthread_self(), fPriority);
}

int JackPosixThread::AcquireRealTime(int priority)
{
    fPriority = priority;
    return AcquireRealTime();
}

int JackPosixThread::AcquireSelfRealTime(int priority)
{
    fPriority = priority;
    return AcquireSelfRealTime();
}
int JackPosixThread::AcquireRealTimeImp(jack_native_thread_t thread, int priority)
{
    struct sched_param rtparam;
    int res;
    int policy = jack_rt_policy();
    memset(&rtparam, 0, sizeof(rtparam));
    rtparam.sched_priority = jack_clamp_rt_priority(policy, priority);

    jack_log("JackPosixThread::AcquireRealTimeImp priority = %d", priority);

    if ((res = pthread_setschedparam(thread, policy, &rtparam)) == 0)
        return 0;

#ifdef SCHED_RESET_ON_FORK
    jack_log("pthread_setschedparam() failed (%d), trying with SCHED_RESET_ON_FORK.", res);
    if ((res = pthread_setschedparam(thread, policy|SCHED_RESET_ON_FORK, &rtparam)) == 0)
        return 0;
#endif

    jack_error("Cannot use real-time scheduling (RR/%d)"
               " (%d: %s)", rtparam.sched_priority, res,
               strerror(res));
    return -1;
}

int JackPosixThread::DropRealTime()
{
    return (fThread != (jack_native_thread_t)NULL) ? DropRealTimeImp(fThread) : -1;
}

int JackPosixThread::DropSelfRealTime()
{
    return DropRealTimeImp(pthread_self());
}

int JackPosixThread::DropRealTimeImp(jack_native_thread_t thread)
{
    struct sched_param rtparam;
    int res;
    memset(&rtparam, 0, sizeof(rtparam));
    rtparam.sched_priority = 0;

    if ((res = pthread_setschedparam(thread, SCHED_OTHER, &rtparam)) != 0) {
        jack_error("Cannot switch to normal scheduling priority(%s)", strerror(errno));
        return -1;
    }
    return 0;
}

jack_native_thread_t JackPosixThread::GetThreadID()
{
    return fThread;
}

bool JackPosixThread::IsThread()
{
    return pthread_self() == fThread;
}

void JackPosixThread::Terminate()
{
    jack_log("JackPosixThread::Terminate");
    pthread_exit(0);
}

SERVER_EXPORT void ThreadExit()
{
    jack_log("ThreadExit");
    pthread_exit(0);
}

} // end of namespace

bool jack_get_thread_realtime_priority_range(int * min_ptr, int * max_ptr)
{
#if defined(_POSIX_PRIORITY_SCHEDULING) && !defined(__APPLE__)
    int min, max;

    min = sched_get_priority_min(jack_rt_policy());
    if (min == -1)
    {
        jack_error("sched_get_priority_min() failed.");
        return false;
    }

    max = sched_get_priority_max(jack_rt_policy());
    if (max == -1)
    {
        jack_error("sched_get_priority_max() failed.");
        return false;
    }

    *min_ptr = min;
    *max_ptr = max;

    return true;
#else
    return false;
#endif
}

bool jack_tls_allocate_key(jack_tls_key *key_ptr)
{
    int ret;

    ret = pthread_key_create(key_ptr, NULL);
    if (ret != 0)
    {
        jack_error("pthread_key_create() failed with error %d", ret);
        return false;
    }

    return true;
}

bool jack_tls_free_key(jack_tls_key key)
{
    int ret;

    ret = pthread_key_delete(key);
    if (ret != 0)
    {
        jack_error("pthread_key_delete() failed with error %d", ret);
        return false;
    }

    return true;
}

bool jack_tls_set(jack_tls_key key, void *data_ptr)
{
    int ret;

    ret = pthread_setspecific(key, (const void *)data_ptr);
    if (ret != 0)
    {
        jack_error("pthread_setspecific() failed with error %d", ret);
        return false;
    }

    return true;
}

void *jack_tls_get(jack_tls_key key)
{
    return pthread_getspecific(key);
}
