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
#include "file_descriptor.h"
#ifndef HDC_HOST
#include <sys/epoll.h>
#endif

namespace Hdc {
static const int SECONDS_TIMEOUT = 5;

HdcFileDescriptor::HdcFileDescriptor(uv_loop_t *loopIn, int fdToRead, void *callerContextIn,
                                     CallBackWhenRead callbackReadIn, CmdResultCallback callbackFinishIn,
                                     bool interactiveShell)
{
    loop = loopIn;
    workContinue = true;
    callbackFinish = callbackFinishIn;
    callbackRead = callbackReadIn;
    fdIO = fdToRead;
    refIO = 0;
    isInteractive = interactiveShell;
    callerContext = callerContextIn;
    if (isInteractive) {
        std::thread([this]() {
            HdcFileDescriptor::IOWriteThread(this);
        }).detach();
    }
}

HdcFileDescriptor::~HdcFileDescriptor()
{
    workContinue = false;
    if (isInteractive) {
        NotifyWrite();
        uv_sleep(MILL_SECONDS);
    }
}

bool HdcFileDescriptor::ReadyForRelease()
{
    return refIO == 0;
}

// just tryCloseFdIo = true, callback will be effect
void HdcFileDescriptor::StopWorkOnThread(bool tryCloseFdIo, std::function<void()> closeFdCallback)
{
    workContinue = false;
    if (isInteractive) {
        NotifyWrite();
    }

    callbackCloseFd = closeFdCallback;
    if (tryCloseFdIo && refIO > 0) {
        if (callbackCloseFd != nullptr) {
            callbackCloseFd();
        }
    }
}

void HdcFileDescriptor::FileIOOnThread(CtxFileIO *ctxIO, int bufSize)
{
#ifdef CONFIG_USE_JEMALLOC_DFX_INIF
    mallopt(M_DELAYED_FREE, M_DELAYED_FREE_DISABLE);
    mallopt(M_SET_THREAD_CACHE, M_THREAD_CACHE_DISABLE);
#endif
    HdcFileDescriptor *thisClass = ctxIO->thisClass;
    uint8_t *buf = ctxIO->bufIO;
    bool bFinish = false;
    bool fetalFinish = false;
    ssize_t nBytes;
#ifndef HDC_HOST
    constexpr int epollSize = 1;
    int epfd = epoll_create(epollSize);
    struct epoll_event ev;
    struct epoll_event events[epollSize];
    ev.data.fd = thisClass->fdIO;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_ADD, thisClass->fdIO, &ev);
#endif
    while (true) {
        if (thisClass->workContinue == false) {
            WRITE_LOG(LOG_INFO, "FileIOOnThread fdIO:%d workContinue false", thisClass->fdIO);
            bFinish = true;
            break;
        }

        if (memset_s(buf, bufSize, 0, bufSize) != EOK) {
            WRITE_LOG(LOG_DEBUG, "FileIOOnThread buf memset_s fail.");
            bFinish = true;
            break;
        }
#ifndef HDC_HOST
        int rc = epoll_wait(epfd, events, epollSize, SECONDS_TIMEOUT * TIME_BASE);
#else
        struct timeval timeout;
        timeout.tv_sec = SECONDS_TIMEOUT;
        timeout.tv_usec = 0;
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(thisClass->fdIO, &rset);
        int rc = select(thisClass->fdIO + 1, &rset, nullptr, nullptr, &timeout);
#endif
        if (rc < 0) {
            WRITE_LOG(LOG_FATAL, "FileIOOnThread select or epoll_wait fdIO:%d error:%d",
                thisClass->fdIO, errno);
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            bFinish = true;
            break;
        } else if (rc == 0) {
            WRITE_LOG(LOG_WARN, "FileIOOnThread select rc = 0, timeout.");
            continue;
        }
        nBytes = 0;
#ifndef HDC_HOST
        uint32_t event = events[0].events;
        if ((event & EPOLLIN) && (thisClass->fdIO > 0)) {
            nBytes = read(thisClass->fdIO, buf, bufSize);
        }
        if ((event & EPOLLERR) || (event & EPOLLHUP) || (event & EPOLLRDHUP)) {
            bFinish = true;
            fetalFinish = true;
            if ((nBytes > 0) && !thisClass->callbackRead(thisClass->callerContext, buf, nBytes)) {
                WRITE_LOG(LOG_WARN, "FileIOOnThread fdIO:%d callbackRead false", thisClass->fdIO);
            }
            break;
        }
        if (nBytes < 0 && (errno == EINTR || errno == EAGAIN)) {
            WRITE_LOG(LOG_WARN, "FileIOOnThread fdIO:%d read interrupt", thisClass->fdIO);
            continue;
        }
        if (nBytes > 0) {
            if (!thisClass->callbackRead(thisClass->callerContext, buf, nBytes)) {
                WRITE_LOG(LOG_WARN, "FileIOOnThread fdIO:%d callbackRead false", thisClass->fdIO);
                bFinish = true;
                break;
            }
            continue;
        } else {
            WRITE_LOG(LOG_INFO, "FileIOOnThread fd:%d nBytes:%d errno:%d",
                thisClass->fdIO, nBytes, errno);
            bFinish = true;
            fetalFinish = true;
            break;
        }
#else
        if (thisClass->fdIO > 0) {
            nBytes = read(thisClass->fdIO, buf, bufSize);
        }
        if (nBytes < 0 && (errno == EINTR || errno == EAGAIN)) {
            WRITE_LOG(LOG_WARN, "FileIOOnThread fdIO:%d read interrupt", thisClass->fdIO);
            continue;
        }
        if (nBytes > 0) {
            if (!thisClass->callbackRead(thisClass->callerContext, buf, nBytes)) {
                WRITE_LOG(LOG_WARN, "FileIOOnThread fdIO:%d callbackRead false", thisClass->fdIO);
                bFinish = true;
                break;
            }
            continue;
        } else {
            WRITE_LOG(LOG_INFO, "FileIOOnThread fd:%d nBytes:%d errno:%d",
                thisClass->fdIO, nBytes, errno);
            bFinish = true;
            fetalFinish = true;
            break;
        }
#endif
    }
#ifndef HDC_HOST
    if ((thisClass->fdIO > 0) && (epoll_ctl(epfd, EPOLL_CTL_DEL, thisClass->fdIO, nullptr) == -1)) {
        WRITE_LOG(LOG_INFO, "EPOLL_CTL_DEL fail fd:%d epfd:%d errno:%d",
            thisClass->fdIO, epfd, errno);
    }
    close(epfd);
#endif
    if (buf != nullptr) {
        delete[] buf;
        buf = nullptr;
    }
    delete ctxIO;

    --thisClass->refIO;
    if (bFinish) {
        thisClass->workContinue = false;
        thisClass->callbackFinish(thisClass->callerContext, fetalFinish, STRING_EMPTY);
    }
}

int HdcFileDescriptor::LoopReadOnThread()
{
#ifdef CONFIG_USE_JEMALLOC_DFX_INIF
    mallopt(M_DELAYED_FREE, M_DELAYED_FREE_DISABLE);
    mallopt(M_SET_THREAD_CACHE, M_THREAD_CACHE_DISABLE);
#endif
    int readMax = Base::GetMaxBufSizeStable() * 1.2; // 120% of max buf size, use stable size to avoid no buf.
    auto contextIO = new(std::nothrow) CtxFileIO();
    auto buf = new(std::nothrow) uint8_t[readMax]();
    if (!contextIO || !buf) {
        if (contextIO) {
            delete contextIO;
        }
        if (buf) {
            delete[] buf;
        }
        WRITE_LOG(LOG_FATAL, "Memory alloc failed");
        callbackFinish(callerContext, true, "Memory alloc failed");
        return -1;
    }
    contextIO->bufIO = buf;
    contextIO->thisClass = this;
    ++refIO;
    std::thread([contextIO, readMax]() {
        HdcFileDescriptor::FileIOOnThread(contextIO, readMax);
    }).detach();
    return 0;
}

bool HdcFileDescriptor::StartWorkOnThread()
{
    if (LoopReadOnThread() < 0) {
        return false;
    }
    return true;
}

int HdcFileDescriptor::Write(uint8_t *data, int size)
{
    if (size > static_cast<int>(HDC_BUF_MAX_BYTES - 1)) {
        size = static_cast<int>(HDC_BUF_MAX_BYTES - 1);
    }
    if (size <= 0) {
        WRITE_LOG(LOG_WARN, "Write failed, size:%d", size);
        return -1;
    }
    auto buf = new(std::nothrow) uint8_t[size];
    if (!buf) {
        return -1;
    }
    if (memcpy_s(buf, size, data, size) != EOK) {
        delete[] buf;
        return -1;
    }
    return WriteWithMem(buf, size);
}

// Data's memory must be Malloc, and the callback FREE after this function is completed
int HdcFileDescriptor::WriteWithMem(uint8_t *data, int size)
{
#ifdef CONFIG_USE_JEMALLOC_DFX_INIF
    mallopt(M_DELAYED_FREE, M_DELAYED_FREE_DISABLE);
    mallopt(M_SET_THREAD_CACHE, M_THREAD_CACHE_DISABLE);
#endif
    auto contextIO = new(std::nothrow) CtxFileIO();
    if (!contextIO) {
        delete[] data;
        WRITE_LOG(LOG_FATAL, "Memory alloc failed");
        callbackFinish(callerContext, true, "Memory alloc failed");
        return -1;
    }
    contextIO->bufIO = data;
    contextIO->size = static_cast<size_t>(size);
    contextIO->thisClass = this;
    PushWrite(contextIO);
    NotifyWrite();
    return size;
}

void HdcFileDescriptor::IOWriteThread(void *object)
{
    HdcFileDescriptor *hfd = reinterpret_cast<HdcFileDescriptor *>(object);
    while (hfd->workContinue) {
        hfd->HandleWrite();
        hfd->WaitWrite();
    }
}

void HdcFileDescriptor::PushWrite(CtxFileIO *cfio)
{
    std::unique_lock<std::mutex> lock(writeMutex);
    writeQueue.push(cfio);
}

CtxFileIO *HdcFileDescriptor::PopWrite()
{
    std::unique_lock<std::mutex> lock(writeMutex);
    CtxFileIO *cfio = nullptr;
    if (!writeQueue.empty()) {
        cfio = writeQueue.front();
        writeQueue.pop();
    }
    return cfio;
}

void HdcFileDescriptor::NotifyWrite()
{
    writeCond.notify_one();
}

void HdcFileDescriptor::WaitWrite()
{
    std::unique_lock<std::mutex> lock(writeMutex);
    writeCond.wait_for(lock, std::chrono::seconds(WAIT_SECONDS), [&]() {
        return !writeQueue.empty() || !workContinue;
    });
}

void HdcFileDescriptor::HandleWrite()
{
    CtxFileIO *cfio = nullptr;
    while ((cfio = PopWrite()) != nullptr) {
        CtxFileIOWrite(cfio);
        delete cfio;
    }
}

void HdcFileDescriptor::CtxFileIOWrite(CtxFileIO *cfio)
{
    std::unique_lock<std::mutex> lock(writeMutex);
    uint8_t *buf = cfio->bufIO;
    uint8_t *data = buf;
    size_t cnt = cfio->size;
    constexpr int intrmax = 1000;
    int intrcnt = 0;
    while (cnt > 0) {
        ssize_t rc = write(fdIO, data, cnt);
        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                if (++intrcnt > intrmax) {
                    WRITE_LOG(LOG_WARN, "CtxFileIOWrite fdIO:%d interrupt errno:%d", fdIO, errno);
                    intrcnt = 0;
                }
                continue;
            } else {
                WRITE_LOG(LOG_FATAL, "CtxFileIOWrite fdIO:%d rc:%d error:%d", fdIO, rc, errno);
                break;
            }
        }
        data += rc;
        cnt -= static_cast<size_t>(rc);
    }
    delete[] buf;
}
}  // namespace Hdc
