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
#ifndef HDC_JDWP_H
#define HDC_JDWP_H
#include "daemon_common.h"
#include <poll.h>
#include <unordered_map>
namespace Hdc {
class HdcJdwp {
public:
    HdcJdwp(uv_loop_t *loopIn, LoopStatus *loopStatus);
    virtual ~HdcJdwp();
    int Initial();
    void Stop();
    bool CreateJdwpTracker(HTaskInfo taskInfo);
    void RemoveJdwpTracker(HTaskInfo taskInfo);
    bool ReadyForRelease();
    string GetProcessList();
    bool SendJdwpNewFD(uint32_t targetPID, int fd);
    bool SendArkNewFD(const std::string str, int fd);
    bool CheckPIDExist(uint32_t targetPID);
    bool SendFdToApp(int sockfd, uint8_t *buf, int size, int fd);

#ifdef FUZZ_TEST
public:
#else
private:
#endif
#ifdef JS_JDWP_CONNECT
    static constexpr uint8_t JS_PKG_MIN_SIZE = 7;
    static constexpr uint16_t JS_PKG_MAX_SIZE = 512;
    struct JsMsgHeader {
        uint32_t msgLen;
        uint32_t pid;
        uint8_t isDebug; // 1:debugApp 0:releaseApp
    };
    string GetProcessListExtendPkgName(uint8_t dr);
#endif // JS_JDWP_CONNECT
    struct _PollFd {
        int fd;
        short events;
        short revents;
    };
    struct PollNode {
        _PollFd pollfd;
        uint32_t ppid;
        PollNode(int fd, uint32_t pid)
        {
            pollfd = {};
            pollfd.fd = fd;
            pollfd.events = POLLNVAL | POLLRDHUP | POLLHUP | POLLERR;
            pollfd.revents = 0;
            ppid = pid;
        }
    };
    struct ContextJdwp {
        uint32_t pid;
        uv_pipe_t pipe;
        HdcJdwp *thisClass;
        bool finish;
#ifdef JS_JDWP_CONNECT
        char buf[JS_PKG_MAX_SIZE + sizeof(JsMsgHeader)];
        string pkgName;
#else
        char buf[sizeof(uint32_t)];
#endif  // JS_JDWP_CONNECT
        uint8_t dummy;
        uv_tcp_t jvmTCP;
        uint8_t isDebug;
    };
    using HCtxJdwp = struct ContextJdwp *;

    bool JdwpListen();
    static int UvPipeBind(uv_pipe_t* handle, const char* name, size_t size);
    static void AcceptClient(uv_stream_t *server, int status);
    static void ReadStream(uv_stream_t *pipe, ssize_t nread, const uv_buf_t *buf);
    static void SendCallbackJdwpNewFD(uv_write_t *req, int status);
    static void *FdEventPollThread(void *args);
    void RemoveFdFromPollList(uint32_t pid);
    size_t JdwpProcessListMsg(char *buffer, size_t bufferlen, uint8_t dr);
    void *MallocContext();
    void FreeContext(HCtxJdwp ctx);
    void *AdminContext(const uint8_t op, const uint32_t pid, HCtxJdwp ctxJdwp);
    int CreateFdEventPoll();
    void ProcessListUpdated(HTaskInfo task = nullptr);
    void SendProcessList(HTaskInfo t, string data);
    void DrainAwakenPollThread() const;
    void WakePollThread();
    uv_loop_t *loop;
    LoopStatus *loopStatus;
    uv_pipe_t listenPipe = {};
    uint32_t refCount;
    int32_t awakenPollFd;
    map<uint32_t, HCtxJdwp> mapCtxJdwp;
    uv_rwlock_t lockMapContext;
    uv_rwlock_t lockJdwpTrack;
    std::unordered_map<int, PollNode> pollNodeMap;  // fd, PollNode
    std::vector<HTaskInfo> jdwpTrackers;
    bool stop;
    std::mutex freeContextMutex;
};
} // namespace Hdc
#endif
