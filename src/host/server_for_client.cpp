/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "server_for_client.h"
#ifndef TEST_HASH
#include "hdc_hash_gen.h"
#endif
#include "server.h"
#ifdef __OHOS__
#include <sys/un.h>
#endif
#include "host_shell_option.h"
namespace Hdc {
static const int MAX_RETRY_COUNT = 500;
static const int MAX_CONNECT_DEVICE_RETRY_COUNT = 100;
#ifdef __OHOS__
static const int MAX_CONNECTIONS_COUNT = 16;
#endif

HdcServerForClient::HdcServerForClient(const bool serverOrClient, const string &addrString, void *pClsServer,
                                       uv_loop_t *loopMainIn)
    : HdcChannelBase(serverOrClient, addrString, loopMainIn)
{
    clsServer = pClsServer;
}

HdcServerForClient::~HdcServerForClient()
{
    WRITE_LOG(LOG_DEBUG, "~HdcServerForClient");
}

void HdcServerForClient::Stop()
{
    Base::TryCloseHandle((uv_handle_t *)&tcpListen);
#ifdef __OHOS__
    Base::TryCloseHandle((uv_handle_t *)&udsListen);
#endif
}

uint16_t HdcServerForClient::GetTCPListenPort()
{
    return channelPort;
}

#ifdef __OHOS__
void HdcServerForClient::AcceptUdsClient(uv_stream_t *server, int status)
{
    StartTraceScope("HdcServerForClient::AcceptUdsClient");
    HdcServerForClient *thisClass = (HdcServerForClient *)(((uv_pipe_t *)server)->data);
    CALLSTAT_GUARD(thisClass->loopMainStatus, server->loop, "HdcServerForClient::AcceptUdsClient");
    HChannel hChannel = new HdcChannel();
    if (hChannel == nullptr) {
        WRITE_LOG(LOG_FATAL, "AcceptUdsClient new channel fail");
        return;
    }
    hChannel->isUds = true;
    uint32_t uid = thisClass->MallocChannel(&hChannel);
    hChannel->startTime = Base::GetRuntimeMSec();
    int rc = 0;
    if ((rc = uv_accept(server, (uv_stream_t *)&hChannel->hWorkUds)) < 0) {
        WRITE_LOG(LOG_FATAL, "AcceptUdsClient uv_accept error rc:%d uid:%u", rc, uid);
        thisClass->FreeChannel(uid);
        return;
    }
    WRITE_LOG(LOG_DEBUG, "AcceptUdsClient uid:%u", uid);
    // limit first recv
    int bufMaxSize = 0;
    uv_recv_buffer_size((uv_handle_t *)&hChannel->hWorkUds, &bufMaxSize);
    auto funcChannelHeaderAlloc = [](uv_handle_t *handle, size_t sizeWanted, uv_buf_t *buf) -> void {
        HChannel context = (HChannel)handle->data;
        Base::ReallocBuf(&context->ioBuf, &context->bufSize, Base::GetMaxBufSize() * BUF_EXTEND_SIZE);
        buf->base = (char *)context->ioBuf + context->availTailIndex;
#ifdef HDC_VERSION_CHECK
        buf->len = sizeof(struct ChannelHandShake) + DWORD_SERIALIZE_SIZE;  // only recv static size
#else
        buf->len = offsetof(struct ChannelHandShake, version) + DWORD_SERIALIZE_SIZE;
#endif
    };
    // first packet static size, after this packet will be dup for normal recv
    uv_read_start((uv_stream_t *)&hChannel->hWorkUds, funcChannelHeaderAlloc, ReadStream);
    // channel handshake step1
    struct ChannelHandShake handShake = {};
    if (EOK == strcpy_s(handShake.banner, sizeof(handShake.banner), HANDSHAKE_MESSAGE.c_str())) {
        handShake.banner[BANNER_FEATURE_TAG_OFFSET] = HUGE_BUF_TAG; // set feature tag for huge buf size
        handShake.banner[SERVICE_KILL_OFFSET] = SERVICE_KILL_TAG;
        handShake.channelId = htonl(hChannel->channelId);
        string ver = Base::GetVersion() + HDC_MSG_HASH;
        WRITE_LOG(LOG_DEBUG, "Server ver:%s", ver.c_str());
        if (EOK != strcpy_s(handShake.version, sizeof(handShake.version), ver.c_str())) {
            WRITE_LOG(LOG_FATAL, "strcpy_s failed");
            return;
        }
#ifdef HDC_VERSION_CHECK
    thisClass->Send(hChannel->channelId, (uint8_t *)&handShake, sizeof(struct ChannelHandShake));
#else
    // do not send version message if check feature disable
    thisClass->Send(hChannel->channelId, reinterpret_cast<uint8_t *>(&handShake),
                    offsetof(struct ChannelHandShake, version));
#endif
    }
}
#endif

void HdcServerForClient::AcceptClient(uv_stream_t *server, int status)
{
    StartTraceScope("HdcServerForClient::AcceptClient");
    uv_tcp_t *pServTCP = (uv_tcp_t *)server;
    HdcServerForClient *thisClass = (HdcServerForClient *)pServTCP->data;
    CALLSTAT_GUARD(thisClass->loopMainStatus, server->loop, "HdcServerForClient::AcceptClient");
#ifdef HOST_OHOS
    HChannel hChannel = new HdcChannel();
    if (hChannel == nullptr) {
        WRITE_LOG(LOG_FATAL, "AcceptClient new channel fail");
        return;
    }
#else
    HChannel hChannel = nullptr;
#endif
    uint32_t uid = thisClass->MallocChannel(&hChannel);
    if (!hChannel) {
        WRITE_LOG(LOG_FATAL, "AcceptClient hChannel is nullptr");
        return;
    }
    hChannel->startTime = Base::GetRuntimeMSec();
    int rc = uv_accept(server, (uv_stream_t *)&hChannel->hWorkTCP);
    if (rc < 0) {
        WRITE_LOG(LOG_FATAL, "AcceptClient uv_accept error rc:%d uid:%u", rc, uid);
        thisClass->FreeChannel(uid);
        return;
    }
    WRITE_LOG(LOG_DEBUG, "AcceptClient uid:%u", uid);
    // limit first recv
    int bufMaxSize = 0;
    uv_recv_buffer_size((uv_handle_t *)&hChannel->hWorkTCP, &bufMaxSize);
    auto funcChannelHeaderAlloc = [](uv_handle_t *handle, size_t sizeWanted, uv_buf_t *buf) -> void {
        HChannel context = (HChannel)handle->data;
        Base::ReallocBuf(&context->ioBuf, &context->bufSize, Base::GetMaxBufSize() * BUF_EXTEND_SIZE);
        buf->base = (char *)context->ioBuf + context->availTailIndex;
#ifdef HDC_VERSION_CHECK
        buf->len = sizeof(struct ChannelHandShake) + DWORD_SERIALIZE_SIZE;  // only recv static size
#else
        buf->len = offsetof(struct ChannelHandShake, version) + DWORD_SERIALIZE_SIZE;
#endif
    };
    // first packet static size, after this packet will be dup for normal recv
    uv_read_start((uv_stream_t *)&hChannel->hWorkTCP, funcChannelHeaderAlloc, ReadStream);
    // channel handshake step1
    struct ChannelHandShake handShake = {};
    if (EOK == strcpy_s(handShake.banner, sizeof(handShake.banner), HANDSHAKE_MESSAGE.c_str())) {
        handShake.banner[BANNER_FEATURE_TAG_OFFSET] = HUGE_BUF_TAG; // set feature tag for huge buf size
#ifdef HOST_OHOS
        handShake.banner[SERVICE_KILL_OFFSET] = SERVICE_KILL_TAG;
#endif
        handShake.channelId = htonl(hChannel->channelId);
        string ver = Base::GetVersion() + HDC_MSG_HASH;
        WRITE_LOG(LOG_DEBUG, "Server ver:%s", ver.c_str());
        if (EOK != strcpy_s(handShake.version, sizeof(handShake.version), ver.c_str())) {
            WRITE_LOG(LOG_FATAL, "strcpy_s failed");
            return;
        }
#ifdef HDC_VERSION_CHECK
    thisClass->Send(hChannel->channelId, (uint8_t *)&handShake, sizeof(struct ChannelHandShake));
#else
    // do not send version message if check feature disable
    thisClass->Send(hChannel->channelId, reinterpret_cast<uint8_t *>(&handShake),
                    offsetof(struct ChannelHandShake, version));
#endif
    }
}

#ifdef __OHOS__
bool HdcServerForClient::SetUdsListen()
{
    udsListen.data = this;
    int ret = -1;
    ret = uv_pipe_init(loopMain, &udsListen, 0);

    unlink(UDS_PATH.c_str());

    if ((ret = uv_pipe_bind(&udsListen, UDS_PATH.c_str())) != 0) {
        WRITE_LOG(LOG_WARN, "bind uds addr fail! ret:%d", ret);
        return false;
    }

    uv_fs_t req = {};
    ret = uv_fs_chmod(nullptr, &req, UDS_PATH.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
        nullptr);
    if (ret < 0) {
        char buffer[BUF_SIZE_DEFAULT] = { 0 };
        uv_strerror_r(ret, buffer, BUF_SIZE_DEFAULT);
        WRITE_LOG(LOG_WARN, "uv_fs_chmod uds failed %s", buffer);
        uv_fs_req_cleanup(&req);
        return false;
    }
    uv_fs_req_cleanup(&req);

    if ((ret = uv_listen((uv_stream_t *)&udsListen, MAX_CONNECTIONS_COUNT, AcceptUdsClient)) != 0) {
        WRITE_LOG(LOG_WARN, "uds listen fail! ret:%d", ret);
        return false;
    }
    return true;
}
#endif

bool HdcServerForClient::SetTCPListen()
{
    char buffer[BUF_SIZE_DEFAULT] = { 0 };
    tcpListen.data = this;
    struct sockaddr_in6 addr;
    uv_tcp_init(loopMain, &tcpListen);

    if (Base::GetIsServerFlag()) {
        WRITE_LOG(LOG_DEBUG, "channelHost %s, port: %d", Hdc::MaskString(channelHost).c_str(), channelPort);
    } else {
        WRITE_LOG(LOG_DEBUG, "channelHost %s, port: %d", channelHost.c_str(), channelPort);
    }
    int rc = uv_ip6_addr(channelHost.c_str(), channelPort, &addr);
    if (rc != 0) {
        uv_strerror_r(rc, buffer, BUF_SIZE_DEFAULT);
        WRITE_LOG(LOG_FATAL, "uv_ip6_addr %d %s", rc, buffer);
        return false;
    }
    rc = uv_tcp_bind(&tcpListen, (const struct sockaddr *)&addr, 0);
    if (rc != 0) {
        WRITE_LOG(LOG_WARN, "uv_tcp_bind ipv6 %d", rc);
        if (rc == -EAFNOSUPPORT) {
            size_t index = channelHost.find(IPV4_MAPPING_PREFIX);
            size_t size = IPV4_MAPPING_PREFIX.size();
            if (index != std::string::npos) {
                struct sockaddr_in addr4v;
                std::string ipv4 = channelHost.substr(index + size);
                uv_ip4_addr(ipv4.c_str(), channelPort, &addr4v);
                rc = uv_tcp_bind(&tcpListen, (const struct sockaddr *)&addr4v, 0);
                if (rc != 0) {
                    uv_strerror_r(rc, buffer, BUF_SIZE_DEFAULT);
                    if (Base::GetIsServerFlag()) {
                        WRITE_LOG(LOG_FATAL, "uv_tcp_bind ipv4 %s failed %d %s",
                            Hdc::MaskString(ipv4).c_str(), rc, buffer);
                    } else {
                        WRITE_LOG(LOG_FATAL, "uv_tcp_bind ipv4 %s failed %d %s",
                            ipv4.c_str(), rc, buffer);
                    }
                    return false;
                }
            }
        } else {
            uv_strerror_r(rc, buffer, BUF_SIZE_DEFAULT);
            WRITE_LOG(LOG_FATAL, "uv_tcp_bind %d %s", rc, buffer);
            return false;
        }
    }
    int backLog = 128;
    rc = uv_listen((uv_stream_t *)&tcpListen, backLog, (uv_connection_cb)AcceptClient);
    if (rc != 0) {
        uv_strerror_r(rc, buffer, BUF_SIZE_DEFAULT);
        WRITE_LOG(LOG_FATAL, "uv_listen %d %s", rc, buffer);
        return false;
    }
    return true;
}

int HdcServerForClient::Initial()
{
    if (!clsServer) {
        WRITE_LOG(LOG_FATAL, "Module client initial failed");
        return -1;
    }
#ifndef __OHOS__
    if (!channelHostPort.size() || !channelHost.size() || !channelPort) {
        WRITE_LOG(LOG_FATAL, "Listen string initial failed");
        return -2;  // -2:err for Listen initial failed
    }
    if (!SetTCPListen()) {
        WRITE_LOG(LOG_FATAL, "SetTCPListen failed");
        int listenError = -3;  // -3:error for SetTCPListen failed
        return listenError;
    }
#else
    if (!channelHostPort.size() || !channelHost.size() || !channelPort) {
        WRITE_LOG(LOG_FATAL, "Listen string initial failed");
    } else {
        if (!SetTCPListen()) {
            WRITE_LOG(LOG_FATAL, "SetTCPListen failed");
        }
    }
    if (!SetUdsListen()) {
        WRITE_LOG(LOG_FATAL, "SetUdsListen failed");
        int listenError = -3; // -3:error for SetUdsListen failed
        return listenError;
    }
#endif
    
    return 0;
}

void HdcServerForClient::EchoClient(HChannel hChannel, MessageLevel level, const char *msg, ...)
{
    StartTraceScope("HdcServerForClient::EchoClient");
    string logInfo = "";
    switch (level) {
        case MSG_FAIL:
            logInfo = MESSAGE_FAIL;
            break;
        case MSG_INFO:
            logInfo = MESSAGE_INFO;
            break;
        default:  // successful, not append extra info
            break;
    }
    va_list vaArgs;
    va_start(vaArgs, msg);
    string log = logInfo + Base::StringFormat(msg, vaArgs);
    va_end(vaArgs);
    if (log.back() != '\n') {
        log += "\r\n";
    }
    SendChannel(hChannel, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(log.c_str())), log.size());
}

void HdcServerForClient::EchoClientRaw(const HChannel hChannel, uint8_t *payload, const int payloadSize)
{
    SendChannel(hChannel, payload, payloadSize);
}

// HdcServerForClient passthrough file command to client
void HdcServerForClient::SendCommandToClient(const HChannel hChannel, const uint16_t commandFlag,
                                             uint8_t *payload, const int payloadSize)
{
    if (Base::CanPrintCmd(commandFlag)) {
        WRITE_LOG(LOG_INFO, "SendCommandToClient cid:%u sid:%u commandID:%u payloadSize:%d",
            hChannel->channelId, hChannel->targetSessionId, commandFlag, payloadSize);
    }
    SendChannelWithCmd(hChannel, commandFlag, payload, payloadSize);
}

bool HdcServerForClient::SendToDaemon(HChannel hChannel, const uint16_t commandFlag, uint8_t *bufPtr, const int bufSize)
{
    StartTraceScope("HdcServerForClient::SendToDaemon");
    HDaemonInfo hdi = nullptr;
    bool ret = false;
    HdcServer *ptrServer = (HdcServer *)clsServer;
    if (Base::CanPrintCmd(commandFlag)) {
        WRITE_LOG(LOG_INFO, "SendToDaemon cid:%u sid:%u key:%s commandID:%u sizeSend:%d", hChannel->channelId,
            hChannel->targetSessionId, Hdc::MaskString(hChannel->connectKey).c_str(), commandFlag, bufSize);
    }
    while (true) {
        ptrServer->AdminDaemonMap(OP_QUERY, hChannel->connectKey, hdi);
        if (hdi == nullptr) {
            WRITE_LOG(LOG_FATAL, "SendToDaemon hdi nullptr");
            break;
        }
        if (hdi->connStatus != STATUS_CONNECTED) {
            WRITE_LOG(LOG_FATAL, "SendToDaemon not connected");
            break;
        }
        if (!hdi->hSession) {
            WRITE_LOG(LOG_FATAL, "SendToDaemon hdi->hSession nullptr");
            break;
        }
        if (ptrServer->Send(hdi->hSession->sessionId, hChannel->channelId, commandFlag, bufPtr, bufSize) < 0) {
            WRITE_LOG(LOG_FATAL, "SendToDaemon Send failed channelId:%u", hChannel->channelId);
            break;
        }
        ret = true;
        break;
    }
    return ret;
}

void HdcServerForClient::OrderFindTargets(HChannel hChannel)
{
    int count = 0;
    EchoClient(hChannel, MSG_INFO, "Please add HDC server's firewall ruler to allow udp incoming, udpport:%d",
               DEFAULT_PORT);
    HdcServer *ptrServer = (HdcServer *)clsServer;
    ptrServer->clsTCPClt->FindLanDaemon();
    list<string> &lst = ptrServer->clsTCPClt->lstDaemonResult;
    // refresh main list
    HdcDaemonInformation di;
    while (!lst.empty()) {
        di = {};
        ++count;
        di.connectKey = lst.front();
        di.connType = CONN_TCP;
        di.connStatus = STATUS_READY;
        di.inited = false;
        HDaemonInfo pDi = reinterpret_cast<HDaemonInfo>(&di);
        ptrServer->AdminDaemonMap(OP_ADD, STRING_EMPTY, pDi);
        lst.pop_front();
    }
    EchoClient(hChannel, MSG_INFO, "Broadcast find daemon, total:%d", count);
#ifdef UNIT_TEST
    string bufString = std::to_string(count);
    Base::WriteBinFile((UT_TMP_PATH + "/base-discover.result").c_str(), (uint8_t *)bufString.c_str(), bufString.size(),
                       true);
#endif
}

void HdcServerForClient::OrderConnecTargetResult(uv_timer_t *req)
{
    HChannel hChannel = (HChannel)req->data;
    HdcServerForClient *thisClass = (HdcServerForClient *)hChannel->clsChannel;
    HdcServer *ptrServer = (HdcServer *)thisClass->clsServer;
    bool bConnectOK = false;
    bool bExitRepet = false;
    HDaemonInfo hdi = nullptr;
    string sRet;
    string target = std::string(hChannel->bufStd + 2);
    if (target == "any") {
        ptrServer->AdminDaemonMap(OP_GET_ANY, target, hdi);
    } else {
        ptrServer->AdminDaemonMap(OP_QUERY, target, hdi);
    }
    if (hdi && hdi->connStatus == STATUS_CONNECTED) {
        bConnectOK = true;
    }
    while (true) {
        if (bConnectOK) {
            bExitRepet = true;
            if (hChannel->isCheck) {
                WRITE_LOG(LOG_INFO, "%s check device success and remove %s", __FUNCTION__, hChannel->key.c_str());
                thisClass->CommandRemoveSession(hChannel, hChannel->key.c_str());
                thisClass->EchoClient(hChannel, MSG_OK, const_cast<char *>(hdi->version.c_str()));
            } else {
                sRet = "Connect OK";
                thisClass->EchoClient(hChannel, MSG_OK, const_cast<char *>(sRet.c_str()));
            }
            break;
        } else {
            uint16_t *bRetryCount = reinterpret_cast<uint16_t *>(hChannel->bufStd);
            ++(*bRetryCount);
            if (*bRetryCount > MAX_RETRY_COUNT ||
                (hChannel->connectLocalDevice && *bRetryCount > MAX_CONNECT_DEVICE_RETRY_COUNT)) {
                // 5s or localDevice 1s
                bExitRepet = true;
                sRet = "Connect failed";
                thisClass->EchoClient(hChannel, MSG_FAIL, const_cast<char *>(sRet.c_str()));
                hdi->inited = false;
                hdi->connStatus = STATUS_OFFLINE;
                ptrServer->AdminDaemonMap(OP_UPDATE, target, hdi);
                break;
            }
        }
        break;
    }
    if (bExitRepet) {
        thisClass->FreeChannel(hChannel->channelId);
        Base::TryCloseHandle((const uv_handle_t *)req, Base::CloseTimerCallback);
    }
}

bool HdcServerForClient::NewConnectTry(void *ptrServer, HChannel hChannel, const string &connectKey, bool isCheck)
{
#ifdef HDC_DEBUG
    WRITE_LOG(LOG_ALL, "%s %s", __FUNCTION__, Hdc::MaskString(connectKey).c_str());
#endif
    int childRet = ((HdcServer *)ptrServer)->CreateConnect(connectKey, isCheck);
    bool ret = false;
    int connectError = -2;
    constexpr uint8_t bufOffsetTwo = 2;
    constexpr uint8_t bufOffsetThree = 3;
    if (childRet == -1) {
        EchoClient(hChannel, MSG_INFO, "Target is connected, repeat operation");
    } else if (childRet == connectError) {
        EchoClient(hChannel, MSG_FAIL, "CreateConnect failed");
        WRITE_LOG(LOG_FATAL, "CreateConnect failed");
    } else {
        size_t pos = connectKey.find(":");
        if (pos != std::string::npos) {
            string ip = connectKey.substr(0, pos);
            if (ip == "127.0.0.1") {
                hChannel->connectLocalDevice = true;
            }
        }
        (void)memset_s(hChannel->bufStd, sizeof(hChannel->bufStd), 0, sizeof(hChannel->bufStd));
        childRet = snprintf_s(hChannel->bufStd + bufOffsetTwo, sizeof(hChannel->bufStd) - bufOffsetTwo,
                              sizeof(hChannel->bufStd) - bufOffsetThree, "%s",
                              const_cast<char *>(connectKey.c_str()));
        if (childRet > 0) {
            Base::TimerUvTask(loopMain, hChannel, OrderConnecTargetResult, UV_START_REPEAT);
            ret = true;
        }
    }
    return ret;
}

bool HdcServerForClient::CommandRemoveSession(HChannel hChannel, const char *connectKey)
{
    HdcServer *ptrServer = (HdcServer *)clsServer;
    HDaemonInfo hdiOld = nullptr;
    (reinterpret_cast<HdcServer *>(ptrServer))->AdminDaemonMap(OP_QUERY, connectKey, hdiOld);
    if (hdiOld == nullptr || hdiOld->hSession == nullptr) {
        EchoClient(hChannel, MSG_FAIL, "No target available");
        WRITE_LOG(LOG_FATAL, "CommandRemoveSession No target available");
        return false;
    }
    (reinterpret_cast<HdcServer *>(ptrServer))->FreeSession(hdiOld->hSession->sessionId);
    return true;
}

bool HdcServerForClient::CommandRemoveForward(const string &forwardKey)
{
    StartTraceScope("HdcServerForClient::CommandRemoveForward");
    bool ret = RemoveFportkey("0|" + forwardKey);
    ret |= RemoveFportkey("1|" + forwardKey);
    return ret;
}

bool HdcServerForClient::RemoveFportkey(const string &forwardKey)
{
    StartTraceScope("HdcServerForClient::RemoveFportkey");
    HdcServer *ptrServer = (HdcServer *)clsServer;
    HForwardInfo hfi = nullptr;
    ptrServer->AdminForwardMap(OP_QUERY, forwardKey, hfi);
    if (!hfi) {
        WRITE_LOG(LOG_FATAL, "CommandRemoveForward hfi nullptr forwardKey:%s", forwardKey.c_str());
        return false;
    }
    HSession hSession = ptrServer->AdminSession(OP_QUERY, hfi->sessionId, nullptr);
    if (!hSession) {
        WRITE_LOG(LOG_FATAL, "CommandRemoveForward hSession nullptr sessionId:%u", hfi->sessionId);
        ptrServer->AdminForwardMap(OP_REMOVE, forwardKey, hfi);
        return true;
    }
    hSession->commandCount++;
    ptrServer->ClearOwnTasks(hSession, hfi->channelId);
    FreeChannel(hfi->channelId);
    hfi = nullptr;
    ptrServer->AdminForwardMap(OP_REMOVE, forwardKey, hfi);
    return true;
}

void HdcServerForClient::GetTargetList(HChannel hChannel, void *formatCommandInput)
{
    TranslateCommand::FormatCommand *formatCommand = (TranslateCommand::FormatCommand *)formatCommandInput;
    HdcServer *ptrServer = (HdcServer *)clsServer;
    uint16_t cmd = OP_GET_STRLIST;
    if (formatCommand->parameters == "v") {
        cmd = OP_GET_STRLIST_FULL;
    }
    HDaemonInfo hdi = nullptr;
    string sRet = ptrServer->AdminDaemonMap(cmd, STRING_EMPTY, hdi);
    if (!sRet.length()) {
        sRet = EMPTY_ECHO;
    }
    EchoClient(hChannel, MSG_OK, const_cast<char *>(sRet.c_str()));
#ifdef UNIT_TEST
    Base::WriteBinFile((UT_TMP_PATH + "/base-list.result").c_str(), (uint8_t *)MESSAGE_SUCCESS.c_str(),
                       MESSAGE_SUCCESS.size(), true);
#endif
}

bool HdcServerForClient::GetAnyTarget(HChannel hChannel)
{
    HdcServer *ptrServer = (HdcServer *)clsServer;
    HDaemonInfo hdi = nullptr;
    ptrServer->AdminDaemonMap(OP_GET_ANY, STRING_EMPTY, hdi);
    if (!hdi) {
        EchoClient(hChannel, MSG_FAIL, "No target available");
        return false;
    }
    // can not use hdi->connectKey.This memory may be released to re-Malloc
    string connectKey = hdi->connectKey;
    bool ret = NewConnectTry(ptrServer, hChannel, connectKey);
#ifdef UNIT_TEST
    Base::WriteBinFile((UT_TMP_PATH + "/base-any.result").c_str(), (uint8_t *)MESSAGE_SUCCESS.c_str(),
                       MESSAGE_SUCCESS.size(), true);
#endif
    return ret;
}

bool HdcServerForClient::WaitForAny(HChannel hChannel)
{
    HdcServer *ptrServer = (HdcServer *)clsServer;
    HDaemonInfo hdi = nullptr;
    if (!hChannel->connectKey.empty()) {
        ptrServer->AdminDaemonMap(OP_WAIT_FOR_ANY, hChannel->connectKey, hdi);
    } else {
        ptrServer->AdminDaemonMap(OP_WAIT_FOR_ANY, STRING_EMPTY, hdi);
    }
    if (!hdi) {
        EchoClient(hChannel, MSG_FAIL, "No any connected target");
        return false;
    }
    string key = hdi->connectKey;
    EchoClient(hChannel, MSG_OK, "Wait for connected target is %s", key.c_str());
    return true;
}

bool HdcServerForClient::RemoveForward(HChannel hChannel, const char *parameterString)
{
    StartTraceScope("HdcServerForClient::RemoveForward");
    HdcServer *ptrServer = (HdcServer *)clsServer;
    if (parameterString == nullptr) {  // remove all
        HForwardInfo hfi = nullptr;    // dummy
        string echo = ptrServer->AdminForwardMap(OP_GET_STRLIST, "", hfi);
        if (!echo.length()) {
            return false;
        }
        vector<string> filterStrings;
        Base::SplitString(echo, string("\n"), filterStrings);
        for (auto &&s : filterStrings) {
            if (CommandRemoveForward(s.c_str())) {
                EchoClient(hChannel, MSG_OK, "Remove forward ruler success, ruler:%s", s.c_str());
            } else {
                EchoClient(hChannel, MSG_FAIL, "Remove forward ruler failed, ruler is not exist %s", s.c_str());
            }
        }
    } else {  // remove single
        if (CommandRemoveForward(parameterString)) {
            EchoClient(hChannel, MSG_OK, "Remove forward ruler success, ruler:%s", parameterString);
        } else {
            EchoClient(hChannel, MSG_FAIL, "Remove forward ruler failed, ruler is not exist %s", parameterString);
        }
    }
    return true;
}

bool HdcServerForClient::DoCommandLocal(HChannel hChannel, void *formatCommandInput)
{
    StartTraceScope("HdcServerForClient::DoCommandLocal");
    TranslateCommand::FormatCommand *formatCommand = (TranslateCommand::FormatCommand *)formatCommandInput;
    HdcServer *ptrServer = (HdcServer *)clsServer;
    bool ret = false;
    // Main thread command, direct Listen main thread
    switch (formatCommand->cmdFlag) {
        case CMD_KERNEL_TARGET_DISCOVER: {
            OrderFindTargets(hChannel);
            hChannel->isSuccess = true;
            ret = false;
            break;
        }
        case CMD_KERNEL_TARGET_LIST: {
            GetTargetList(hChannel, formatCommandInput);
            hChannel->isSuccess = true;
            ret = false;
            break;
        }
        case CMD_SERVICE_START: {
            PrintLastError(hChannel);
            hChannel->isSuccess = true;
            ret = false;
            break;
        }
#ifdef HOST_OHOS
        case CMD_SERVER_KILL: {
            WRITE_LOG(LOG_FATAL, "CMD_SERVER_KILL command");
            ptrServer->SessionSoftReset();
            hChannel->isSuccess  = true;
            EchoClient(hChannel, MSG_OK, "Kill server finish");
            _exit(0);
        }
#endif
        case CMD_CHECK_SERVER: {
            WRITE_LOG(LOG_DEBUG, "CMD_CHECK_SERVER command");
            ReportServerVersion(hChannel);
            hChannel->isSuccess = true;
            ret = false;
            break;
        }
        case CMD_WAIT_FOR: {
            WRITE_LOG(LOG_DEBUG, "CMD_WAIT_FOR command");
            ret = !WaitForAny(hChannel);
            hChannel->isSuccess = true;
            break;
        }
        case CMD_KERNEL_TARGET_ANY: {
#ifdef HDC_DEBUG
            WRITE_LOG(LOG_DEBUG, "%s CMD_KERNEL_TARGET_ANY %s", __FUNCTION__, formatCommand->parameters.c_str());
#endif
            ret = GetAnyTarget(hChannel);
            hChannel->isSuccess = ret;
            break;
        }
        case CMD_KERNEL_TARGET_CONNECT: {
#ifdef HDC_DEBUG
            WRITE_LOG(LOG_DEBUG, "%s CMD_KERNEL_TARGET_CONNECT %s", __FUNCTION__, formatCommand->parameters.c_str());
#endif
            ret = NewConnectTry(ptrServer, hChannel, formatCommand->parameters.c_str());
            hChannel->isSuccess = ret;
            break;
        }
        case CMD_CHECK_DEVICE: {
            WRITE_LOG(LOG_INFO, "%s CMD_CHECK_DEVICE %s", __FUNCTION__, formatCommand->parameters.c_str());
            hChannel->isCheck = true;
            hChannel->key = formatCommand->parameters.c_str();
            ret = NewConnectTry(ptrServer, hChannel, formatCommand->parameters.c_str(), true);
            hChannel->isSuccess = ret;
            break;
        }
        case CMD_KERNEL_TARGET_DISCONNECT: {
            CommandRemoveSession(hChannel, formatCommand->parameters.c_str());
            hChannel->isSuccess = true;
            break;
        }
        // task will be global task，Therefore, it can only be controlled in the global session.
        case CMD_FORWARD_LIST: {
            HForwardInfo hfi = nullptr;  // dummy
            string echo = ptrServer->AdminForwardMap(OP_GET_STRLIST_FULL, "", hfi);
            if (!echo.length()) {
                echo = EMPTY_ECHO;
            }
            EchoClient(hChannel, MSG_OK, const_cast<char *>(echo.c_str()));
            hChannel->isSuccess = true;
            break;
        }
        case CMD_FORWARD_REMOVE: {
            RemoveForward(hChannel, formatCommand->parameters.c_str());
            hChannel->isSuccess = true;
            break;
        }
        case CMD_KERNEL_ENABLE_KEEPALIVE: {
            // just use for 'list targets' now
            hChannel->keepAlive = true;
            ret = true;
            hChannel->isSuccess = true;
            break;
        }
        default: {
            EchoClient(hChannel, MSG_FAIL, "ExecuteCommand need connect-key? please confirm a device by help info");
            FillChannelResult(hChannel, false, "found no devices");
            break;
        }
    }
    return ret;
}

bool HdcServerForClient::TaskCommand(HChannel hChannel, void *formatCommandInput)
{
    StartTraceScope("HdcServerForClient::TaskCommand");
    TranslateCommand::FormatCommand *formatCommand = (TranslateCommand::FormatCommand *)formatCommandInput;
    HdcServer *ptrServer = (HdcServer *)clsServer;
    string cmdFlag;
    uint8_t sizeCmdFlag = 0;
    if (formatCommand->cmdFlag == CMD_FILE_INIT) {
        cmdFlag = "send ";
        sizeCmdFlag = 5;  // 5: cmdFlag send size
        HandleRemote(hChannel, formatCommand->parameters, RemoteType::REMOTE_FILE);
    } else if (formatCommand->cmdFlag == CMD_FORWARD_INIT) {
        cmdFlag = "fport ";
        sizeCmdFlag = 6;  // 6: cmdFlag fport size
    } else if (formatCommand->cmdFlag == CMD_APP_INIT) {
        cmdFlag = "install ";
        sizeCmdFlag = 8;  // 8: cmdFlag install size
        HandleRemote(hChannel, formatCommand->parameters, RemoteType::REMOTE_APP);
    } else if (formatCommand->cmdFlag == CMD_APP_UNINSTALL) {
        cmdFlag = "uninstall ";
        sizeCmdFlag = 10;  // 10: cmdFlag uninstall size
    } else if (formatCommand->cmdFlag == CMD_UNITY_BUGREPORT_INIT) {
        cmdFlag = "bugreport ";
        sizeCmdFlag = 10;  // 10: cmdFlag bugreport size
    } else if (formatCommand->cmdFlag == CMD_APP_SIDELOAD) {
        cmdFlag = "sideload ";
        sizeCmdFlag = 9; // 9: cmdFlag sideload size
    } else if (formatCommand->cmdFlag == CMD_FLASHD_UPDATE_INIT) {
        cmdFlag = "update ";
        sizeCmdFlag = 7; // 7: cmdFlag update size
    } else if (formatCommand->cmdFlag == CMD_FLASHD_FLASH_INIT) {
        cmdFlag = "flash ";
        sizeCmdFlag = 6; // 6: cmdFlag flash size
    }
    int sizeSend = formatCommand->parameters.size();
    if (!strncmp(formatCommand->parameters.c_str(), cmdFlag.c_str(), sizeCmdFlag)) {  // local do
        HSession hSession = FindAliveSession(hChannel->targetSessionId);
        if (!hSession) {
            return false;
        }
        if ((formatCommand->cmdFlag == CMD_FILE_INIT || formatCommand->cmdFlag == CMD_APP_INIT) &&
            hChannel->fromClient) {
            // remote client mode, CMD_FILE_INIT and CMD_APP_INIT command send back to client
            WRITE_LOG(LOG_INFO, "CMD_FILE_INIT|CMD_APP_INIT command send back to remote client, cid:%u",
                hChannel->channelId);
            SendChannelWithCmd(hChannel, formatCommand->cmdFlag,
                reinterpret_cast<uint8_t *>(const_cast<char *>(formatCommand->parameters.c_str())) + sizeCmdFlag,
                sizeSend - sizeCmdFlag);
            return false;
        }
        ptrServer->DispatchTaskData(hSession, hChannel->channelId, formatCommand->cmdFlag,
            reinterpret_cast<uint8_t *>(const_cast<char *>(formatCommand->parameters.c_str())) + sizeCmdFlag,
            sizeSend - sizeCmdFlag);
    } else {  // Send to Daemon-side to do
        SendToDaemon(hChannel, formatCommand->cmdFlag,
            reinterpret_cast<uint8_t *>(const_cast<char *>(formatCommand->parameters.c_str())) + sizeCmdFlag,
            sizeSend - sizeCmdFlag);
    }
    return true;
}

void HdcServerForClient::HandleRemote(HChannel hChannel, string &parameters, RemoteType flag)
{
    StartTraceScope("HdcServerForClient::HandleRemote");
    hChannel->remote = flag;
    int argc = 0;
    char **argv = Base::SplitCommandToArgs(parameters.c_str(), &argc);
    for (int i = 0; i < argc; i++) {
        if (argv[i] == CMDSTR_REMOTE_PARAMETER) {
            hChannel->fromClient = true;
            WRITE_LOG(LOG_DEBUG, "remote client mode channelId:%u", hChannel->channelId);
            break;
        }
    }
    if (hChannel->fromClient) {
        string remote = CMDSTR_REMOTE_PARAMETER + " ";
        if (parameters.find(remote) != std::string::npos) {
            parameters.replace(parameters.find(remote), remote.size(), "");
            WRITE_LOG(LOG_DEBUG, "parameters: %s", Hdc::MaskString(parameters).c_str());
        }
    }
    delete[](reinterpret_cast<char *>(argv));
}

#ifdef __OHOS__
bool HdcServerForClient::IsServerTransfer(HChannel hChannel, uint16_t cmdFlag, string &parameters)
{
    if (cmdFlag == CMD_FILE_INIT || cmdFlag== CMD_APP_INIT) {
        HandleRemote(hChannel, parameters, RemoteType::REMOTE_FILE);
        if (!hChannel->fromClient) {
            EchoClient(hChannel, MSG_FAIL, "[E005200] Unsupport file command");
            return false;
        }
    }
    return true;
}
#endif

bool HdcServerForClient::DoCommandRemote(HChannel hChannel, void *formatCommandInput)
{
    StartTraceScope("HdcServerForClient::DoCommandRemote");
    TranslateCommand::FormatCommand *formatCommand = (TranslateCommand::FormatCommand *)formatCommandInput;
    bool ret = false;
    int sizeSend = formatCommand->parameters.size();
    switch (formatCommand->cmdFlag) {
        // Some simple commands only need to forward the instruction, no need to start Task
        case CMD_SHELL_INIT:
        case CMD_SHELL_DATA:
        case CMD_UNITY_EXECUTE:
        case CMD_UNITY_EXECUTE_EX:
        case CMD_UNITY_REMOUNT:
        case CMD_UNITY_REBOOT:
        case CMD_UNITY_RUNMODE:
        case CMD_UNITY_HILOG:
        case CMD_UNITY_ROOTRUN:
        case CMD_JDWP_TRACK:
        case CMD_JDWP_LIST: {
            if (!SendToDaemon(hChannel, formatCommand->cmdFlag,
                reinterpret_cast<uint8_t *>(const_cast<char *>(formatCommand->parameters.c_str())), sizeSend)) {
                break;
            }
            ret = true;
            if (formatCommand->cmdFlag == CMD_SHELL_INIT) {
                hChannel->interactiveShellMode = true;
            }
            break;
        }
        case CMD_FILE_INIT:
        case CMD_FORWARD_INIT:
        case CMD_APP_INIT: {
#ifdef __OHOS__
            if (!IsServerTransfer(hChannel, formatCommand->cmdFlag, formatCommand->parameters)) {
                return false;
            }
#endif
        }
        case CMD_APP_UNINSTALL:
        case CMD_UNITY_BUGREPORT_INIT:
        case CMD_APP_SIDELOAD:
        case CMD_FLASHD_UPDATE_INIT:
        case CMD_FLASHD_FLASH_INIT:
        case CMD_FLASHD_ERASE:
        case CMD_FLASHD_FORMAT: {
            TaskCommand(hChannel, formatCommandInput);
            ret = true;
            break;
        }
        default:
            break;
    }
    if (!ret) {
        EchoClient(hChannel, MSG_FAIL, "[E002106] Failed to communicate with daemon");
    }
    return ret;
}
// Do not specify Target's operations no longer need to put it in the thread.
bool HdcServerForClient::DoCommand(HChannel hChannel, void *formatCommandInput, HDaemonInfo &hdi)
{
    StartTraceScope("HdcServerForClient::DoCommand");
    bool ret = false;
    TranslateCommand::FormatCommand *formatCommand = (TranslateCommand::FormatCommand *)formatCommandInput;
#ifdef __OHOS__
    if ((!hChannel->hChildWorkTCP.loop && !hChannel->hChildWorkUds.loop) ||
#else
    if (!hChannel->hChildWorkTCP.loop ||
#endif
        formatCommand->cmdFlag == CMD_FORWARD_REMOVE ||
#ifdef HOST_OHOS
        formatCommand->cmdFlag == CMD_SERVER_KILL ||
#endif
        formatCommand->cmdFlag == CMD_SERVICE_START) {
        hChannel->commandFlag = formatCommand->cmdFlag;
        hChannel->commandParameters = formatCommand->parameters;
        // Main thread command, direct Listen main thread
        ret = DoCommandLocal(hChannel, formatCommandInput);
    } else {  // CONNECT DAEMON's work thread command, non-primary thread
        if (!CommandMatchDaemonFeature(formatCommand->cmdFlag, hdi)) {
            // only the cmdFlag in the FEATURE_VERSION_MATCH_MAP needs to be checked, others to permissive.
            WRITE_LOG(LOG_WARN, "unsupport cmdFlag: %d, due to daemon feature dismatch", formatCommand->cmdFlag);
            EchoClient(hChannel, MSG_FAIL, "[E002105] Unsupport command");
            return false;
        }
        if (formatCommand->cmdFlag != CMD_SHELL_DATA) {
            hChannel->commandFlag = formatCommand->cmdFlag;
            hChannel->commandParameters = formatCommand->parameters;
        }
        ret = DoCommandRemote(hChannel, formatCommandInput);
    }
    return ret;
}

// just call from BindChannelToSession
HSession HdcServerForClient::FindAliveSessionFromDaemonMap(const HChannel hChannel)
{
    StartTraceScope("HdcServerForClient::FindAliveSessionFromDaemonMap");
    HSession hSession = nullptr;
    HDaemonInfo hdi = nullptr;
    HdcServer *ptrServer = (HdcServer *)clsServer;
    ptrServer->AdminDaemonMap(OP_QUERY, hChannel->connectKey, hdi);
    if (!hdi) {
        WRITE_LOG(LOG_WARN, "Not match target founded cid:%u", hChannel->channelId);
        FillChannelResult(hChannel, false, "no match targets found");
        EchoClient(hChannel, MSG_FAIL, "Not match target founded, check connect-key please");
        return nullptr;
    }
    if (hdi->connStatus != STATUS_CONNECTED) {
        WRITE_LOG(LOG_WARN, "Device not found or connected cid:%u", hChannel->channelId);
        FillChannelResult(hChannel, false, "device not found or connected");
        EchoClient(hChannel, MSG_FAIL, "[E001005] Device not found or connected");
        return nullptr;
    }
    if (hdi->hSession == nullptr || hdi->hSession->isDead) {
        WRITE_LOG(LOG_WARN, "Bind tartget session is null or dead cid:%u", hChannel->channelId);
        FillChannelResult(hChannel, false, "bind tartget session is null or dead");
        EchoClient(hChannel, MSG_FAIL, "Bind tartget session is dead");
        return nullptr;
    }
    if (!hdi->hSession->handshakeOK) {
        WRITE_LOG(LOG_WARN, "hSession handShake is false sid:%u cid:%u",
            hdi->hSession->sessionId, hChannel->channelId);
        FillChannelResult(hChannel, false, "handshake is not ready");
        const string errMsg = "[E000004]:The communication channel is being established.\r\n"\
            "Please wait for several seconds and try again.";
        EchoClient(hChannel, MSG_FAIL, errMsg.c_str());
        return nullptr;
    }
    hSession = reinterpret_cast<HSession>(hdi->hSession);
    return hSession;
}

#ifdef __OHOS__
int HdcServerForClient::BindChannelToSession(HChannel hChannel, uint8_t *bufPtr, const int bytesIO)
{
    StartTraceScope("HdcServerForClient::BindChannelToSession");
    if (FindAliveSessionFromDaemonMap(hChannel) == nullptr) {
        WRITE_LOG(LOG_FATAL, "Find no alive session channelId:%u", hChannel->channelId);
        return ERR_SESSION_NOFOUND;
    }
    bool isClosing = false;
    if (!hChannel->isUds) {
        isClosing = uv_is_closing((const uv_handle_t *)&hChannel->hWorkTCP);
        if (!isClosing && (hChannel->fdChildWorkTCP = Base::DuplicateUvSocket(&hChannel->hWorkTCP)) < 0) {
            WRITE_LOG(LOG_FATAL, "Duplicate socket failed channelId:%u", hChannel->channelId);
            return ERR_SOCKET_DUPLICATE;
        }
    } else {
        isClosing = uv_is_closing((const uv_handle_t *)&hChannel->hWorkUds);
        if (!isClosing && (hChannel->fdChildWorkTCP = Base::DuplicateUvPipe(&hChannel->hWorkUds)) < 0) {
            WRITE_LOG(LOG_FATAL, "Duplicate pipe failed channelId:%u", hChannel->channelId);
            return ERR_SOCKET_DUPLICATE;
        }
    }

    uv_close_cb funcWorkTcpClose = [](uv_handle_t *handle) -> void {
        HChannel hChannel = (HChannel)handle->data;
        --hChannel->ref;
    };
    ++hChannel->ref;
    if (!isClosing) {
        if (!hChannel->isUds) {
            uv_close((uv_handle_t *)&hChannel->hWorkTCP, funcWorkTcpClose);
        } else {
            uv_close((uv_handle_t *)&hChannel->hWorkUds, funcWorkTcpClose);
        }
    }
    Base::DoNextLoop(loopMain, hChannel, [](const uint8_t flag, string &msg, const void *data) {
        // Thread message can avoid using thread lock and improve program efficiency
        // If not next loop call, ReadStream will thread conflict
        HChannel hChannel = (HChannel)data;
        auto thisClass = (HdcServerForClient *)hChannel->clsChannel;
        HSession hSession = nullptr;
        if ((hSession = thisClass->FindAliveSessionFromDaemonMap(hChannel)) == nullptr) {
            WRITE_LOG(LOG_FATAL, "hSession nullptr channelId:%u", hChannel->channelId);
            return;
        }
        auto ctrl = HdcSessionBase::BuildCtrlString(SP_ATTACH_CHANNEL, hChannel->channelId, nullptr, 0);
        Base::SendToPollFd(hSession->ctrlFd[STREAM_MAIN], ctrl.data(), ctrl.size());
    });
    return RET_SUCCESS;
}
#else
int HdcServerForClient::BindChannelToSession(HChannel hChannel, uint8_t *bufPtr, const int bytesIO)
{
    StartTraceScope("HdcServerForClient::BindChannelToSession");
    if (FindAliveSessionFromDaemonMap(hChannel) == nullptr) {
        WRITE_LOG(LOG_FATAL, "Find no alive session channelId:%u", hChannel->channelId);
        return ERR_SESSION_NOFOUND;
    }
    bool isClosing = uv_is_closing((const uv_handle_t *)&hChannel->hWorkTCP);
    if (!isClosing && (hChannel->fdChildWorkTCP = Base::DuplicateUvSocket(&hChannel->hWorkTCP)) < 0) {
        WRITE_LOG(LOG_FATAL, "Duplicate socket failed channelId:%u", hChannel->channelId);
        return ERR_SOCKET_DUPLICATE;
    }

    uv_close_cb funcWorkTcpClose = [](uv_handle_t *handle) -> void {
        HChannel hChannel = (HChannel)handle->data;
        --hChannel->ref;
    };
    ++hChannel->ref;
    if (!isClosing) {
        uv_close((uv_handle_t *)&hChannel->hWorkTCP, funcWorkTcpClose);
    }
    Base::DoNextLoop(loopMain, hChannel, [](const uint8_t flag, string &msg, const void *data) {
        // Thread message can avoid using thread lock and improve program efficiency
        // If not next loop call, ReadStream will thread conflict
        HChannel hChannel = (HChannel)data;
        auto thisClass = (HdcServerForClient *)hChannel->clsChannel;
        HSession hSession = nullptr;
        if ((hSession = thisClass->FindAliveSessionFromDaemonMap(hChannel)) == nullptr) {
            WRITE_LOG(LOG_FATAL, "hSession nullptr channelId:%u", hChannel->channelId);
            return;
        }
        auto ctrl = HdcSessionBase::BuildCtrlString(SP_ATTACH_CHANNEL, hChannel->channelId, nullptr, 0);
        Base::SendToPollFd(hSession->ctrlFd[STREAM_MAIN], ctrl.data(), ctrl.size());
    });
    return RET_SUCCESS;
}
#endif

bool HdcServerForClient::CheckAutoFillTarget(HChannel hChannel)
{
    HdcServer *ptrServer = (HdcServer *)clsServer;
    if (!hChannel->connectKey.size()) {
        WRITE_LOG(LOG_FATAL, "connectKey.size 0 channelId:%u", hChannel->channelId);
        return false;  // Operation of non-bound destination of scanning
    }
    if (hChannel->connectKey == CMDSTR_CONNECT_ANY) {
        HDaemonInfo hdiOld = nullptr;
        ptrServer->AdminDaemonMap(OP_GET_ONLY, "", hdiOld);
        if (!hdiOld) {
            WRITE_LOG(LOG_WARN, "No any key found channelId:%u", hChannel->channelId);
            return false;
        }
        if (!hdiOld->hSession) {
            WRITE_LOG(LOG_WARN, "hSession is null. channelId:%u", hChannel->channelId);
            return false;
        }
        if (!hdiOld->hSession->handshakeOK) {
            WRITE_LOG(LOG_WARN, "hSession handShake is false SessionId:%u", hdiOld->hSession->sessionId);
            return false;
        }
        hChannel->connectKey = hdiOld->connectKey;
        return true;
    }
    return true;
}

int HdcServerForClient::ChannelHandShake(HChannel hChannel, uint8_t *bufPtr, const int bytesIO)
{
    StartTraceScope("HdcServerForClient::ChannelHandShake");
    vector<uint8_t> rebuildHandshake;
    rebuildHandshake.insert(rebuildHandshake.end(), bufPtr, bufPtr + bytesIO);
    rebuildHandshake.push_back(0x00);
    struct ChannelHandShake *handShake = reinterpret_cast<struct ChannelHandShake *>(rebuildHandshake.data());
    if (strncmp(handShake->banner, HANDSHAKE_MESSAGE.c_str(), HANDSHAKE_MESSAGE.size())) {
        hChannel->availTailIndex = 0;
        WRITE_LOG(LOG_DEBUG, "Channel Hello failed");
        return ERR_HANDSHAKE_NOTMATCH;
    }
    if (strlen(handShake->connectKey) > sizeof(handShake->connectKey)) {
        hChannel->availTailIndex = 0;
        WRITE_LOG(LOG_DEBUG, "Connectkey's size incorrect");
        return ERR_HANDSHAKE_CONNECTKEY_FAILED;
    }
    // channel handshake step3
    WRITE_LOG(LOG_DEBUG, "ServerForClient cid:%u sid:%u handshake finished",
        hChannel->channelId, hChannel->targetSessionId);
    hChannel->connectKey = handShake->connectKey;
    hChannel->handshakeOK = true;
    if (handShake->banner[WAIT_TAG_OFFSET] == WAIT_DEVICE_TAG || !CheckAutoFillTarget(hChannel)) {
        WRITE_LOG(LOG_WARN, "No target channelId:%u", hChannel->channelId);
        return 0;
    }
    // channel handshake stBindChannelToSession
    if (BindChannelToSession(hChannel, nullptr, 0)) {
        hChannel->availTailIndex = 0;
        WRITE_LOG(LOG_FATAL, "BindChannelToSession failed channelId:%u sid:%u",
            hChannel->channelId, hChannel->targetSessionId);
        return ERR_GENERIC;
    }
    return 0;
}

void HdcServerForClient::ReportServerVersion(HChannel hChannel)
{
    string version = Base::GetVersion();
    SendChannelWithCmd(hChannel, CMD_CHECK_SERVER,
                       const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(version.c_str())),
                       version.size());
}

// Here is Server to get data, the source is the SERVER's ChildWork to send data
int HdcServerForClient::ReadChannel(HChannel hChannel, uint8_t *bufPtr, const int bytesIO)
{
    StartTraceScope("HdcServerForClient::ReadChannel");
    if (!hChannel->handshakeOK) {
        return ChannelHandShake(hChannel, bufPtr, bytesIO);
    }
    HDaemonInfo hdi = nullptr;
    HdcServer *ptrServer = (HdcServer *)clsServer;
    ptrServer->AdminDaemonMap(OP_QUERY, hChannel->connectKey, hdi);
    if (hdi && !hdi->emgmsg.empty()) {
        EchoClient(hChannel, MSG_FAIL, hdi->emgmsg.c_str());
        return ERR_GENERIC;
    }
    uint16_t command = *reinterpret_cast<uint16_t *>(bufPtr);
    if (command != 0 && (hChannel->remote > RemoteType::REMOTE_NONE)) {
        // server directly passthrough file command to daemon
        if (!SendToDaemon(hChannel, command, bufPtr + sizeof(uint16_t), bytesIO - sizeof(uint16_t))) {
            WRITE_LOG(LOG_FATAL, "Client ReadChannel : direct send to daemon failed");
        }
        return 0;
    }
    struct TranslateCommand::FormatCommand formatCommand = {};
    if (!hChannel->interactiveShellMode) {
        string retEcho = String2FormatCommand(reinterpret_cast<char *>(bufPtr), bytesIO, &formatCommand);
        if (retEcho.length()) {
            if (!strncmp(reinterpret_cast<char *>(bufPtr), CMDSTR_SOFTWARE_HELP.c_str(),
                CMDSTR_SOFTWARE_HELP.size()) ||
                !strcmp(reinterpret_cast<char *>(bufPtr), CMDSTR_SOFTWARE_VERSION.c_str()) ||
                !strcmp(reinterpret_cast<char *>(bufPtr), "flash")) {
                EchoClient(hChannel, MSG_OK, retEcho.c_str());
            } else {
                EchoClient(hChannel, MSG_FAIL, retEcho.c_str());
            }
        }

        WRITE_LOG(LOG_INFO, "ReadChannel cid:%u sid:%u key:%s", hChannel->channelId,
            hChannel->targetSessionId, Hdc::MaskString(hChannel->connectKey).c_str());

        if (Hdc::Base::GetCmdLogSwitch()) {
#ifdef SUPPORT_DETAILE_HDC_CMD_LOG
            string logBuf = Base::CmdLogStringFormat(hChannel->targetSessionId, (reinterpret_cast<char *>(bufPtr)));
#else
            std::string cmdStr = std::to_string(command);
            string logBuf = Base::CmdLogStringFormat(hChannel->targetSessionId, cmdStr.c_str());
#endif
            if (logBuf.length() > 0) {
                ptrServer->PrintCmdLogEx(logBuf);
            }
        }
        if (formatCommand.bJumpDo) {
            WRITE_LOG(LOG_FATAL, "ReadChannel bJumpDo true");
            return -10;  //  -10 error formatCommand
        }
    } else {
        formatCommand.parameters = string(reinterpret_cast<char *>(bufPtr), bytesIO);
        formatCommand.cmdFlag = CMD_SHELL_DATA;
    }
    if (!DoCommand(hChannel, &formatCommand, hdi)) {
        return -3;  // -3: error or want close
    }
    return bytesIO;
};

// avoid session dead
HSession HdcServerForClient::FindAliveSession(uint32_t sessionId)
{
    StartTraceScope("HdcServerForClient::FindAliveSession");
    HdcServer *ptrServer = (HdcServer *)clsServer;
    HSession hSession = ptrServer->AdminSession(OP_QUERY, sessionId, nullptr);
    if (!hSession || hSession->isDead) {
        WRITE_LOG(LOG_FATAL, "FindAliveSession hSession nullptr or isDead sessionId:%u", sessionId);
        return nullptr;
    } else {
        return hSession;
    }
}

bool HdcServerForClient::ChannelSendSessionCtrlMsg(vector<uint8_t> &ctrlMsg, uint32_t sessionId)
{
    HSession hSession = FindAliveSession(sessionId);
    if (!hSession) {
        WRITE_LOG(LOG_FATAL, "ChannelSendSessionCtrlMsg hSession nullptr sessionId:%u", sessionId);
        return false;
    }
    int rc = Base::SendToPollFd(hSession->ctrlFd[STREAM_MAIN], ctrlMsg.data(), ctrlMsg.size());
    if (rc <= 0) {
        WRITE_LOG(LOG_FATAL, "send ctrlmsg failed sessionId:%u rc:%d", sessionId, rc);
    }
    return rc > 0;
}

void HdcServerForClient::PrintLastError(HChannel HChannel)
{
    HdcServer *ptrServer = (HdcServer *)clsServer;
    uint32_t errorCode = ptrServer->lastErrorNum;
    if (errorCode > 0) {
        string errorString = GetErrorString(errorCode);
        EchoClient(HChannel, MSG_FAIL, "[E%06x]%s", errorCode, errorString.c_str());
        ptrServer->lastErrorNum = 0;
    }
}

string HdcServerForClient::GetErrorString(uint32_t errorCode)
{
    auto map = ErrorStringEnglish.find(errorCode);
    if (map != ErrorStringEnglish.end()) {
        return map->second;
    }
    return ErrorStringEnglish.at(0xFFFFFF); // 0xFFFFFF:  Unknown error
}

bool HdcServerForClient::CommandMatchDaemonFeature(uint16_t cmdFlag, const HDaemonInfo &hdi)
{
    StartTraceScope("HdcServerForClient::CommandMatchDaemonFeature");
    string cmdFlagStr = std::to_string(cmdFlag);
    if (FEATURE_CHECK_SET.find(cmdFlag) == FEATURE_CHECK_SET.end()) { // not need check
        return true;
    }
    auto tagMatch = hdi->daemonFeature.find(cmdFlagStr);
    if (tagMatch == hdi->daemonFeature.end()) { // unsupport command
        return false;
    }
    return (tagMatch->second == STR_FEATURE_ENABLE);
}
}  // namespace Hdc
