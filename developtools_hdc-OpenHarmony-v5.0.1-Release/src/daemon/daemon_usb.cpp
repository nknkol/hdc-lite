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
#include "daemon_usb.h"
#include <cerrno>
#include <cstddef>
#include "arpa/inet.h"
#include "asm-generic/int-ll64.h"
#include "fcntl.h"
#include "linux/usb/functionfs.h"
#include "new"
#include "sched.h"
#include "system_depend.h"
#include "unistd.h"
#include "uv/unix.h"
#include "daemon.h"
#include "usb_ffs.h"

namespace Hdc {
static constexpr int CONFIG_COUNT2 = 2;
static constexpr int CONFIG_COUNT3 = 3;
static constexpr int CONFIG_COUNT5 = 5;

struct UvData {
    HdcDaemonUSB *daemonUsb;
    const uint8_t *buf;
};

HdcDaemonUSB::HdcDaemonUSB(const bool serverOrDaemonIn, void *ptrMainBase)
    : HdcUSBBase(serverOrDaemonIn, ptrMainBase)
{
}

HdcDaemonUSB::~HdcDaemonUSB()
{
    // Closed in the IO loop, no longer closing CLOSE ENDPOINT
    Base::CloseFd(controlEp);
    if (ctxRecv.buf) {
        delete[] ctxRecv.buf;
    }
    uv_fs_req_cleanup(&ctxRecv.req);
}

void HdcDaemonUSB::Stop()
{
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB Stop");
    // Here only clean up the IO-related resources, session related resources clear reason to clean up the session
    // module
    modRunning = false;
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB Stop free main session");
    Base::TryCloseHandle((uv_handle_t *)&checkEP);
    CloseEndpoint(&usbHandle);
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB Stop free main session finish");
}

string HdcDaemonUSB::GetDevPath(const std::string &path)
{
    DIR *dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        WRITE_LOG(LOG_WARN, "%s: cannot open devpath: errno: %d", path.c_str(), errno);
        return "";
    }

    string res = USB_FFS_BASE;
    string node;
    int count = 0;
    struct dirent *entry = nullptr;
    while ((entry = ::readdir(dir))) {
        if (*entry->d_name == '.') {
            continue;
        }
        node = entry->d_name;
        ++count;
    }
    if (count > 1) {
        res += "hdc";
    } else {
        res += node;
    }
    ::closedir(dir);
    return res;
}

int HdcDaemonUSB::GetMaxPacketSize(int fdFfs)
{
    // no ioctl support, todo dynamic get
    return MAX_PACKET_SIZE_HISPEED;
}

int HdcDaemonUSB::Initial()
{
    // after Linux-3.8，kernel switch to the USB Function FS
    // Implement USB hdc function in user space
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB init");
    basePath = GetDevPath(USB_FFS_BASE);
    if (access((basePath + "/ep0").c_str(), F_OK) != 0) {
        WRITE_LOG(LOG_DEBUG,"Only support usb-ffs, make sure kernel3.8+ and usb-ffs enabled, "
                  "usbmode disabled: errno: %d, basePath: %s ", errno, basePath.c_str());
        return ERR_API_FAIL;
    }
    ctxRecv.thisClass = this;
    ctxRecv.bufSizeMax = Base::GetUsbffsBulkSize();
    ctxRecv.buf = new uint8_t[ctxRecv.bufSizeMax]();
    if (!ctxRecv.buf) {
        WRITE_LOG(LOG_FATAL, "Init alloc memory failed");
        return ERR_BUF_ALLOC;
    }

    HdcDaemon *daemon = (HdcDaemon *)clsMainBase;
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB::Initiall");
    uv_timer_init(&daemon->loopMain, &checkEP);
    checkEP.data = this;
    uv_timer_start(&checkEP, WatchEPTimer, 0, TIME_BASE);
    return 0;
}

// make gnuc++ happy. Clang support direct assignment value to structure, buf g++ weakness
void HdcDaemonUSB::FillUsbV2Head(UsbFunctionfsDescV2 &descUsbFfs)
{
    descUsbFfs.head.magic = LONG_LE(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    descUsbFfs.head.length = LONG_LE(sizeof(descUsbFfs));
    descUsbFfs.head.flags
        = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC | FUNCTIONFS_HAS_MS_OS_DESC;
    descUsbFfs.config1Count = CONFIG_COUNT3;
    descUsbFfs.config2Count = CONFIG_COUNT3;
    descUsbFfs.config3Count = CONFIG_COUNT5;
    descUsbFfs.configWosCount = CONFIG_COUNT2;
    descUsbFfs.config1Desc = config1;
    descUsbFfs.config2Desc = config2;
    descUsbFfs.config3Desc = config3;
    descUsbFfs.wosHead = g_wosHead;
    descUsbFfs.wosDesc = g_wosDesc;
    descUsbFfs.osPropHead = g_osPropHead;
    descUsbFfs.osPropValues = g_osPropValues;
}

// DAEMON end USB module USB-FFS EP port connection
int HdcDaemonUSB::ConnectEPPoint(HUSB hUSB)
{
    int ret = ERR_GENERIC;
    struct UsbFunctionfsDescV2 descUsbFfs = {};
    FillUsbV2Head(descUsbFfs);
    while (true) {
        if (controlEp <= 0) {
            // After the control port sends the instruction, the device is initialized by the device to the HOST host,
            // which can be found for USB devices. Do not send initialization to the EP0 control port, the USB
            // device will not be initialized by Host
            WRITE_LOG(LOG_DEBUG, "Begin send to control(EP0) for usb descriptor init");
            string ep0Path = basePath + "/ep0";
            if ((controlEp = open(ep0Path.c_str(), O_RDWR)) < 0) {
                WRITE_LOG(LOG_WARN, "%s: cannot open control endpoint: errno=%d", ep0Path.c_str(), errno);
                break;
            }
            if (write(controlEp, &descUsbFfs, sizeof(descUsbFfs)) < 0) {
                WRITE_LOG(LOG_WARN, "%s: write ffs configs failed: errno=%d", ep0Path.c_str(), errno);
                break;
            }
            if (write(controlEp, &USB_FFS_VALUE, sizeof(USB_FFS_VALUE)) < 0) {
                WRITE_LOG(LOG_WARN, "%s: write USB_FFS_VALUE failed: errno=%d", ep0Path.c_str(), errno);
                break;
            }
            // active usbrc，Send USB initialization signal
            SystemDepend::SetDevItem("sys.usb.ffs.ready.hdc", "0");
            SystemDepend::SetDevItem("sys.usb.ffs.ready", "1");
            SystemDepend::SetDevItem("sys.usb.ffs.ready.hdc", "1");
            WRITE_LOG(LOG_DEBUG, "ConnectEPPoint ctrl init finish, set usb-ffs ready");
        }
        string outPath = basePath + "/ep1";
        if ((hUSB->bulkOut = open(outPath.c_str(), O_RDWR)) < 0) {
            WRITE_LOG(LOG_WARN, "%s: cannot open bulk-out ep: errno=%d", outPath.c_str(), errno);
            break;
        }
        string inPath = basePath + "/ep2";
        if ((hUSB->bulkIn = open(inPath.c_str(), O_RDWR)) < 0) {
            WRITE_LOG(LOG_WARN, "%s: cannot open bulk-in ep: errno=%d", inPath.c_str(), errno);
            break;
        }
        // cannot open with O_CLOEXEC, must fcntl
        fcntl(controlEp, F_SETFD, FD_CLOEXEC);
        fcntl(hUSB->bulkOut, F_SETFD, FD_CLOEXEC);
        fcntl(hUSB->bulkIn, F_SETFD, FD_CLOEXEC);
        hUSB->wMaxPacketSizeSend = GetMaxPacketSize(hUSB->bulkIn);

        WRITE_LOG(LOG_DEBUG, "New bulk in\\out open bulkout:%d bulkin:%d", hUSB->bulkOut, hUSB->bulkIn);
        ret = RET_SUCCESS;
        break;
    }
    if (ret != RET_SUCCESS) {
        CloseEndpoint(hUSB, true);
    }
    return ret;
}

void HdcDaemonUSB::CloseEndpoint(HUSB hUSB, bool closeCtrlEp)
{
    Base::CloseFd(hUSB->bulkIn);
    Base::CloseFd(hUSB->bulkOut);
    if (controlEp > 0 && closeCtrlEp) {
        Base::CloseFd(controlEp);
        controlEp = 0;
    }
    isAlive = false;
    WRITE_LOG(LOG_FATAL, "DaemonUSB close endpoint");
}

void HdcDaemonUSB::ResetOldSession(uint32_t sessionId, bool isSoftReset)
{
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(clsMainBase);
    if (sessionId == 0) {
        sessionId = currentSessionId;
    }
    HSession hSession = daemon->AdminSession(OP_QUERY, sessionId, nullptr);
    if (hSession == nullptr) {
        WRITE_LOG(LOG_FATAL, "ResetOldSession hSession nullptr sessionId:%u", sessionId);
        return;
    }
    // The Host side is restarted, but the USB cable is still connected
    hSession->isSoftReset = isSoftReset;
    WRITE_LOG(LOG_WARN, "Hostside softreset to restart daemon, old sessionId:%u isSoftReset:%d",
        sessionId, isSoftReset);
    daemon->FreeSession(sessionId);
}

// Prevent other USB data misfortunes to send the program crash
int HdcDaemonUSB::AvailablePacket(uint8_t *ioBuf, int ioBytes, uint32_t *sessionId)
{
    int ret = RET_SUCCESS;
    while (true) {
        if (!IsUsbPacketHeader(ioBuf, ioBytes)) {
            break;
        }
        // usb header
        USBHead *usbPayloadHeader = reinterpret_cast<struct USBHead *>(ioBuf);
        uint32_t inSessionId = ntohl(usbPayloadHeader->sessionId);
        if ((usbPayloadHeader->option & USB_OPTION_RESET)) {
            WRITE_LOG(LOG_INFO, "USB_OPTION_RESET inSessionId:%u, currentSessionId:%u",
                inSessionId, currentSessionId);
            ResetOldSession(inSessionId, true);
            ret = ERR_IO_SOFT_RESET;
            break;
        }
        *sessionId = inSessionId;
        break;
    }
    return ret;
}

// Work in subcrete，Work thread is ready
bool HdcDaemonUSB::ReadyForWorkThread(HSession hSession)
{
    HdcUSBBase::ReadyForWorkThread(hSession);
    return true;
};

int HdcDaemonUSB::CloseBulkEp(bool bulkInOut, int bulkFd, uv_loop_t *loop)
{
    struct CtxCloseBulkEp {
        uv_fs_t req;
        HdcDaemonUSB *thisClass;
        bool bulkInOut;
    };
    CtxCloseBulkEp *ctx = new(std::nothrow) CtxCloseBulkEp();
    if (ctx == nullptr) {
        WRITE_LOG(LOG_FATAL, "CloseBulkEp new ctx failed");
        return -1;
    }
    uv_fs_t *req = &ctx->req;
    req->data = ctx;
    ctx->bulkInOut = bulkInOut;
    ctx->thisClass = this;
    isAlive = false;
    WRITE_LOG(LOG_DEBUG, "CloseBulkEp bulkFd:%d", bulkFd);
    uv_fs_close(loop, req, bulkFd, [](uv_fs_t *req) {
        auto ctx = (CtxCloseBulkEp *)req->data;
        WRITE_LOG(LOG_DEBUG, "Try to abort blukin write callback %s", ctx->bulkInOut ? "bulkin" : "bulkout");
        if (ctx->bulkInOut) {
            ctx->thisClass->usbHandle.bulkIn = 0;
        } else {
            ctx->thisClass->usbHandle.bulkOut = 0;
        }
        uv_fs_req_cleanup(req);
        delete ctx;
    });
    return 0;
}

int HdcDaemonUSB::SendUSBIOSync(HSession hSession, HUSB hMainUSB, const uint8_t *data, const int length)
{
    int bulkIn = hMainUSB->bulkIn;
    int childRet = 0;
    int ret = ERR_IO_FAIL;
    int offset = 0;
    StartTraceScope("HdcDaemonUSB::SendUSBIOSync");
    while (modRunning && isAlive && !hSession->isDead) {
        childRet = write(bulkIn, const_cast<uint8_t *>(data) + offset, length - offset);
        if (childRet <= 0) {
            int err = errno;
            if (err == EINTR) {
                WRITE_LOG(LOG_WARN, "BulkinWrite write EINTR, try again, offset:%u bulkIn:%d bulkOut:%d",
                    offset, bulkIn, hMainUSB->bulkOut);
                continue;
            } else {
                WRITE_LOG(LOG_FATAL, "BulkinWrite write fatal errno %d", err);
                isAlive = false;
            }
            break;
        }
        offset += childRet;
        if (offset >= length) {
            break;
        }
    }
    if (offset == length) {
        ret = length;
    } else {
        WRITE_LOG(LOG_FATAL, "BulkinWrite write failed, nsize:%d really:%d modRunning:%d isAlive:%d SessionDead:%d",
                  length, offset, modRunning, isAlive, hSession->isDead);
    }
    return ret;
}

int HdcDaemonUSB::SendUSBRaw(HSession hSession, uint8_t *data, const int length)
{
    StartTraceScope("HdcDaemonUSB::SendUSBRaw");
    HdcDaemon *daemon = (HdcDaemon *)hSession->classInstance;
    uint32_t sessionId = hSession->sessionId;
    std::unique_lock<std::mutex> lock(mutexUsbFfs);
    if (daemon->IsSessionDeleted(sessionId)) {
        WRITE_LOG(LOG_DEBUG, "SendUSBRaw session %u is deleted", sessionId);
        return ERR_SESSION_DEAD;
    }
    ++hSession->ref;
    int ret = SendUSBIOSync(hSession, &usbHandle, data, length);
    --hSession->ref;
    if (ret < 0) {
        daemon->FreeSession(hSession->sessionId);
        WRITE_LOG(LOG_DEBUG, "SendUSBRaw try to freesession");
    }
    return ret;
}

// cross thread call
void HdcDaemonUSB::OnNewHandshakeOK(const uint32_t sessionId)
{
    currentSessionId = sessionId;  // sync with server, and set server's real Id
}

// MainThreadCall, when seession was freed
void HdcDaemonUSB::OnSessionFreeFinally(const HSession hSession)
{
    WRITE_LOG(LOG_DEBUG, "OnSessionFreeFinally sid:%u currentsid:%u", hSession->sessionId, currentSessionId);
    if (hSession->isSoftReset) {
        WRITE_LOG(LOG_INFO, "OnSessionFreeFinally sid:%u softreset", hSession->sessionId);
        return;
    }
    if (currentSessionId == hSession->sessionId) {
        WRITE_LOG(LOG_DEBUG, "OnSessionFreeFinally set isAlive false");
        isAlive = false;
        // uv_cancel ctxRecv.req == UV_EBUSY, not effect immediately. It must be close by logic
    }
}

HSession HdcDaemonUSB::PrepareNewSession(uint32_t sessionId, uint8_t *pRecvBuf, int recvBytesIO)
{
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(clsMainBase);
    StartTraceScope("HdcDaemonUSB::PrepareNewSession");
    HSession hChildSession = daemon->MallocSession(false, CONN_USB, this, sessionId);
    if (!hChildSession) {
        WRITE_LOG(LOG_FATAL, "malloc session failed sessionId:%u", sessionId);
        return nullptr;
    }
    currentSessionId = sessionId;
    Base::StartWorkThread(&daemon->loopMain, daemon->SessionWorkThread, Base::FinishWorkThread, hChildSession);

    HSessionInfo hSessionInfo = new(std::nothrow) HdcSessionInfo();
    if (hSessionInfo == nullptr) {
        WRITE_LOG(LOG_FATAL, "PrepareNewSession new hSessionInfo failed");
        return nullptr;
    }
    hSessionInfo->sessionId = hChildSession->sessionId;
    hSessionInfo->classInstance = hChildSession->classInstance;
    hSessionInfo->classModule = hChildSession->classModule;
    hSessionInfo->hSession = hChildSession;
    auto funcNewSessionUp = [](uv_timer_t *handle) -> void {
        HSessionInfo hSessionInfo = reinterpret_cast<HSessionInfo>(handle->data);
        if (hSessionInfo == nullptr) {
            Base::TryCloseHandle(reinterpret_cast<uv_handle_t *>(handle), Base::CloseTimerCallback);
            WRITE_LOG(LOG_FATAL, "hSessionInfo is null");
            return;
        }
        HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(hSessionInfo->classInstance);
        if (daemon->IsSessionDeleted(hSessionInfo->sessionId)) {
            WRITE_LOG(LOG_INFO, "funcNewSessionUp session is deleted");
            delete hSessionInfo;
            handle->data = nullptr;
            Base::TryCloseHandle(reinterpret_cast<uv_handle_t *>(handle), Base::CloseTimerCallback);
            return;
        }
        HSession hChildSession = hSessionInfo->hSession;
        if (hChildSession->childLoop.active_handles == 0) {
            return;
        }
        if (!hChildSession->isDead) {
            auto ctrl = daemon->BuildCtrlString(SP_START_SESSION, 0, nullptr, 0);
            Base::SendToPollFd(hChildSession->ctrlFd[STREAM_MAIN], ctrl.data(), ctrl.size());
            WRITE_LOG(LOG_DEBUG, "Main thread usbio migrate finish");
        }
        delete hSessionInfo;
        handle->data = nullptr;
        Base::TryCloseHandle(reinterpret_cast<uv_handle_t *>(handle), Base::CloseTimerCallback);
    };
    Base::TimerUvTask(&daemon->loopMain, hSessionInfo, funcNewSessionUp);
    return hChildSession;
}

void HdcDaemonUSB::UvWriteCallback(uv_write_t *req, int status)
{
    StartTraceScope("HdcDaemonUSB::UvWriteCallback");
    if (status < 0) {
        constexpr int bufSize = 1024;
        char buf[bufSize] = { 0 };
        uv_strerror_r(status, buf, bufSize);
        WRITE_LOG(LOG_WARN, "SendCallback failed,status:%d %s", status, buf);
    }
    UvData *uvData = reinterpret_cast<UvData *>(req->data);
    if (uvData) {
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
        uvData->daemonUsb->cirbuf.Free(uvData->buf);
#else
        delete[] uvData->buf;
#endif
        delete uvData;
    }
    delete req;
}

int HdcDaemonUSB::UsbToStream(uv_stream_t *stream, const uint8_t *buf, const int size)
{
    StartTraceScope("HdcDaemonUSB::UsbToStream");
    int ret = ERR_GENERIC;
    uv_write_t *reqWrite = new uv_write_t();
    if (!reqWrite) {
        WRITE_LOG(LOG_WARN, "UsbToStream new write_t failed size:%d", size);
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
        cirbuf.Free(buf);
#else
        delete[] buf;
#endif
        return ERR_BUF_ALLOC;
    }
    uv_buf_t bfr;
    while (true) {
        UvData *uvData = new(std::nothrow) UvData();
        if (uvData == nullptr) {
            WRITE_LOG(LOG_FATAL, "UsbToStream new uvData failed size:%d", size);
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
            cirbuf.Free(buf);
#else
            delete[] buf;
#endif
            delete reqWrite;
            return ERR_BUF_ALLOC;
        }
        uvData->daemonUsb = this;
        uvData->buf = buf;
        reqWrite->data = reinterpret_cast<void *>(uvData);
        bfr.base = (char *)buf;
        bfr.len = size;
        if (!uv_is_writable(stream)) {
            WRITE_LOG(LOG_WARN, "UsbToStream uv_is_writable false size:%d", size);
            delete reqWrite;
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
            cirbuf.Free(buf);
#else
            delete[] buf;
#endif
            delete uvData;
            break;
        }
        ret = uv_write(reqWrite, stream, &bfr, 1, UvWriteCallback);
        if (ret < 0) {
            WRITE_LOG(LOG_WARN, "UsbToStream uv_write false ret:%d", ret);
            delete reqWrite;
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
            cirbuf.Free(buf);
#else
            delete[] buf;
#endif
            delete uvData;
            ret = ERR_IO_FAIL;
            break;
        }
        ret = size;
        break;
    }
    return ret;
}

int HdcDaemonUSB::UsbToHdcProtocol(uv_stream_t *stream, uint8_t *appendData, int dataSize)
{
    StartTraceScope("HdcDaemonUSB::UsbToHdcProtocol");
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
    uint8_t *data = cirbuf.Malloc();
#else
    uint8_t *data = new uint8_t[dataSize];
#endif
    if (data == nullptr) {
        WRITE_LOG(LOG_WARN, "UsbToHdcProtocol data nullptr");
        return -1;
    }
    if (memcpy_s(data, dataSize, appendData, dataSize)) {
        WRITE_LOG(LOG_WARN, "UsbToHdcProtocol memory copy failed dataSize:%d", dataSize);
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
        cirbuf.Free(data);
#else
        delete[] data;
#endif
        return ERR_BUF_COPY;
    }
    return UsbToStream(stream, data, dataSize);
}

int HdcDaemonUSB::DispatchToWorkThread(uint32_t sessionId, uint8_t *readBuf, int readBytes)
{
    HSession hChildSession = nullptr;
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(clsMainBase);
    int childRet = RET_SUCCESS;
    StartTraceScope("HdcDaemonUSB::DispatchToWorkThread");
    if (sessionId == 0) {
        // hdc packet data
        sessionId = currentSessionId;
    }
    if (currentSessionId != 0 && sessionId != currentSessionId) {
        WRITE_LOG(LOG_WARN, "New session coming, restart old sessionId:%u", currentSessionId);
        ResetOldSession(currentSessionId);
        currentSessionId = 0;
    }
    hChildSession = daemon->AdminSession(OP_QUERY, sessionId, nullptr);
    if (!hChildSession) {
        hChildSession = PrepareNewSession(sessionId, readBuf, readBytes);
        if (!hChildSession) {
            WRITE_LOG(LOG_WARN, "prep new session err for sessionId:%u", sessionId);
            return ERR_SESSION_NOFOUND;
        }
    }

    if (hChildSession->childCleared || hChildSession->isDead) {
        WRITE_LOG(LOG_WARN, "session dead clr:%d - %d", hChildSession->childCleared, hChildSession->isDead);
        return ERR_SESSION_DEAD;
    }
    uv_stream_t *stream = reinterpret_cast<uv_stream_t *>(&hChildSession->dataPipe[STREAM_MAIN]);
    if ((childRet = SendToHdcStream(hChildSession, stream, readBuf, readBytes)) < 0) {
        WRITE_LOG(LOG_WARN, "DispatchToWorkThread SendToHdcStream err ret:%d", childRet);
        return ERR_IO_FAIL;
    }
    return childRet;
}

bool HdcDaemonUSB::JumpAntiquePacket(const uint8_t &buf, ssize_t bytes) const
{
    constexpr size_t antiqueFlagSize = 4;
    constexpr size_t antiqueFullSize = 24;
    // anti CNXN 0x4e584e43
    uint8_t flag[] = { 0x43, 0x4e, 0x58, 0x4e };
    if (bytes == antiqueFullSize && !memcmp(&buf, flag, antiqueFlagSize)) {
        return true;
    }
    return false;
}

// Only physically swap EP ports will be reset
void HdcDaemonUSB::OnUSBRead(uv_fs_t *req)
{  // Only read at the main thread
    StartTraceScope("HdcDaemonUSB::OnUSBRead");
    auto ctxIo = reinterpret_cast<CtxUvFileCommonIo *>(req->data);
    auto hUSB = reinterpret_cast<HUSB>(ctxIo->data);
    auto thisClass = reinterpret_cast<HdcDaemonUSB *>(ctxIo->thisClass);
    uint8_t *bufPtr = ctxIo->buf;
    ssize_t bytesIOBytes = req->result;
    uint32_t sessionId = 0;
    bool ret = false;
    int childRet = 0;
    if (bytesIOBytes > hUSB->wMaxPacketSizeSend && bytesIOBytes != thisClass->saveNextReadSize) {
        WRITE_LOG(LOG_WARN, "Not full packet, wanted:%d really:%d", thisClass->saveNextReadSize, bytesIOBytes);
    }
    while (thisClass->isAlive) {
        // Don't care is module running, first deal with this
        if (bytesIOBytes < 0) {
            // logic alive and EINTER is gdb attach
            //
            // [about gdb attach known issue]
            // When GDB debugging is loaded, the number of USB read interrupts of libuv will increase. Multiple
            // interrupts will increase the correctness of USB data reading. Setting GDB to asynchronous mode or using
            // log debugging can avoid this problem
            if (bytesIOBytes != -EINTR) {  // Epoll will be broken when gdb attach
                constexpr int bufSize = 1024;
                char buf[bufSize] = { 0 };
                uv_strerror_r(bytesIOBytes, buf, bufSize);
                WRITE_LOG(LOG_WARN, "USBIO ret:%d failed:%s", bytesIOBytes, buf);
                ret = false;
                break;
            } else {
                WRITE_LOG(LOG_ALL, "OnUSBRead signal EINTR");
            }
        } else if (bytesIOBytes == 0) {  // zero packet
            WRITE_LOG(LOG_ALL, "Zero packet received");
        } else {
            if (thisClass->JumpAntiquePacket(*bufPtr, bytesIOBytes)) {
                WRITE_LOG(LOG_DEBUG, "JumpAntiquePacket auto jump");
                ret = true;
                break;
            }
            // guess is head of packet
            if ((childRet = thisClass->AvailablePacket((uint8_t *)bufPtr, bytesIOBytes, &sessionId)) != RET_SUCCESS) {
                if (childRet != ERR_IO_SOFT_RESET) {
                    WRITE_LOG(LOG_WARN, "AvailablePacket check failed, ret:%d buf:%-50s", bytesIOBytes, bufPtr);
                    break;
                }
                // reset packet
                childRet = 0;  // need max size
            } else {
                // AvailablePacket case
                if ((childRet = thisClass->DispatchToWorkThread(sessionId, bufPtr, bytesIOBytes)) < 0) {
                    WRITE_LOG(LOG_FATAL, "DispatchToWorkThread failed");
                    break;
                }
            }
        }
        int nextReadSize = childRet == 0 ? hUSB->wMaxPacketSizeSend : std::min(childRet, Base::GetUsbffsBulkSize());
        thisClass->saveNextReadSize = nextReadSize;
        if (thisClass->LoopUSBRead(hUSB, nextReadSize) < 0) {
            WRITE_LOG(LOG_FATAL, "LoopUSBRead failed");
            break;
        }
        ret = true;
        break;
    }
    if (!ret) {
        WRITE_LOG(LOG_INFO, "OnUSBRead ret false, set isAlive = false");
        thisClass->isAlive = false;
        thisClass->ctxRecv.atPollQueue = false;
    }
}

int HdcDaemonUSB::LoopUSBRead(HUSB hUSB, int readMaxWanted)
{
    StartTraceScope("HdcDaemonUSB::LoopUSBRead");
    int ret = ERR_GENERIC;
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(clsMainBase);
    uv_buf_t iov;
    ctxRecv.data = hUSB;
    ctxRecv.bufSize = readMaxWanted;
    ctxRecv.req = {};
    uv_fs_t *req = &ctxRecv.req;
    req->data = &ctxRecv;
    iov = uv_buf_init(reinterpret_cast<char *>(ctxRecv.buf), ctxRecv.bufSize);
    ret = uv_fs_read(&daemon->loopMain, req, hUSB->bulkOut, &iov, 1, -1, OnUSBRead);
    if (ret < 0) {
        WRITE_LOG(LOG_FATAL, "uv_fs_read ret:%d < 0", ret);
        return ERR_API_FAIL;
    }
    ctxRecv.atPollQueue = true;
    return RET_SUCCESS;
}

// Because USB can connect to only one host，daemonUSB is only one Session by default
void HdcDaemonUSB::WatchEPTimer(uv_timer_t *handle)
{
    HdcDaemonUSB *thisClass = (HdcDaemonUSB *)handle->data;
    HUSB hUSB = &thisClass->usbHandle;
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(thisClass->clsMainBase);
    if (thisClass->isAlive || thisClass->ctxRecv.atPollQueue) {
        return;
    }
    bool resetEp = false;
    do {
        if (hUSB->bulkIn > 0) {
            WRITE_LOG(LOG_DEBUG, "Watchdog close bulkin");
            thisClass->CloseBulkEp(true, thisClass->usbHandle.bulkIn, &daemon->loopMain);
            resetEp = true;
        }
        if (hUSB->bulkOut > 0) {
            WRITE_LOG(LOG_DEBUG, "Watchdog close bulkout");
            thisClass->CloseBulkEp(false, thisClass->usbHandle.bulkOut, &daemon->loopMain);
            resetEp = true;
        }
        if (thisClass->controlEp > 0) {
            Base::CloseFd(thisClass->controlEp);
            resetEp = true;
        }
    } while (false);
    if (resetEp || thisClass->usbHandle.bulkIn != 0 || thisClass->usbHandle.bulkOut != 0) {
        return;
    }
    // until all bulkport reset
    if (thisClass->ConnectEPPoint(hUSB) != RET_SUCCESS) {
        WRITE_LOG(LOG_DEBUG, "WatchEPTimer ConnectEPPoint failed");
        return;
    }
    // connect OK
    thisClass->isAlive = true;
    thisClass->LoopUSBRead(hUSB, hUSB->wMaxPacketSizeSend);
}
}  // namespace Hdc
