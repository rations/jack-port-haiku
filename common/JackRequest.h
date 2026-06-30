/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2004-2008 Grame
Copyright (C) 2016-2026 Filipe Coelho

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

#ifndef __JackRequest__
#define __JackRequest__

#include "JackConstants.h"
#include "JackError.h"
#include "JackPlatformPlug.h"
#include "JackChannel.h"
#include "JackTime.h"
#include "types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <list>

namespace Jack
{

#define JACK_CLIENT_NAME_SIZE_1 (JACK_CLIENT_NAME_SIZE + 1)
#define JACK_LOAD_INIT_LIMIT_1 (JACK_LOAD_INIT_LIMIT + 1)
#define JACK_MESSAGE_SIZE_1 (JACK_MESSAGE_SIZE + 1)
#define JACK_PORT_NAME_SIZE_1 (JACK_PORT_NAME_SIZE + 1)
#define JACK_PORT_TYPE_SIZE_1 (JACK_PORT_TYPE_SIZE + 1)
#define REAL_JACK_PORT_NAME_SIZE_1 (REAL_JACK_PORT_NAME_SIZE + 1)

#define CheckRes(exp) { \
    int reserr = (exp); \
    if (reserr < 0) { \
        if (reserr != JACK_REQUEST_ERR_ABORTED) \
            jack_error("CheckRes error for " #exp "in %s", __PRETTY_FUNCTION__); \
        return reserr; \
    } \
}

#define CheckSize() { \
    int size = -1; \
    CheckRes(trans->Read(&size, sizeof(int))); \
    if (size != Size()) { \
        jack_error("CheckSize error size = %d Size() = %d", size, Size()); \
        return -1; \
    } \
}

/*!
\brief Session API constants.
*/

enum JackSessionReply {

    kImmediateSessionReply = 1,
    kPendingSessionReply = 2

};

/*!
\brief Request from client to server.
*/

struct JackRequest
{

    enum RequestType {
        kRegisterPort = 1,
        kUnRegisterPort = 2,
        kConnectPorts = 3,
        kDisconnectPorts = 4,
        kSetTimeBaseClient = 5,
        kActivateClient = 6,
        kDeactivateClient = 7,
        kDisconnectPort = 8,
        kSetClientCapabilities = 9,
        kGetPortConnections = 10,
        kGetPortNConnections = 11,
        kReleaseTimebase = 12,
        kSetTimebaseCallback = 13,
        kSetBufferSize = 20,
        kSetFreeWheel = 21,
        kClientCheck = 22,
        kClientOpen = 23,
        kClientClose = 24,
        kConnectNamePorts = 25,
        kDisconnectNamePorts = 26,
        kGetInternalClientName = 27,
        kInternalClientHandle = 28,
        kInternalClientLoad = 29,
        kInternalClientUnload = 30,
        kPortRename = 31,
        kNotification = 32,
        kSessionNotify = 33,
        kSessionReply  = 34,
        kGetClientByUUID = 35,
        kReserveClientName = 36,
        kGetUUIDByClient = 37,
        kClientHasSessionCallback = 38,
        kComputeTotalLatencies = 39,
        kPropertyChangeNotify = 40
    };

    RequestType fType;

    JackRequest(RequestType type)
        : fType(type)
    {}

    virtual ~JackRequest()
    {}

    virtual int Read(detail::JackChannelTransactionInterface* trans) = 0;

    virtual int Write(detail::JackChannelTransactionInterface* trans) = 0;
};

struct JackRequestHeader : JackRequest
{
    JackRequestHeader()
        : JackRequest((RequestType)0)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        return trans->Read(&fType, sizeof(RequestType));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        return -1;
    }
};

template<int DataSize>
struct JackRequestImpl : JackRequest
{

    static constexpr int TotalSize = sizeof(RequestType) + sizeof(int) + DataSize;

    JackRequestImpl(RequestType type)
        : JackRequest(type), fPacketPos(0)
    {
    }

    int WritePacketInit()
    {
        if (fType == 0) {
            jack_error("JackRequestImpl::WritePacketInit error unspecified type");
            return -1;
        }
        if (fPacketPos != 0) {
            jack_error("JackRequestImpl::WritePacketInit error called more than once for type %d", fType);
            return -1;
        }
        const int size = DataSize;
        memcpy(fPacketData, &fType, sizeof(RequestType));
        memcpy(fPacketData + sizeof(RequestType), &size, sizeof(int));
        fPacketPos = sizeof(RequestType) + sizeof(int);
        return 0;
    }

    int WritePacket(void* data, int len)
    {
        if (fPacketPos == 0) {
            jack_error("JackRequestImpl::WritePacket failed for type %d because it has not been initialized", fType);
            return -1;
        }
        if (fPacketPos == -1) {
            jack_error("JackRequestImpl::WritePacket failed for type %d because it has already been sent", fType);
            return -1;
        }
        if (fPacketPos + len > TotalSize) {
            jack_error("JackRequestImpl::WritePacket failed for type %d because of too much data, pos %d, len %d, total size %d",
                       fType, fPacketPos, len, TotalSize);
            return -1;
        }

        jack_log("JackRequestImpl::WritePacket ok for type %d, pos %d, len %d, total size %d (new pos %d)",
                 fType, fPacketPos, len, TotalSize, fPacketPos + len);
        memcpy(fPacketData + fPacketPos, data, len);
        fPacketPos += len;
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        if (fPacketPos != TotalSize) {
            jack_error("JackRequestImpl::Write failed for type %d because of incomplete data, pos %d, total size %d",
                       fType, fPacketPos, TotalSize);
            return -1;
        }

        // invalidate any more writes
        fPacketPos = -1;

        const int err = trans->Write(fPacketData, TotalSize);
        if (err < 0 && err != JACK_REQUEST_ERR_ABORTED)
            jack_error("JackRequestImpl::Write failed for type %d", fType);

        return err;
    }

    static constexpr int Size() { return DataSize; }

private:
    uint8_t fPacketData[TotalSize];
    int fPacketPos;

};

/*!
\brief Result from the server.
*/

struct JackResult
{

    int fResult;

    JackResult(): fResult( -1)
    {}
    JackResult(int result): fResult(result)
    {}
    virtual ~JackResult()
    {}

    virtual int Read(detail::JackChannelTransactionInterface* trans)
    {
        return trans->Read(&fResult, sizeof(int));
    }

    virtual int Write(detail::JackChannelTransactionInterface* trans)
    {
        return trans->Write(&fResult, sizeof(int));
    }

};

/*!
\brief CheckClient request.
*/

struct JackClientCheckRequest : public JackRequestImpl<JACK_CLIENT_NAME_SIZE_1 + 3 * sizeof(int) + sizeof(jack_uuid_t)>
{

    char fName[JACK_CLIENT_NAME_SIZE_1];
    int fProtocol;
    int fOptions;
    int fOpen;
    jack_uuid_t fUUID;

    JackClientCheckRequest()
        : JackRequestImpl(kClientCheck), fProtocol(0), fOptions(0), fOpen(0), fUUID(JACK_UUID_EMPTY_INITIALIZER)
    {
        memset(fName, 0, sizeof(fName));
    }

    JackClientCheckRequest(const char* name, int protocol, int options, jack_uuid_t uuid, int open = false)
        : JackRequestImpl(kClientCheck), fProtocol(protocol), fOptions(options), fOpen(open), fUUID(uuid)
    {
        memset(fName, 0, sizeof(fName));
        snprintf(fName, sizeof(fName), "%s", name);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fName, sizeof(fName)));
        CheckRes(trans->Read(&fProtocol, sizeof(int)));
        CheckRes(trans->Read(&fOptions, sizeof(int)));
        CheckRes(trans->Read(&fUUID, sizeof(jack_uuid_t)));
        return trans->Read(&fOpen, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fName, sizeof(fName)));
        CheckRes(WritePacket(&fProtocol, sizeof(int)));
        CheckRes(WritePacket(&fOptions, sizeof(int)));
        CheckRes(WritePacket(&fUUID, sizeof(jack_uuid_t)));
        CheckRes(WritePacket(&fOpen, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

    static constexpr int Size() { return sizeof(fName) + 3 * sizeof(int) + sizeof(jack_uuid_t); }

};

/*!
\brief CheckClient result.
*/

struct JackClientCheckResult : public JackResult
{

    char fName[JACK_CLIENT_NAME_SIZE+1];
    int fStatus;

    JackClientCheckResult(): JackResult(), fStatus(0)
    {
        memset(fName, 0, sizeof(fName));
    }
    JackClientCheckResult(int32_t result, const char* name, int status)
            : JackResult(result), fStatus(status)
    {
        memset(fName, 0, sizeof(fName));
        snprintf(fName, sizeof(fName), "%s", name);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fName, sizeof(fName)));
        CheckRes(trans->Read(&fStatus, sizeof(int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fName, sizeof(fName)));
        CheckRes(trans->Write(&fStatus, sizeof(int)));
        return 0;
    }

};

/*!
\brief NewClient request.
*/

struct JackClientOpenRequest : public JackRequestImpl<sizeof(int) + sizeof(jack_uuid_t) + JACK_CLIENT_NAME_SIZE_1>
{

    int fPID;
    jack_uuid_t fUUID;
    char fName[JACK_CLIENT_NAME_SIZE_1];

    JackClientOpenRequest()
        : JackRequestImpl(kClientOpen), fPID(0), fUUID(JACK_UUID_EMPTY_INITIALIZER)
    {
        memset(fName, 0, sizeof(fName));
    }

    JackClientOpenRequest(const char* name, int pid, jack_uuid_t uuid)
        : JackRequestImpl(kClientOpen)
    {
        memset(fName, 0, sizeof(fName));
        snprintf(fName, sizeof(fName), "%s", name);
        fPID = pid;
        fUUID = uuid;
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fPID, sizeof(int)));
        CheckRes(trans->Read(&fUUID, sizeof(jack_uuid_t)));
        return trans->Read(&fName, sizeof(fName));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fPID, sizeof(int)));
        CheckRes(WritePacket(&fUUID, sizeof(jack_uuid_t)));
        CheckRes(WritePacket(&fName, sizeof(fName)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief NewClient result.
*/

struct JackClientOpenResult : public JackResult
{

    int fSharedEngine;
    int fSharedClient;
    int fSharedGraph;

    JackClientOpenResult()
            : JackResult(), fSharedEngine(-1), fSharedClient(-1), fSharedGraph(-1)
    {}
    JackClientOpenResult(int32_t result, int index1, int index2, int index3)
            : JackResult(result), fSharedEngine(index1), fSharedClient(index2), fSharedGraph(index3)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fSharedEngine, sizeof(int)));
        CheckRes(trans->Read(&fSharedClient, sizeof(int)));
        CheckRes(trans->Read(&fSharedGraph, sizeof(int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fSharedEngine, sizeof(int)));
        CheckRes(trans->Write(&fSharedClient, sizeof(int)));
        CheckRes(trans->Write(&fSharedGraph, sizeof(int)));
        return 0;
    }

};

/*!
\brief CloseClient request.
*/

struct JackClientCloseRequest : public JackRequestImpl<sizeof(int)>
{

    int fRefNum;

    JackClientCloseRequest()
        : JackRequestImpl(kClientClose), fRefNum(0)
    {}

    JackClientCloseRequest(int refnum)
        : JackRequestImpl(kClientClose), fRefNum(refnum)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        return trans->Read(&fRefNum, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief Activate request.
*/

struct JackActivateRequest : public JackRequestImpl<2 * sizeof(int)>
{

    int fRefNum;
    int fIsRealTime;

    JackActivateRequest()
        : JackRequestImpl(kActivateClient), fRefNum(0), fIsRealTime(0)
    {}

    JackActivateRequest(int refnum, int is_real_time)
        : JackRequestImpl(kActivateClient), fRefNum(refnum), fIsRealTime(is_real_time)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        return trans->Read(&fIsRealTime, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fIsRealTime, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief Deactivate request.
*/

struct JackDeactivateRequest : public JackRequestImpl<sizeof(int)>
{

    int fRefNum;

    JackDeactivateRequest()
        : JackRequestImpl(kDeactivateClient), fRefNum(0)
    {}

    JackDeactivateRequest(int refnum)
        : JackRequestImpl(kDeactivateClient), fRefNum(refnum)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        return trans->Read(&fRefNum, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief PortRegister request.
*/

struct JackPortRegisterRequest : public JackRequestImpl<sizeof(int) + JACK_PORT_NAME_SIZE_1 + JACK_PORT_TYPE_SIZE_1 + 2 * sizeof(unsigned int)>
{

    int fRefNum;
    char fName[JACK_PORT_NAME_SIZE_1];   // port short name
    char fPortType[JACK_PORT_TYPE_SIZE_1];
    unsigned int fFlags;
    unsigned int fBufferSize;

    JackPortRegisterRequest()
        : JackRequestImpl(kRegisterPort), fRefNum(0), fFlags(0), fBufferSize(0)
    {
        memset(fName, 0, sizeof(fName));
        memset(fPortType, 0, sizeof(fPortType));
    }

    JackPortRegisterRequest(int refnum, const char* name, const char* port_type, unsigned int flags, unsigned int buffer_size)
        : JackRequestImpl(kRegisterPort), fRefNum(refnum), fFlags(flags), fBufferSize(buffer_size)
    {
        memset(fName, 0, sizeof(fName));
        memset(fPortType, 0, sizeof(fPortType));
        strncpy(fName, name, sizeof(fName)-1);
        strncpy(fPortType, port_type, sizeof(fPortType)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fName, sizeof(fName)));
        CheckRes(trans->Read(&fPortType, sizeof(fPortType)));
        CheckRes(trans->Read(&fFlags, sizeof(unsigned int)));
        CheckRes(trans->Read(&fBufferSize, sizeof(unsigned int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fName, sizeof(fName)));
        CheckRes(WritePacket(&fPortType, sizeof(fPortType)));
        CheckRes(WritePacket(&fFlags, sizeof(unsigned int)));
        CheckRes(WritePacket(&fBufferSize, sizeof(unsigned int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief PortRegister result.
*/

struct JackPortRegisterResult : public JackResult
{

    jack_port_id_t fPortIndex;

    JackPortRegisterResult(): JackResult(), fPortIndex(NO_PORT)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        return trans->Read(&fPortIndex, sizeof(jack_port_id_t));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        return trans->Write(&fPortIndex, sizeof(jack_port_id_t));
    }

};

/*!
\brief PortUnregister request.
*/

struct JackPortUnRegisterRequest : public JackRequestImpl<sizeof(int) + sizeof(jack_port_id_t)>
{

    int fRefNum;
    jack_port_id_t fPortIndex;

    JackPortUnRegisterRequest()
        : JackRequestImpl(kUnRegisterPort), fRefNum(0), fPortIndex(0)
    {}

    JackPortUnRegisterRequest(int refnum, jack_port_id_t index)
        : JackRequestImpl(kUnRegisterPort), fRefNum(refnum), fPortIndex(index)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fPortIndex, sizeof(jack_port_id_t)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fPortIndex, sizeof(jack_port_id_t)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief PortConnectName request.
*/

struct JackPortConnectNameRequest : public JackRequestImpl<sizeof(int) + 2 * REAL_JACK_PORT_NAME_SIZE_1>
{

    int fRefNum;
    char fSrc[REAL_JACK_PORT_NAME_SIZE_1];    // port full name
    char fDst[REAL_JACK_PORT_NAME_SIZE_1];    // port full name

    JackPortConnectNameRequest()
        : JackRequestImpl(kConnectNamePorts), fRefNum(0)
    {
        memset(fSrc, 0, sizeof(fSrc));
        memset(fDst, 0, sizeof(fDst));
    }

    JackPortConnectNameRequest(int refnum, const char* src_name, const char* dst_name)
        : JackRequestImpl(kConnectNamePorts), fRefNum(refnum)
    {
        memset(fSrc, 0, sizeof(fSrc));
        memset(fDst, 0, sizeof(fDst));
        strncpy(fSrc, src_name, sizeof(fSrc)-1);
        strncpy(fDst, dst_name, sizeof(fDst)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fSrc, sizeof(fSrc)));
        CheckRes(trans->Read(&fDst, sizeof(fDst)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fSrc, sizeof(fSrc)));
        CheckRes(WritePacket(&fDst, sizeof(fDst)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief PortDisconnectName request.
*/

struct JackPortDisconnectNameRequest : public JackRequestImpl<sizeof(int) + 2 * REAL_JACK_PORT_NAME_SIZE_1>
{

    int fRefNum;
    char fSrc[REAL_JACK_PORT_NAME_SIZE_1];    // port full name
    char fDst[REAL_JACK_PORT_NAME_SIZE_1];    // port full name

    JackPortDisconnectNameRequest()
        : JackRequestImpl(kDisconnectNamePorts), fRefNum(0)
    {
        memset(fSrc, 0, sizeof(fSrc));
        memset(fDst, 0, sizeof(fDst));
    }

    JackPortDisconnectNameRequest(int refnum, const char* src_name, const char* dst_name)
        : JackRequestImpl(kDisconnectNamePorts), fRefNum(refnum)
    {
        memset(fSrc, 0, sizeof(fSrc));
        memset(fDst, 0, sizeof(fDst));
        strncpy(fSrc, src_name, sizeof(fSrc)-1);
        strncpy(fDst, dst_name, sizeof(fDst)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fSrc, sizeof(fSrc)));
        CheckRes(trans->Read(&fDst, sizeof(fDst)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fSrc, sizeof(fSrc)));
        CheckRes(WritePacket(&fDst, sizeof(fDst)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief PortConnect request.
*/

struct JackPortConnectRequest : public JackRequestImpl<sizeof(int) + 2 * sizeof(jack_port_id_t)>
{

    int fRefNum;
    jack_port_id_t fSrc;
    jack_port_id_t fDst;

    JackPortConnectRequest()
        : JackRequestImpl(kConnectPorts), fRefNum(0), fSrc(0), fDst(0)
    {}

    JackPortConnectRequest(int refnum, jack_port_id_t src, jack_port_id_t dst)
        : JackRequestImpl(kConnectPorts), fRefNum(refnum), fSrc(src), fDst(dst)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fSrc, sizeof(jack_port_id_t)));
        CheckRes(trans->Read(&fDst, sizeof(jack_port_id_t)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fSrc, sizeof(jack_port_id_t)));
        CheckRes(WritePacket(&fDst, sizeof(jack_port_id_t)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief PortDisconnect request.
*/

struct JackPortDisconnectRequest : public JackRequestImpl<sizeof(int) + 2 * sizeof(jack_port_id_t)>
{

    int fRefNum;
    jack_port_id_t fSrc;
    jack_port_id_t fDst;

    JackPortDisconnectRequest()
        : JackRequestImpl(kDisconnectPorts), fRefNum(0), fSrc(0), fDst(0)
    {}

    JackPortDisconnectRequest(int refnum, jack_port_id_t src, jack_port_id_t dst)
        : JackRequestImpl(kDisconnectPorts), fRefNum(refnum), fSrc(src), fDst(dst)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fSrc, sizeof(jack_port_id_t)));
        CheckRes(trans->Read(&fDst, sizeof(jack_port_id_t)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fSrc, sizeof(jack_port_id_t)));
        CheckRes(WritePacket(&fDst, sizeof(jack_port_id_t)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief PortRename request.
*/

struct JackPortRenameRequest : public JackRequestImpl<sizeof(int) + sizeof(jack_port_id_t) + JACK_PORT_NAME_SIZE_1>
{

    int fRefNum;
    jack_port_id_t fPort;
    char fName[JACK_PORT_NAME_SIZE_1];   // port short name

    JackPortRenameRequest()
        : JackRequestImpl(kPortRename), fRefNum(0), fPort(0)
    {
        memset(fName, 0, sizeof(fName));
    }

    JackPortRenameRequest(int refnum, jack_port_id_t port, const char* name)
        : JackRequestImpl(kPortRename), fRefNum(refnum), fPort(port)
    {
        memset(fName, 0, sizeof(fName));
        strncpy(fName, name, sizeof(fName)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fPort, sizeof(jack_port_id_t)));
        CheckRes(trans->Read(&fName, sizeof(fName)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fPort, sizeof(jack_port_id_t)));
        CheckRes(WritePacket(&fName, sizeof(fName)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief SetBufferSize request.
*/

struct JackSetBufferSizeRequest : public JackRequestImpl<sizeof(jack_nframes_t)>
{

    jack_nframes_t fBufferSize;

    JackSetBufferSizeRequest()
        : JackRequestImpl(kSetBufferSize), fBufferSize(0)
    {}

    JackSetBufferSizeRequest(jack_nframes_t buffer_size)
        : JackRequestImpl(kSetBufferSize), fBufferSize(buffer_size)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        return trans->Read(&fBufferSize, sizeof(jack_nframes_t));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fBufferSize, sizeof(jack_nframes_t)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief SetFreeWheel request.
*/

struct JackSetFreeWheelRequest : public JackRequestImpl<sizeof(int)>
{

    int fOnOff;

    JackSetFreeWheelRequest()
        : JackRequestImpl(kSetFreeWheel), fOnOff(0)
    {}

    JackSetFreeWheelRequest(int onoff)
        : JackRequestImpl(kSetFreeWheel), fOnOff(onoff)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        return trans->Read(&fOnOff, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fOnOff, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief ComputeTotalLatencies request.
*/

struct JackComputeTotalLatenciesRequest : public JackRequestImpl<0>
{

    JackComputeTotalLatenciesRequest()
        : JackRequestImpl(kComputeTotalLatencies)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief ReleaseTimebase request.
*/

struct JackReleaseTimebaseRequest : public JackRequestImpl<sizeof(int)>
{

    int fRefNum;

    JackReleaseTimebaseRequest()
        : JackRequestImpl(kReleaseTimebase), fRefNum(0)
    {}

    JackReleaseTimebaseRequest(int refnum)
        : JackRequestImpl(kReleaseTimebase), fRefNum(refnum)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        return trans->Read(&fRefNum, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief SetTimebaseCallback request.
*/

struct JackSetTimebaseCallbackRequest : public JackRequestImpl<sizeof(int) + sizeof(int)>
{

    int fRefNum;
    int fConditionnal;

    JackSetTimebaseCallbackRequest()
        : JackRequestImpl(kSetTimebaseCallback), fRefNum(0), fConditionnal(0)
    {}

    JackSetTimebaseCallbackRequest(int refnum, int conditional)
        : JackRequestImpl(kSetTimebaseCallback), fRefNum(refnum), fConditionnal(conditional)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        return trans->Read(&fConditionnal, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fConditionnal, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief GetInternalClientName request.
*/

struct JackGetInternalClientNameRequest : public JackRequestImpl<sizeof(int) + sizeof(int)>
{

    int fRefNum;
    int fIntRefNum;

    JackGetInternalClientNameRequest()
        : JackRequestImpl(kGetInternalClientName), fRefNum(0), fIntRefNum(0)
    {}

    JackGetInternalClientNameRequest(int refnum, int int_ref)
        : JackRequestImpl(kGetInternalClientName), fRefNum(refnum), fIntRefNum(int_ref)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        return trans->Read(&fIntRefNum, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fIntRefNum, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief GetInternalClient result.
*/

struct JackGetInternalClientNameResult : public JackResult
{

    char fName[JACK_CLIENT_NAME_SIZE+1];

    JackGetInternalClientNameResult(): JackResult()
    {
        memset(fName, 0, sizeof(fName));
    }
    JackGetInternalClientNameResult(int32_t result, const char* name)
            : JackResult(result)
    {
        memset(fName, 0, sizeof(fName));
        snprintf(fName, sizeof(fName), "%s", name);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fName, sizeof(fName)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fName, sizeof(fName)));
        return 0;
    }

    int Size() const { return sizeof(fName); }
};

/*!
\brief InternalClientHandle request.
*/

struct JackInternalClientHandleRequest : public JackRequestImpl<sizeof(int) + JACK_CLIENT_NAME_SIZE_1>
{

    int fRefNum;
    char fName[JACK_CLIENT_NAME_SIZE_1];

    JackInternalClientHandleRequest()
        : JackRequestImpl(kInternalClientHandle), fRefNum(0)
    {
        memset(fName, 0, sizeof(fName));
    }

    JackInternalClientHandleRequest(int refnum, const char* client_name)
        : JackRequestImpl(kInternalClientHandle), fRefNum(refnum)
    {
        memset(fName, 0, sizeof(fName));
        snprintf(fName, sizeof(fName), "%s", client_name);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        return trans->Read(&fName, sizeof(fName));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fName, sizeof(fName)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief InternalClientHandle result.
*/

struct JackInternalClientHandleResult : public JackResult
{

    int fStatus;
    int fIntRefNum;

    JackInternalClientHandleResult(): JackResult(), fStatus(0), fIntRefNum(0)
    {}
    JackInternalClientHandleResult(int32_t result, int status, int int_ref)
            : JackResult(result), fStatus(status), fIntRefNum(int_ref)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fStatus, sizeof(int)));
        CheckRes(trans->Read(&fIntRefNum, sizeof(int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fStatus, sizeof(int)));
        CheckRes(trans->Write(&fIntRefNum, sizeof(int)));
        return 0;
    }

    int Size() const { return sizeof(int) + sizeof(int); }
};

/*!
\brief InternalClientLoad request.
*/

#ifndef MAX_PATH
#define MAX_PATH 256
#endif
#define MAX_PATH_1 (MAX_PATH + 1)

struct JackInternalClientLoadRequest : public JackRequestImpl<sizeof(int) + JACK_CLIENT_NAME_SIZE_1 + MAX_PATH_1 + JACK_LOAD_INIT_LIMIT_1 + sizeof(int) + sizeof(jack_uuid_t)>
{

    int fRefNum;
    char fName[JACK_CLIENT_NAME_SIZE_1];
    char fDllName[MAX_PATH_1];
    char fLoadInitName[JACK_LOAD_INIT_LIMIT_1];
    int fOptions;
    jack_uuid_t fUUID;

    JackInternalClientLoadRequest()
        : JackRequestImpl(kInternalClientLoad), fRefNum(0), fOptions(0), fUUID(JACK_UUID_EMPTY_INITIALIZER)
    {
        memset(fName, 0, sizeof(fName));
        memset(fDllName, 0, sizeof(fDllName));
        memset(fLoadInitName, 0, sizeof(fLoadInitName));
    }

    JackInternalClientLoadRequest(int refnum, const char* client_name, const char* so_name, const char* objet_data, int options, jack_uuid_t uuid )
        : JackRequestImpl(kInternalClientLoad), fRefNum(refnum), fOptions(options), fUUID(uuid)
    {
        memset(fName, 0, sizeof(fName));
        memset(fDllName, 0, sizeof(fDllName));
        memset(fLoadInitName, 0, sizeof(fLoadInitName));
        strncpy(fName, client_name, sizeof(fName)-1);
        if (so_name) {
            strncpy(fDllName, so_name, sizeof(fDllName)-1);
        }
        if (objet_data) {
            strncpy(fLoadInitName, objet_data, sizeof(fLoadInitName)-1);
        }
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fName, sizeof(fName)));
        CheckRes(trans->Read(&fDllName, sizeof(fDllName)));
        CheckRes(trans->Read(&fLoadInitName, sizeof(fLoadInitName)));
        CheckRes(trans->Read(&fUUID, sizeof(jack_uuid_t)));
        return trans->Read(&fOptions, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fName, sizeof(fName)));
        CheckRes(WritePacket(&fDllName, sizeof(fDllName)));
        CheckRes(WritePacket(&fLoadInitName, sizeof(fLoadInitName)));
        CheckRes(WritePacket(&fUUID, sizeof(jack_uuid_t)));
        CheckRes(WritePacket(&fOptions, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief InternalClientLoad result.
*/

struct JackInternalClientLoadResult : public JackResult
{

    int fStatus;
    int fIntRefNum;

    JackInternalClientLoadResult(): JackResult(), fStatus(0), fIntRefNum(0)
    {}
    JackInternalClientLoadResult(int32_t result, int status, int int_ref)
            : JackResult(result), fStatus(status), fIntRefNum(int_ref)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fStatus, sizeof(int)));
        CheckRes(trans->Read(&fIntRefNum, sizeof(int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fStatus, sizeof(int)));
        CheckRes(trans->Write(&fIntRefNum, sizeof(int)));
        return 0;
    }

    int Size() const { return sizeof(int) + sizeof(int); }
};

/*!
\brief InternalClientUnload request.
*/

struct JackInternalClientUnloadRequest : public JackRequestImpl<sizeof(int) + sizeof(int)>
{

    int fRefNum;
    int fIntRefNum;

    JackInternalClientUnloadRequest()
        : JackRequestImpl(kInternalClientUnload), fRefNum(0), fIntRefNum(0)
    {}

    JackInternalClientUnloadRequest(int refnum, int int_ref)
        : JackRequestImpl(kInternalClientUnload), fRefNum(refnum), fIntRefNum(int_ref)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        return trans->Read(&fIntRefNum, sizeof(int));
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fIntRefNum, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief InternalClientLoad result.
*/

struct JackInternalClientUnloadResult : public JackResult
{

    int fStatus;

    JackInternalClientUnloadResult(): JackResult(), fStatus(0)
    {}
    JackInternalClientUnloadResult(int32_t result, int status)
            : JackResult(result), fStatus(status)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fStatus, sizeof(int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fStatus, sizeof(int)));
        return 0;
    }

    int Size() const { return sizeof(int); }
};

/*!
\brief ClientNotification request.
*/

struct JackClientNotificationRequest : public JackRequestImpl<3 * sizeof(int)>
{

    int fRefNum;
    int fNotify;
    int fValue;

    JackClientNotificationRequest()
        : JackRequestImpl(kNotification), fRefNum(0), fNotify(0), fValue(0)
    {}

    JackClientNotificationRequest(int refnum, int notify, int value)
        : JackRequestImpl(kNotification), fRefNum(refnum), fNotify(notify), fValue(value)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fNotify, sizeof(int)));
        CheckRes(trans->Read(&fValue, sizeof(int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        CheckRes(WritePacket(&fNotify, sizeof(int)));
        CheckRes(WritePacket(&fValue, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

struct JackSessionCommand
{
    char fUUID[JACK_UUID_STRING_SIZE];
    char fClientName[JACK_CLIENT_NAME_SIZE+1];
    char fCommand[JACK_SESSION_COMMAND_SIZE+1];
    jack_session_flags_t fFlags;

    JackSessionCommand() : fFlags(JackSessionSaveError)
    {
        memset(fUUID, 0, sizeof(fUUID));
        memset(fClientName, 0, sizeof(fClientName));
        memset(fCommand, 0, sizeof(fCommand));
    }
    JackSessionCommand(const char *uuid, const char *clientname, const char *command, jack_session_flags_t flags)
    {
        memset(fUUID, 0, sizeof(fUUID));
        memset(fClientName, 0, sizeof(fClientName));
        memset(fCommand, 0, sizeof(fCommand));
        strncpy(fUUID, uuid, sizeof(fUUID)-1);
        strncpy(fClientName, clientname, sizeof(fClientName)-1);
        strncpy(fCommand, command, sizeof(fCommand)-1);
        fFlags = flags;
    }
};

struct JackSessionNotifyResult : public JackResult
{

    std::list<JackSessionCommand> fCommandList;
    bool fDone;

    JackSessionNotifyResult(): JackResult(), fDone(false)
    {}
    JackSessionNotifyResult(int32_t result)
            : JackResult(result), fDone(false)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        if (trans == NULL)
        {
            return 0;
        }

        CheckRes(JackResult::Read(trans));
        while (true) {
            JackSessionCommand buffer;

            CheckRes(trans->Read(buffer.fUUID, sizeof(buffer.fUUID)));
            if (buffer.fUUID[0] == '\0')
                break;

            CheckRes(trans->Read(buffer.fClientName, sizeof(buffer.fClientName)));
            CheckRes(trans->Read(buffer.fCommand, sizeof(buffer.fCommand)));
            CheckRes(trans->Read(&(buffer.fFlags), sizeof(buffer.fFlags)));

            fCommandList.push_back(buffer);
        }

        fDone = true;

        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        if (trans == NULL)
        {
            fDone = true;
            return 0;
        }

        char terminator[JACK_UUID_STRING_SIZE];
        memset(terminator, 0, sizeof(terminator));

        CheckRes(JackResult::Write(trans));
        for (std::list<JackSessionCommand>::iterator i = fCommandList.begin(); i != fCommandList.end(); i++) {
            CheckRes(trans->Write(i->fUUID, sizeof(i->fUUID)));
            CheckRes(trans->Write(i->fClientName, sizeof(i->fClientName)));
            CheckRes(trans->Write(i->fCommand, sizeof(i->fCommand)));
            CheckRes(trans->Write(&(i->fFlags), sizeof(i->fFlags)));
        }
        CheckRes(trans->Write(terminator, sizeof(terminator)));
        return 0;
    }

    jack_session_command_t* GetCommands()
    {
        /* TODO: some kind of signal should be used instead */
        while (!fDone)
        {
            JackSleep(50000);    /* 50 ms */
        }

        jack_session_command_t* session_command = (jack_session_command_t *)malloc(sizeof(jack_session_command_t) * (fCommandList.size() + 1));
        int i = 0;

        for (std::list<JackSessionCommand>::iterator ci = fCommandList.begin(); ci != fCommandList.end(); ci++) {
            session_command[i].uuid = strdup(ci->fUUID);
            session_command[i].client_name = strdup(ci->fClientName);
            session_command[i].command = strdup(ci->fCommand);
            session_command[i].flags = ci->fFlags;
            i += 1;
        }

        session_command[i].uuid = NULL;
        session_command[i].client_name = NULL;
        session_command[i].command = NULL;
        session_command[i].flags = (jack_session_flags_t)0;

        return session_command;
    }
};

/*!
\brief SessionNotify request.
*/

struct JackSessionNotifyRequest : public JackRequestImpl<JACK_MESSAGE_SIZE_1 + JACK_CLIENT_NAME_SIZE_1 + sizeof(jack_session_event_type_t) + sizeof(int)>
{
    char fPath[JACK_MESSAGE_SIZE_1];
    char fDst[JACK_CLIENT_NAME_SIZE_1];
    jack_session_event_type_t fEventType;
    int fRefNum;

    JackSessionNotifyRequest()
        : JackRequestImpl(kSessionNotify), fEventType(JackSessionSave), fRefNum(0)
    {}

    JackSessionNotifyRequest(int refnum, const char* path, jack_session_event_type_t type, const char* dst)
        : JackRequestImpl(kSessionNotify), fEventType(type), fRefNum(refnum)
    {
        memset(fPath, 0, sizeof(fPath));
        memset(fDst, 0, sizeof(fDst));
        strncpy(fPath, path, sizeof(fPath)-1);
        if (dst) {
            strncpy(fDst, dst, sizeof(fDst)-1);
        }
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(fRefNum)));
        CheckRes(trans->Read(&fPath, sizeof(fPath)));
        CheckRes(trans->Read(&fDst, sizeof(fDst)));
        CheckRes(trans->Read(&fEventType, sizeof(fEventType)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(fRefNum)));
        CheckRes(WritePacket(&fPath, sizeof(fPath)));
        CheckRes(WritePacket(&fDst, sizeof(fDst)));
        CheckRes(WritePacket(&fEventType, sizeof(fEventType)));
        return JackRequestImpl::Write(trans);
    }

};

struct JackSessionReplyRequest : public JackRequestImpl<sizeof(int)>
{
    int fRefNum;

    JackSessionReplyRequest()
        : JackRequestImpl(kSessionReply), fRefNum(0)
    {}

    JackSessionReplyRequest(int refnum)
        : JackRequestImpl(kSessionReply), fRefNum(refnum)
    {}

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fRefNum, sizeof(int)));
        return JackRequestImpl::Write(trans);
    }

};

struct JackClientNameResult : public JackResult
{
    char fName[JACK_CLIENT_NAME_SIZE+1];

    JackClientNameResult(): JackResult()
    {
        memset(fName, 0, sizeof(fName));
    }
    JackClientNameResult(int32_t result, const char* name)
            : JackResult(result)
    {
        memset(fName, 0, sizeof(fName));
        strncpy(fName, name, sizeof(fName)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fName, sizeof(fName)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fName, sizeof(fName)));
        return 0;
    }

};

struct JackUUIDResult : public JackResult
{
    char fUUID[JACK_UUID_STRING_SIZE];

    JackUUIDResult(): JackResult()
    {
        memset(fUUID, 0, sizeof(fUUID));
    }
    JackUUIDResult(int32_t result, const char* uuid)
            : JackResult(result)
    {
        memset(fUUID, 0, sizeof(fUUID));
        strncpy(fUUID, uuid, sizeof(fUUID)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Read(trans));
        CheckRes(trans->Read(&fUUID, sizeof(fUUID)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(JackResult::Write(trans));
        CheckRes(trans->Write(&fUUID, sizeof(fUUID)));
        return 0;
    }

};

struct JackGetUUIDRequest : public JackRequestImpl<JACK_CLIENT_NAME_SIZE_1>
{
    char fName[JACK_CLIENT_NAME_SIZE_1];

    JackGetUUIDRequest()
        : JackRequestImpl(kGetUUIDByClient)
    {
        memset(fName, 0, sizeof(fName));
    }

    JackGetUUIDRequest(const char* client_name)
        : JackRequestImpl(kGetUUIDByClient)
    {
        memset(fName, 0, sizeof(fName));
        strncpy(fName, client_name, sizeof(fName)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fName, sizeof(fName)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fName, sizeof(fName)));
        return JackRequestImpl::Write(trans);
    }

};

struct JackGetClientNameRequest : public JackRequestImpl<JACK_UUID_STRING_SIZE>
{
    char fUUID[JACK_UUID_STRING_SIZE];

    JackGetClientNameRequest()
        : JackRequestImpl(kGetClientByUUID)
    {
        memset(fUUID, 0, sizeof(fUUID));
    }

    JackGetClientNameRequest(const char* uuid)
        : JackRequestImpl(kGetClientByUUID)
    {
        memset(fUUID, 0, sizeof(fUUID));
        strncpy(fUUID, uuid, sizeof(fUUID)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fUUID, sizeof(fUUID)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fUUID, sizeof(fUUID)));
        return JackRequestImpl::Write(trans);
    }

};

struct JackReserveNameRequest : public JackRequestImpl<sizeof(int) + JACK_CLIENT_NAME_SIZE_1 + JACK_UUID_STRING_SIZE>
{
    int  fRefNum;
    char fName[JACK_CLIENT_NAME_SIZE_1];
    char fUUID[JACK_UUID_STRING_SIZE];

    JackReserveNameRequest()
        : JackRequestImpl(kReserveClientName), fRefNum(0)
    {
        memset(fName, 0, sizeof(fName));
        memset(fUUID, 0, sizeof(fUUID));
    }

    JackReserveNameRequest(int refnum, const char *name, const char* uuid)
        : JackRequestImpl(kReserveClientName), fRefNum(refnum)
    {
        memset(fName, 0, sizeof(fName));
        memset(fUUID, 0, sizeof(fUUID));
        strncpy(fName, name, sizeof(fName)-1);
        strncpy(fUUID, uuid, sizeof(fUUID)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fUUID, sizeof(fUUID)));
        CheckRes(trans->Read(&fName, sizeof(fName)));
        CheckRes(trans->Read(&fRefNum, sizeof(fRefNum)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fUUID, sizeof(fUUID)));
        CheckRes(WritePacket(&fName, sizeof(fName)));
        CheckRes(WritePacket(&fRefNum, sizeof(fRefNum)));
        return JackRequestImpl::Write(trans);
    }

};

struct JackClientHasSessionCallbackRequest : public JackRequestImpl<JACK_CLIENT_NAME_SIZE_1>
{
    char fName[JACK_CLIENT_NAME_SIZE_1];

    JackClientHasSessionCallbackRequest()
        : JackRequestImpl(kClientHasSessionCallback)
    {
        memset(fName, 0, sizeof(fName));
    }

    JackClientHasSessionCallbackRequest(const char *name)
        : JackRequestImpl(kClientHasSessionCallback)
    {
        memset(fName, 0, sizeof(fName));
        strncpy(fName, name, sizeof(fName)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fName, sizeof(fName)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fName, sizeof(fName)));
        return JackRequestImpl::Write(trans);
    }

};


struct JackPropertyChangeNotifyRequest : public JackRequestImpl<sizeof(jack_uuid_t) + MAX_PATH_1 + sizeof(jack_property_change_t)>
{
    jack_uuid_t fSubject;
    char fKey[MAX_PATH_1];
    jack_property_change_t fChange;

    JackPropertyChangeNotifyRequest()
        : JackRequestImpl(kPropertyChangeNotify), fChange((jack_property_change_t)0)
    {
        jack_uuid_clear(&fSubject);
        memset(fKey, 0, sizeof(fKey));
    }

    JackPropertyChangeNotifyRequest(jack_uuid_t subject, const char* key, jack_property_change_t change)
        : JackRequestImpl(kPropertyChangeNotify), fChange(change)
    {
        jack_uuid_copy(&fSubject, subject);
        memset(fKey, 0, sizeof(fKey));
        if (key)
            strncpy(fKey, key, sizeof(fKey)-1);
    }

    int Read(detail::JackChannelTransactionInterface* trans) override
    {
        CheckSize();
        CheckRes(trans->Read(&fSubject, sizeof(fSubject)));
        CheckRes(trans->Read(&fKey, sizeof(fKey)));
        CheckRes(trans->Read(&fChange, sizeof(fChange)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans) override
    {
        CheckRes(WritePacketInit());
        CheckRes(WritePacket(&fSubject, sizeof(fSubject)));
        CheckRes(WritePacket(&fKey, sizeof(fKey)));
        CheckRes(WritePacket(&fChange, sizeof(fChange)));
        return JackRequestImpl::Write(trans);
    }

};

/*!
\brief ClientNotification.
*/

struct JackClientNotification
{
    int fSize;
    char fName[JACK_CLIENT_NAME_SIZE+1];
    int fRefNum;
    int fNotify;
    int fValue1;
    int fValue2;
    int fSync;
    char fMessage[JACK_MESSAGE_SIZE+1];

    JackClientNotification(): fSize(0), fRefNum(0), fNotify(-1), fValue1(-1), fValue2(-1), fSync(0)
    {
        memset(fName, 0, sizeof(fName));
        memset(fMessage, 0, sizeof(fMessage));
    }
    JackClientNotification(const char* name, int refnum, int notify, int sync, const char* message, int value1, int value2)
            : fRefNum(refnum), fNotify(notify), fValue1(value1), fValue2(value2), fSync(sync)
    {
        memset(fName, 0, sizeof(fName));
        memset(fMessage, 0, sizeof(fMessage));
        strncpy(fName, name, sizeof(fName)-1);
        if (message) {
            strncpy(fMessage, message, sizeof(fMessage)-1);
        }
        fSize = Size();
    }

    int Read(detail::JackChannelTransactionInterface* trans)
    {
        CheckSize();
        CheckRes(trans->Read(&fName, sizeof(fName)));
        CheckRes(trans->Read(&fRefNum, sizeof(int)));
        CheckRes(trans->Read(&fNotify, sizeof(int)));
        CheckRes(trans->Read(&fValue1, sizeof(int)));
        CheckRes(trans->Read(&fValue2, sizeof(int)));
        CheckRes(trans->Read(&fSync, sizeof(int)));
        CheckRes(trans->Read(&fMessage, sizeof(fMessage)));
        return 0;
    }

    int Write(detail::JackChannelTransactionInterface* trans)
    {
        CheckRes(trans->Write(&fSize, sizeof(int)));
        CheckRes(trans->Write(&fName, sizeof(fName)));
        CheckRes(trans->Write(&fRefNum, sizeof(int)));
        CheckRes(trans->Write(&fNotify, sizeof(int)));
        CheckRes(trans->Write(&fValue1, sizeof(int)));
        CheckRes(trans->Write(&fValue2, sizeof(int)));
        CheckRes(trans->Write(&fSync, sizeof(int)));
        CheckRes(trans->Write(&fMessage, sizeof(fMessage)));
        return 0;
    }

    int Size() const { return sizeof(int) + sizeof(fName) + 5 * sizeof(int) + sizeof(fMessage); }

};

} // end of namespace

#endif
