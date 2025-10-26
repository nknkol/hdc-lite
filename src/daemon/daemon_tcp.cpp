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
#include "daemon_tcp.h"
#include <cstdlib>
#include "arpa/inet.h"
#include "netinet/in.h"
#include "new"
#include "sys/socket.h"
#include "system_depend.h"
#include "unistd.h"
#include "common.h"
#include "session.h"

namespace Hdc {
HdcDaemonTCP::HdcDaemonTCP(const bool serverOrDaemonIn, void *ptrMainBase)
    : HdcTCPBase(serverOrDaemonIn, ptrMainBase)
{
    // If the listening value for the property setting is obtained, it will be 0 randomly assigned.
    string strTCPPort;
    SystemDepend::GetDevItem("persist.hdc.port", strTCPPort);
    tcpListenPort = atoi(strTCPPort.c_str());
    if (tcpListenPort <= 0) {
        WRITE_LOG(LOG_WARN, "persist.hdc.port is invalid, set default to 0");
        tcpListenPort = 0;
    }
}

HdcDaemonTCP::~HdcDaemonTCP()
{
}

void HdcDaemonTCP::Stop()
{
    Base::TryCloseHandle((const uv_handle_t *)&servUDP);
    Base::TryCloseHandle((const uv_handle_t *)&servTCP);
    WRITE_LOG(LOG_INFO, "Stop tcpListenPort:%u", tcpListenPort);
}

void HdcDaemonTCP::TransmitConfig(const sockaddr *addrSrc, uv_udp_t *handle)
{
    char srcIP[BUF_SIZE_TINY] = "";
    struct sockaddr addrSrcIPPort;
    uv_udp_send_t *req = new uv_udp_send_t();
    if (!req) {
        return;
    }
    string sendBuf = Base::StringFormat("%s-%d", HANDSHAKE_MESSAGE.c_str(), tcpListenPort);
    uv_buf_t sndbuf = uv_buf_init((char *)sendBuf.c_str(), sendBuf.size());
    uv_ip4_name(const_cast<sockaddr_in *>(reinterpret_cast<const sockaddr_in *>(addrSrc)), srcIP, sizeof(srcIP));
    uv_ip4_addr(srcIP, DEFAULT_PORT, const_cast<sockaddr_in *>(reinterpret_cast<const sockaddr_in *>(&addrSrcIPPort)));
    uv_udp_send(req, handle, &sndbuf, 1, &addrSrcIPPort, SendUDPFinish);
}

void HdcDaemonTCP::AcceptClient(uv_stream_t *server, int status)
{
    uv_loop_t *ptrLoop = server->loop;
    uv_tcp_t *pServTCP = (uv_tcp_t *)server;
    HdcDaemonTCP *thisClass = (HdcDaemonTCP *)pServTCP->data;
    HdcSessionBase *ptrConnect = reinterpret_cast<HdcSessionBase *>(thisClass->clsMainBase);
    HdcSessionBase *daemon = reinterpret_cast<HdcSessionBase *>(thisClass->clsMainBase);
    CALLSTAT_GUARD(daemon->loopMainStatus, server->loop, "HdcDaemonTCP::AcceptClient");
    const uint16_t maxWaitTime = UV_DEFAULT_INTERVAL;
    auto ctrl = daemon->BuildCtrlString(SP_START_SESSION, 0, nullptr, 0);
    HSession hSession = ptrConnect->MallocSession(false, CONN_TCP, thisClass);
    if (!hSession) {
        WRITE_LOG(LOG_FATAL, "malloc tcp session failed");
        return;
    }
    if (uv_accept(server, (uv_stream_t *)&hSession->hWorkTCP) < 0) {
        WRITE_LOG(LOG_FATAL, "uv_accept error sessionId:%u", hSession->sessionId);
        goto Finish;
    }
    if ((hSession->fdChildWorkTCP = Base::DuplicateUvSocket(&hSession->hWorkTCP)) < 0) {
        WRITE_LOG(LOG_FATAL, "AcceptClient error fdChildWorkTCP:%d,errno:%d", hSession->fdChildWorkTCP, errno);
        goto Finish;
    }
    Base::TryCloseHandle((uv_handle_t *)&hSession->hWorkTCP);
    Base::StartWorkThread(ptrLoop, ptrConnect->SessionWorkThread, Base::FinishWorkThread, hSession);
    // wait for thread up
    while (hSession->childLoop.active_handles == 0) {
        usleep(maxWaitTime);
    }
    Base::SendToPollFd(hSession->ctrlFd[STREAM_MAIN], ctrl.data(), ctrl.size());
    return;
Finish:
    ptrConnect->FreeSession(hSession->sessionId);
}

void HdcDaemonTCP::RecvUDPEntry(const sockaddr *addrSrc, uv_udp_t *handle, const uv_buf_t *rcvbuf)
{
    TransmitConfig(addrSrc, handle);
}

void HdcDaemonTCP::SetUDPListen()
{
    struct sockaddr_in addr;
    HdcSessionBase *ptrConnect = (HdcSessionBase *)clsMainBase;
    // udp broadcast
    servUDP.data = this;
    uv_udp_init(&ptrConnect->loopMain, &servUDP);
    uv_ip4_addr("0.0.0.0", DEFAULT_PORT, &addr);
    uv_udp_bind(&servUDP, (const struct sockaddr *)&addr, UV_UDP_REUSEADDR);
    uv_udp_recv_start(&servUDP, AllocStreamUDP, RecvUDP);
}

// Set the daemon-side TCP listening
int HdcDaemonTCP::SetTCPListen()
{
    // tcp listen
    HdcSessionBase *ptrConnect = (HdcSessionBase *)clsMainBase;
    servTCP.data = this;
    struct sockaddr_in addr = {};
    int namelen;
    const int DEFAULT_BACKLOG = 128;

    uv_tcp_init(&ptrConnect->loopMain, &servTCP);
    uv_ip4_addr("0.0.0.0", tcpListenPort, &addr);  // tcpListenPort == 0
    uv_tcp_bind(&servTCP, (const struct sockaddr *)&addr, 0);
    if (uv_listen((uv_stream_t *)&servTCP, DEFAULT_BACKLOG, (uv_connection_cb)AcceptClient)) {
        return ERR_API_FAIL;
    }
    // Get listen port
    (void)memset_s(&addr, sizeof(sockaddr_in), 0, sizeof(sockaddr_in));
    namelen = sizeof(addr);
    if (uv_tcp_getsockname(&servTCP, (sockaddr *)&addr, &namelen)) {
        return ERR_API_FAIL;
    }
    tcpListenPort = ntohs(addr.sin_port);
    SystemDepend::SetDevItem("persist.hdc.port", std::to_string(tcpListenPort).c_str());
    return RET_SUCCESS;
}

int HdcDaemonTCP::Initial()
{
    WRITE_LOG(LOG_INFO, "HdcDaemonTCP init");
    SetUDPListen();
    if (SetTCPListen() != RET_SUCCESS) {
        WRITE_LOG(LOG_FATAL, "TCP listen failed");
        return ERR_GENERIC;
    }
#ifndef UNIT_TEST
    WRITE_LOG(LOG_INFO, "TCP listen on port:[%d]", tcpListenPort);
#endif
    return RET_SUCCESS;
}
}  // namespace Hdc
