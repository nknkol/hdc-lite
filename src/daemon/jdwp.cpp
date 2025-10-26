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
#include "jdwp.h"
#include <sys/eventfd.h>
#include <thread>
#include "system_depend.h"

namespace Hdc {
HdcJdwp::HdcJdwp(uv_loop_t *loopIn, LoopStatus *ls) : loopStatus(ls)
{
    listenPipe.data = this;
    loop = loopIn;
    loopStatus = ls;
    refCount = 0;
    uv_rwlock_init(&lockMapContext);
    uv_rwlock_init(&lockJdwpTrack);
    awakenPollFd = -1;
    stop = false;
}

HdcJdwp::~HdcJdwp()
{
    Base::CloseFd(awakenPollFd);
    uv_rwlock_destroy(&lockMapContext);
    uv_rwlock_destroy(&lockJdwpTrack);
}

bool HdcJdwp::ReadyForRelease()
{
    return refCount == 0;
}

void HdcJdwp::Stop()
{
    stop = true;
    WakePollThread();
    auto funcListenPipeClose = [](uv_handle_t *handle) -> void {
        HdcJdwp *thisClass = (HdcJdwp *)handle->data;
        --thisClass->refCount;
    };
    Base::TryCloseHandle((const uv_handle_t *)&listenPipe, funcListenPipeClose);
    freeContextMutex.lock();
    for (auto &&obj : mapCtxJdwp) {
        HCtxJdwp v = obj.second;
        FreeContext(v);
    }
    AdminContext(OP_CLEAR, 0, nullptr);
    freeContextMutex.unlock();
}

void *HdcJdwp::MallocContext()
{
    HCtxJdwp ctx = nullptr;
    if ((ctx = new ContextJdwp()) == nullptr) {
        return nullptr;
    }
    ctx->isDebug = 0;
    ctx->thisClass = this;
    ctx->pipe.data = ctx;
    ++refCount;
    return ctx;
}

// Single thread, two parameters can be used
void HdcJdwp::FreeContext(HCtxJdwp ctx)
{
    if (ctx->finish) {
        return;
    }
    ctx->finish = true;
    WRITE_LOG(LOG_INFO, "FreeContext for targetPID :%d", ctx->pid);
    if (!stop) {
        AdminContext(OP_REMOVE, ctx->pid, nullptr);
    }
    auto funcReqClose = [](uv_idle_t *handle) -> void {
        HCtxJdwp ctxIn = (HCtxJdwp)handle->data;
        --ctxIn->thisClass->refCount;
        Base::TryCloseHandle((uv_handle_t *)handle, Base::CloseIdleCallback);

        Base::TryCloseHandle((const uv_handle_t *)&ctxIn->pipe, [](uv_handle_t *handle) {
#ifndef HDC_EMULATOR
            HCtxJdwp ctxIn = (HCtxJdwp)handle->data;
            if (ctxIn != nullptr) {
                delete ctxIn;
                ctxIn = nullptr;
            }
#endif
        });
    };
    Base::IdleUvTask(loop, ctx, funcReqClose);
}

void HdcJdwp::RemoveFdFromPollList(uint32_t pid)
{
    for (auto &&pair : pollNodeMap) {
        if (pair.second.ppid == pid) {
            WRITE_LOG(LOG_INFO, "RemoveFdFromPollList for pid:%d.", pid);
            pollNodeMap.erase(pair.second.pollfd.fd);
            break;
        }
    }
}

void HdcJdwp::ReadStream(uv_stream_t *pipe, ssize_t nread, const uv_buf_t *buf)
{
    HCtxJdwp ctxJdwp = static_cast<HCtxJdwp>(pipe->data);
    HdcJdwp *thisClass = static_cast<HdcJdwp *>(ctxJdwp->thisClass);
    CALLSTAT_GUARD(*(thisClass->loopStatus), pipe->loop, "HdcJdwp::ReadStream");
    static std::once_flag firstLog;
    std::call_once(firstLog, [&]() { SystemDepend::SetDevItem("persist.hdc.jdwp", "0"); });
    
    bool ret = true;
    if (nread == UV_ENOBUFS) {  // It is definite enough, usually only 4 bytes
        ret = false;
        WRITE_LOG(LOG_DEBUG, "HdcJdwp::ReadStream IOBuf max");
    } else if (nread == 0) {
        return;
#ifdef JS_JDWP_CONNECT
    } else if (nread < signed(JS_PKG_MIN_SIZE + sizeof(JsMsgHeader)) ||
               nread > signed(JS_PKG_MAX_SIZE + sizeof(JsMsgHeader))) {
#else
    } else if (nread < 0 || nread != 4) {  // 4 : 4 bytes
#endif  // JS_JDWP_CONNECT
        ret = false;
        WRITE_LOG(LOG_DEBUG, "HdcJdwp::ReadStream invalid package nread:%d.", nread);
    }

    if (ret) {
        uint32_t pid = 0;
        char *p = ctxJdwp->buf;
        if (nread == sizeof(uint32_t)) {  // Java: pid
            pid = atoi(p);
        } else {  // JS:pid PkgName
#ifdef JS_JDWP_CONNECT
            // pid isDebug pkgName/processName
            struct JsMsgHeader *jsMsg = reinterpret_cast<struct JsMsgHeader *>(p);
            if (jsMsg->msgLen == static_cast<uint32_t>(nread)) {
                pid = jsMsg->pid;
                string pkgName = string((char *)p + sizeof(JsMsgHeader), jsMsg->msgLen - sizeof(JsMsgHeader));
                ctxJdwp->pkgName = pkgName;
                ctxJdwp->isDebug = jsMsg->isDebug;
            } else {
                ret = false;
                WRITE_LOG(LOG_DEBUG, "HdcJdwp::ReadStream invalid js package size %d:%d.", jsMsg->msgLen, nread);
            }
#endif  // JS_JDWP_CONNECT
        }
        if (pid > 0) {
            ctxJdwp->pid = pid;
#ifdef JS_JDWP_CONNECT
            WRITE_LOG(LOG_DEBUG, "JDWP accept pid:%d-pkg:%s isDebug:%d",
                pid, ctxJdwp->pkgName.c_str(), ctxJdwp->isDebug);
#else
            WRITE_LOG(LOG_DEBUG, "JDWP accept pid:%d", pid);
#endif  // JS_JDWP_CONNECT
            thisClass->AdminContext(OP_ADD, pid, ctxJdwp);
            ret = true;
            int fd = -1;
            if (uv_fileno(reinterpret_cast<uv_handle_t *>(&(ctxJdwp->pipe)), &fd) < 0) {
                WRITE_LOG(LOG_DEBUG, "HdcJdwp::ReadStream uv_fileno fail.");
            } else {
                thisClass->freeContextMutex.lock();
                thisClass->pollNodeMap.emplace(fd, PollNode(fd, pid));
                thisClass->freeContextMutex.unlock();
                thisClass->WakePollThread();
            }
        }
    }
    (void)memset_s(ctxJdwp->buf, sizeof(ctxJdwp->buf), 0, sizeof(ctxJdwp->buf));
    if (!ret) {
        WRITE_LOG(LOG_INFO, "ReadStream proc:%d err, free it.", ctxJdwp->pid);
        thisClass->freeContextMutex.lock();
        thisClass->FreeContext(ctxJdwp);
        thisClass->freeContextMutex.unlock();
    }
}

#ifdef JS_JDWP_CONNECT
string HdcJdwp::GetProcessListExtendPkgName(uint8_t dr)
{
    constexpr uint8_t releaseApp = 2;
    constexpr uint8_t allAppWithDr = 3;
    string ret;
    uv_rwlock_rdlock(&lockMapContext);
    for (auto &&v : mapCtxJdwp) {
        HCtxJdwp hj = v.second;
        if (dr == 0) {
            // allApp
            ret += std::to_string(v.first) + " " + hj->pkgName + "\n";
        } else if (dr == 1) {
            // debugApp
            if (hj->isDebug) {
                ret += std::to_string(v.first) + " " + hj->pkgName + "\n";
            }
        } else if (dr == releaseApp) {
            // releaseApp
            if (!hj->isDebug) {
                ret += std::to_string(v.first) + " " + hj->pkgName + "\n";
            }
        } else if (dr == allAppWithDr) {
            // allApp with display debug or release
            string apptype = "release";
            if (hj->isDebug) {
                apptype = "debug";
            }
            ret += std::to_string(v.first) + " " + hj->pkgName + " " + apptype + "\n";
        }
    }
    uv_rwlock_rdunlock(&lockMapContext);
    return ret;
}
#endif  // JS_JDWP_CONNECT

void HdcJdwp::AcceptClient(uv_stream_t *server, int status)
{
    uv_pipe_t *listenPipe = (uv_pipe_t *)server;
    HdcJdwp *thisClass = (HdcJdwp *)listenPipe->data;
    CALLSTAT_GUARD(*(thisClass->loopStatus), server->loop, "HdcJdwp::AcceptClient");
    HCtxJdwp ctxJdwp = (HCtxJdwp)thisClass->MallocContext();
    if (!ctxJdwp) {
        return;
    }
    uv_pipe_init(thisClass->loop, &ctxJdwp->pipe, 1);
    if (uv_accept(server, (uv_stream_t *)&ctxJdwp->pipe) < 0) {
        WRITE_LOG(LOG_DEBUG, "uv_accept failed");
        thisClass->freeContextMutex.lock();
        thisClass->FreeContext(ctxJdwp);
        thisClass->freeContextMutex.unlock();
        return;
    }
    auto funAlloc = [](uv_handle_t *handle, size_t sizeSuggested, uv_buf_t *buf) -> void {
        HCtxJdwp ctxJdwp = (HCtxJdwp)handle->data;
        buf->base = (char *)ctxJdwp->buf;
        buf->len = sizeof(ctxJdwp->buf);
    };
    uv_read_start((uv_stream_t *)&ctxJdwp->pipe, funAlloc, ReadStream);
}

// Test bash connnet(UNIX-domain sockets):nc -U path/ohjpid-control < hexpid.file
// Test uv connect(pipe): 'uv_pipe_connect'
bool HdcJdwp::JdwpListen()
{
#ifdef HDC_PCDEBUG
    // if test, can be enabled
    return true;
    const char jdwpCtrlName[] = { 'o', 'h', 'j', 'p', 'i', 'd', '-', 'c', 'o', 'n', 't', 'r', 'o', 'l', 0 };
    unlink(jdwpCtrlName);
#else
    const char jdwpCtrlName[] = { '\0', 'o', 'h', 'j', 'p', 'i', 'd', '-', 'c', 'o', 'n', 't', 'r', 'o', 'l', 0 };
#endif
    const int DEFAULT_BACKLOG = 4;
    bool ret = false;
    while (true) {
        uv_pipe_init(loop, &listenPipe, 0);
        listenPipe.data = this;
        if (UvPipeBind(&listenPipe, jdwpCtrlName, sizeof(jdwpCtrlName))) {
            WRITE_LOG(LOG_FATAL, "UvPipeBind failed");
            return ret;
        }
        if (uv_listen((uv_stream_t *)&listenPipe, DEFAULT_BACKLOG, AcceptClient)) {
            WRITE_LOG(LOG_FATAL, "uv_listen failed");
            break;
        }
        ++refCount;
        ret = true;
        break;
    }
    // listenPipe close by stop
    return ret;
}

int HdcJdwp::UvPipeBind(uv_pipe_t* handle, const char* name, size_t size)
{
    char buffer[BUF_SIZE_DEFAULT] = { 0 };

    if (handle->io_watcher.fd >= 0) {
        WRITE_LOG(LOG_FATAL, "socket already bound %d", handle->io_watcher.fd);
        return -1;
    }

    int type = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
    int sockfd = socket(AF_UNIX, type, 0);
    if (sockfd < 0) {
        strerror_r(errno, buffer, BUF_SIZE_DEFAULT);
        WRITE_LOG(LOG_FATAL, "socket failed errno:%d %s", errno, buffer);
        return -1;
    }

#if defined(SO_NOSIGPIPE)
    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    struct sockaddr_un saddr;
    memset_s(&saddr, sizeof(sockaddr_un), 0, sizeof(sockaddr_un));
    size_t capacity = sizeof(saddr.sun_path);
    size_t min = size < capacity ? size : capacity;
    for (size_t i = 0; i < min; i++) {
        saddr.sun_path[i] = name[i];
    }
    saddr.sun_path[capacity - 1] = '\0';
    saddr.sun_family = AF_UNIX;
    size_t saddrLen = sizeof(saddr.sun_family) + size - 1;
    int err = bind(sockfd, reinterpret_cast<struct sockaddr*>(&saddr), saddrLen);
    if (err != 0) {
        strerror_r(errno, buffer, BUF_SIZE_DEFAULT);
        WRITE_LOG(LOG_FATAL, "bind failed errno:%d %s", errno, buffer);
        close(sockfd);
        return -1;
    }
    constexpr uint32_t uvHandleBound = 0x00002000;
    handle->flags |= uvHandleBound;
    handle->io_watcher.fd = sockfd;
    return 0;
}

// Working in the main thread, but will be accessed by each session thread, so we need to set thread lock
void *HdcJdwp::AdminContext(const uint8_t op, const uint32_t pid, HCtxJdwp ctxJdwp)
{
    HCtxJdwp hRet = nullptr;
    switch (op) {
        case OP_ADD: {
            uv_rwlock_wrlock(&lockMapContext);
            const int maxMapSize = 1024;
            if (mapCtxJdwp.size() < maxMapSize) {
                mapCtxJdwp[pid] = ctxJdwp;
            }
            uv_rwlock_wrunlock(&lockMapContext);
            break;
        }
        case OP_REMOVE:
            uv_rwlock_wrlock(&lockMapContext);
            mapCtxJdwp.erase(pid);
            RemoveFdFromPollList(pid);
            uv_rwlock_wrunlock(&lockMapContext);
            break;
        case OP_QUERY: {
            uv_rwlock_rdlock(&lockMapContext);
            if (mapCtxJdwp.count(pid)) {
                hRet = mapCtxJdwp[pid];
            }
            uv_rwlock_rdunlock(&lockMapContext);
            break;
        }
        case OP_CLEAR: {
            uv_rwlock_wrlock(&lockMapContext);
            mapCtxJdwp.clear();
            pollNodeMap.clear();
            uv_rwlock_wrunlock(&lockMapContext);
            break;
        }
        default:
            break;
    }
    if (op == OP_ADD || op == OP_REMOVE || op == OP_CLEAR) {
        uv_rwlock_wrlock(&lockJdwpTrack);
        ProcessListUpdated();
        uv_rwlock_wrunlock(&lockJdwpTrack);
    }
    return hRet;
}

// work on main thread
void HdcJdwp::SendCallbackJdwpNewFD(uv_write_t *req, int status)
{
    // It usually works successful, not notify session work
    HCtxJdwp ctx = (HCtxJdwp)req->data;
    if (status >= 0) {
        WRITE_LOG(LOG_DEBUG, "SendCallbackJdwpNewFD successful %d, active jdwp forward", ctx->pid);
    } else {
        WRITE_LOG(LOG_WARN, "SendCallbackJdwpNewFD failed %d", ctx->pid);
    }
    delete req;
}

// Each session calls the interface through the main thread message queue, which cannot be called directly across
// threads
// work on main thread
bool HdcJdwp::SendJdwpNewFD(uint32_t targetPID, int fd)
{
    bool ret = false;
    while (true) {
        HCtxJdwp ctx = (HCtxJdwp)AdminContext(OP_QUERY, targetPID, nullptr);
        if (!ctx) {
            break;
        }
        ctx->dummy = static_cast<uint8_t>('!');
        if (uv_tcp_init(loop, &ctx->jvmTCP)) {
            break;
        }
        if (uv_tcp_open(&ctx->jvmTCP, fd)) {
            break;
        }
        // transfer fd to jvm
        // clang-format off
        if (Base::SendToStreamEx((uv_stream_t *)&ctx->pipe, (uint8_t *)&ctx->dummy, 1, (uv_stream_t *)&ctx->jvmTCP,
            (void *)SendCallbackJdwpNewFD, (const void *)ctx) < 0) {
            break;
        }
        // clang-format on
        ++refCount;
        ret = true;
        WRITE_LOG(LOG_DEBUG, "SendJdwpNewFD successful targetPID:%d fd%d", targetPID, fd);
        break;
    }
    return ret;
}

bool HdcJdwp::SendArkNewFD(const std::string str, int fd)
{
    bool ret = false;
    while (true) {
        // str(ark:pid@tid@Debugger)
        size_t pos = str.find_first_of(':');
        std::string right = str.substr(pos + 1);
        pos = right.find_first_of("@");
        std::string pidstr = right.substr(0, pos);
        uint32_t pid = static_cast<uint32_t>(std::atoi(pidstr.c_str()));
        HCtxJdwp ctx = (HCtxJdwp)AdminContext(OP_QUERY, pid, nullptr);
        if (!ctx) {
            WRITE_LOG(LOG_FATAL, "SendArkNewFD query pid:%u failed", pid);
            break;
        }
        uint32_t size = sizeof(int32_t) + str.size();
        // fd | str(ark:pid@tid@Debugger)
        uint8_t *buf = new(std::nothrow) uint8_t[size];
        if (buf == nullptr) {
            WRITE_LOG(LOG_FATAL, "out of memory size:%u", size);
            Base::CloseFd(fd);
            return false;
        }
        if (memcpy_s(buf, sizeof(int32_t), &fd, sizeof(int32_t)) != EOK) {
            WRITE_LOG(LOG_WARN, "From fd Create buf failed, fd:%d", fd);
            Base::CloseFd(fd);
            delete[] buf;
            return false;
        }
        if (memcpy_s(buf + sizeof(int32_t), str.size(), str.c_str(), str.size()) != EOK) {
            WRITE_LOG(LOG_WARN, "SendArkNewFD failed fd:%d str:%s", fd, str.c_str());
            Base::CloseFd(fd);
            delete[] buf;
            return false;
        }
        uv_stream_t *stream = (uv_stream_t *)&ctx->pipe;
        SendFdToApp(stream->io_watcher.fd, buf, size, fd);
        delete[] buf;
        ret = true;
        WRITE_LOG(LOG_DEBUG, "SendArkNewFD successful str:%s fd%d", str.c_str(), fd);
        Base::CloseFd(fd);
        break;
    }
    return ret;
}

bool HdcJdwp::SendFdToApp(int sockfd, uint8_t *buf, int size, int fd)
{
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = static_cast<unsigned int>(size);
    struct msghdr msg;
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    int len = CMSG_SPACE(static_cast<unsigned int>(sizeof(fd)));
    char ctlBuf[len];
    msg.msg_control = ctlBuf;
    msg.msg_controllen = sizeof(ctlBuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr) {
        WRITE_LOG(LOG_FATAL, "SendFdToApp cmsg is nullptr");
        return false;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    if (memcpy_s(CMSG_DATA(cmsg), sizeof(fd), &fd, sizeof(fd)) != 0) {
        WRITE_LOG(LOG_FATAL, "SendFdToApp memcpy error:%d", errno);
        return false;
    }
    if (sendmsg(sockfd, &msg, 0) < 0) {
        WRITE_LOG(LOG_FATAL, "SendFdToApp sendmsg errno:%d", errno);
        return false;
    }
    return true;
}

// cross thread call begin
bool HdcJdwp::CheckPIDExist(uint32_t targetPID)
{
    HCtxJdwp ctx = (HCtxJdwp)AdminContext(OP_QUERY, targetPID, nullptr);
    return ctx != nullptr;
}

string HdcJdwp::GetProcessList()
{
    string ret;
    uv_rwlock_rdlock(&lockMapContext);
    for (auto &&v : mapCtxJdwp) {
        ret += std::to_string(v.first) + "\n";
    }
    uv_rwlock_rdunlock(&lockMapContext);
    return ret;
}
// cross thread call finish

size_t HdcJdwp::JdwpProcessListMsg(char *buffer, size_t bufferlen, uint8_t dr)
{
    // Message is length-prefixed with 4 hex digits in ASCII.
    static constexpr size_t headerLen = 5;
    char head[headerLen + 2];
#ifdef JS_JDWP_CONNECT
    string result = GetProcessListExtendPkgName(dr);
#else
    string result = GetProcessList();
#endif // JS_JDWP_CONNECT

    size_t len = result.length();
    if (bufferlen < (len + headerLen)) {
        WRITE_LOG(LOG_WARN, "truncating JDWP process list (max len = %zu) ", bufferlen);
        len = bufferlen;
    }
    if (snprintf_s(head, sizeof head, sizeof head - 1, "%04zx\n", len) < 0) {
        WRITE_LOG(LOG_WARN, " JdwpProcessListMsg head fail.");
        return 0;
    }
    if (memcpy_s(buffer, bufferlen, head, headerLen) != EOK) {
        WRITE_LOG(LOG_WARN, " JdwpProcessListMsg get head fail.");
        return 0;
    }
    if (memcpy_s(buffer + headerLen, (bufferlen - headerLen), result.c_str(), len) != EOK) {
        WRITE_LOG(LOG_WARN, " JdwpProcessListMsg get data  fail.");
        return 0;
    }
    return len + headerLen;
}

void HdcJdwp::SendProcessList(HTaskInfo t, string data)
{
    if (t == nullptr || data.size() == 0) {
        WRITE_LOG(LOG_WARN, " SendProcessList, Nothing needs to be sent.");
        return;
    }
    void *clsSession = t->ownerSessionClass;
    HdcSessionBase *sessionBase = static_cast<HdcSessionBase *>(clsSession);
    sessionBase->LogMsg(t->sessionId, t->channelId, MSG_OK, data.c_str());
}

void HdcJdwp::ProcessListUpdated(HTaskInfo task)
{
    if (jdwpTrackers.size() <= 0) {
        return;
    }
#ifdef JS_JDWP_CONNECT
    static constexpr uint32_t jpidTrackListSize = 1024 * 4;
#else
    static constexpr uint32_t jpidTrackListSize = 1024;
#endif // JS_JDWP_CONNECT
    std::string data;
    if (task != nullptr) {
        data.resize(jpidTrackListSize);
        size_t len = JdwpProcessListMsg(&data[0], data.size(), task->debugRelease);
        if (len > 0) {
            data.resize(len);
            SendProcessList(task, data);
        }
        return;
    }
    for (auto iter = jdwpTrackers.begin(); iter != jdwpTrackers.end();) {
        if (*iter == nullptr) {
            continue;
        }
        // The channel for the track-jpid has been stopped.
        if ((*iter)->taskStop || (*iter)->taskFree || !(*iter)->taskClass) {
            iter = jdwpTrackers.erase(remove(jdwpTrackers.begin(), jdwpTrackers.end(), *iter), jdwpTrackers.end());
            if (jdwpTrackers.size() == 0) {
                return;
            }
        } else {
            data.resize(jpidTrackListSize);
            size_t len = JdwpProcessListMsg(&data[0], data.size(), (*iter)->debugRelease);
            if (len > 0) {
                data.resize(len);
                SendProcessList(*iter, data);
            }
            iter++;
        }
    }
}

bool HdcJdwp::CreateJdwpTracker(HTaskInfo taskInfo)
{
    if (taskInfo == nullptr) {
        return false;
    }
    uv_rwlock_wrlock(&lockJdwpTrack);
    auto it = std::find(jdwpTrackers.begin(), jdwpTrackers.end(), taskInfo);
    if (it == jdwpTrackers.end()) {
        jdwpTrackers.push_back(taskInfo);
    }
    ProcessListUpdated(taskInfo);
    uv_rwlock_wrunlock(&lockJdwpTrack);
    return true;
}

void HdcJdwp::RemoveJdwpTracker(HTaskInfo taskInfo)
{
    if (taskInfo == nullptr) {
        return;
    }
    uv_rwlock_wrlock(&lockJdwpTrack);
    auto it = std::find(jdwpTrackers.begin(), jdwpTrackers.end(), taskInfo);
    if (it != jdwpTrackers.end()) {
        WRITE_LOG(LOG_DEBUG, "RemoveJdwpTracker channelId:%d, taskType:%d.", taskInfo->channelId, taskInfo->taskType);
        jdwpTrackers.erase(remove(jdwpTrackers.begin(), jdwpTrackers.end(), *it), jdwpTrackers.end());
    }
    uv_rwlock_wrunlock(&lockJdwpTrack);
}

void HdcJdwp::DrainAwakenPollThread() const
{
    uint64_t value = 0;
    ssize_t retVal = read(awakenPollFd, &value, sizeof(value));
    if (retVal < 0) {
        WRITE_LOG(LOG_FATAL, "DrainAwakenPollThread: Failed to read data from awaken pipe %d", retVal);
    }
}

void HdcJdwp::WakePollThread()
{
    if (awakenPollFd < 0) {
        WRITE_LOG(LOG_DEBUG, "awakenPollFd: MUST initialized before notifying");
        return;
    }
    static const uint64_t increment = 1;
    ssize_t retVal = write(awakenPollFd, &increment, sizeof(increment));
    if (retVal < 0) {
        WRITE_LOG(LOG_FATAL, "WakePollThread: Failed to write data into awaken pipe %d", retVal);
    }
}

void *HdcJdwp::FdEventPollThread(void *args)
{
    auto thisClass = static_cast<HdcJdwp *>(args);
    std::vector<struct pollfd> pollfds;
    size_t size = 0;
    while (!thisClass->stop) {
        thisClass->freeContextMutex.lock();
        if (size != thisClass->pollNodeMap.size() || thisClass->pollNodeMap.size() == 0) {
            pollfds.clear();
            struct pollfd pollFd;
            for (const auto &pair : thisClass->pollNodeMap) {
                pollFd.fd = pair.second.pollfd.fd;
                pollFd.events = pair.second.pollfd.events;
                pollFd.revents = pair.second.pollfd.revents;
                pollfds.push_back(pollFd);
            }
            pollFd.fd = thisClass->awakenPollFd;
            pollFd.events = POLLIN;
            pollFd.revents = 0;
            pollfds.push_back(pollFd);
            size = pollfds.size();
        }
        thisClass->freeContextMutex.unlock();
        poll(&pollfds[0], size, -1);
        for (const auto &pollfdsing : pollfds) {
            // POLLNVAL:fd not open
            if ((static_cast<unsigned short>(pollfdsing.revents) & (POLLNVAL | POLLRDHUP | POLLHUP | POLLERR)) != 0) {
                thisClass->freeContextMutex.lock();
                auto it = thisClass->pollNodeMap.find(pollfdsing.fd);
                if (it != thisClass->pollNodeMap.end()) {
                    uint32_t targetPID = it->second.ppid;
                    HCtxJdwp ctx = static_cast<HCtxJdwp>(thisClass->AdminContext(OP_QUERY, targetPID, nullptr));
                    if (ctx != nullptr) {
                        thisClass->AdminContext(OP_REMOVE, targetPID, nullptr);
                    }
                }
                thisClass->freeContextMutex.unlock();
            } else if ((static_cast<unsigned short>(pollfdsing.revents) & POLLIN) != 0) {
                if (pollfdsing.fd == thisClass->awakenPollFd) {
                    thisClass->DrainAwakenPollThread();
                }
            }
        }
    }
    return nullptr;
}

int HdcJdwp::CreateFdEventPoll()
{
    pthread_t tid;
    Base::CloseFd(awakenPollFd);
    awakenPollFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (awakenPollFd < 0) {
        WRITE_LOG(LOG_FATAL, "CreateFdEventPoll : Failed to create awakenPollFd");
        return ERR_GENERIC;
    }
    int tret = pthread_create(&tid, nullptr, FdEventPollThread, this);
    if (tret != 0) {
        WRITE_LOG(LOG_INFO, "FdEventPollThread create fail.");
        return tret;
    }
    return RET_SUCCESS;
}

// jdb -connect com.sun.jdi.SocketAttach:hostname=localhost,port=8000
int HdcJdwp::Initial()
{
    freeContextMutex.lock();
    pollNodeMap.clear();
    freeContextMutex.unlock();
    if (!JdwpListen()) {
        WRITE_LOG(LOG_FATAL, "JdwpListen failed");
        return ERR_MODULE_JDWP_FAILED;
    }
    SystemDepend::SetDevItem("persist.hdc.jdwp", "0");
    SystemDepend::SetDevItem("persist.hdc.jdwp", "1");
    if (CreateFdEventPoll() < 0) {
        return ERR_MODULE_JDWP_FAILED;
    }
    return RET_SUCCESS;
}
}
