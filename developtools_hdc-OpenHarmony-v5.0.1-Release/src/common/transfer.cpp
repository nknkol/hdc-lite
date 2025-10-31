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
#include "transfer.h"
#include "serial_struct.h"
#include <sys/stat.h>
#ifdef HARMONY_PROJECT
#include <lz4.h>
#endif
#if (!(defined(HOST_MINGW)||defined(HOST_MAC))) && defined(SURPPORT_SELINUX)
#include <selinux/selinux.h>
#endif
namespace Hdc {
constexpr uint64_t HDC_TIME_CONVERT_BASE = 1000000000;


HdcTransferBase::HdcTransferBase(HTaskInfo hTaskInfo)
    : HdcTaskBase(hTaskInfo)
{
    ResetCtx(&ctxNow, true);
    commandBegin = 0;
    commandData = 0;
    isStableBuf = false;
}

HdcTransferBase::~HdcTransferBase()
{
    if (ctxNow.isFdOpen) {
        WRITE_LOG(LOG_DEBUG, "~HdcTransferBase channelId:%u lastErrno:%u result:%d ioFinish:%d",
            taskInfo->channelId, ctxNow.lastErrno, ctxNow.fsOpenReq.result, ctxNow.ioFinish);
        
        if (ctxNow.lastErrno != 0 || (ctxNow.fsOpenReq.result > 0 && !ctxNow.ioFinish)) {
            uv_fs_close(nullptr, &ctxNow.fsCloseReq, ctxNow.fsOpenReq.result, nullptr);
            ctxNow.isFdOpen = false;
        }
    } else {
        WRITE_LOG(LOG_DEBUG, "~HdcTransferBase channelId:%u lastErrno:%u ioFinish:%d",
            taskInfo->channelId, ctxNow.lastErrno, ctxNow.ioFinish);
    }
};

bool HdcTransferBase::ResetCtx(CtxFile *context, bool full)
{
    if (full) {
        *context = {};
        context->fsOpenReq.data = context;
        context->fsCloseReq.data = context;
        context->thisClass = this;
        context->loop = loopTask;
        context->cb = OnFileIO;
    }
    context->closeNotify = false;
    context->indexIO = 0;
    context->lastErrno = 0;
    context->ioFinish = false;
    context->closeReqSubmitted = false;
    return true;
}

int HdcTransferBase::SimpleFileIO(CtxFile *context, uint64_t index, uint8_t *sendBuf, int bytes)
{
    StartTraceScope("HdcTransferBase::SimpleFileIO");
    // The first 8 bytes file offset
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
    uint8_t *buf = cirbuf.Malloc();
#else
    uint8_t *buf = new uint8_t[bytes + payloadPrefixReserve]();
#endif
    if (buf == nullptr) {
        WRITE_LOG(LOG_FATAL, "SimpleFileIO buf nullptr");
        return -1;
    }
    CtxFileIO *ioContext = new(std::nothrow) CtxFileIO();
    if (ioContext == nullptr) {
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
        cirbuf.Free(buf);
#else
        delete[] buf;
#endif
        WRITE_LOG(LOG_FATAL, "SimpleFileIO ioContext nullptr");
        return -1;
    }
    bool ret = false;
    while (true) {
        size_t bufMaxSize = context->isStableBufSize ?
            static_cast<size_t>(Base::GetUsbffsBulkSizeStable() - payloadPrefixReserve) :
            static_cast<size_t>(Base::GetUsbffsBulkSize() - payloadPrefixReserve);
        if (bytes < 0 || static_cast<size_t>(bytes) > bufMaxSize) {
            WRITE_LOG(LOG_DEBUG, "SimpleFileIO param check failed");
            break;
        }
        if (context->ioFinish) {
            WRITE_LOG(LOG_DEBUG, "SimpleFileIO to closed IOStream");
            break;
        }
        uv_fs_t *req = &ioContext->fs;
        ioContext->bufIO = buf + payloadPrefixReserve;
        ioContext->context = context;
        req->data = ioContext;
        ++refCount;
        if (context->master) {  // master just read, and slave just write.when master/read, sendBuf can be nullptr
            uv_buf_t iov = uv_buf_init(reinterpret_cast<char *>(ioContext->bufIO), bytes);
            uv_fs_read(context->loop, req, context->fsOpenReq.result, &iov, 1, index, context->cb);
        } else {
            // The US_FS_WRITE here must be brought into the actual file offset, which cannot be incorporated with local
            // accumulated index because UV_FS_WRITE will be executed multiple times and then trigger a callback.
            if (bytes > 0 && memcpy_s(ioContext->bufIO, bufMaxSize, sendBuf, bytes) != EOK) {
                WRITE_LOG(LOG_WARN, "SimpleFileIO memcpy error");
                break;
            }
            uv_buf_t iov = uv_buf_init(reinterpret_cast<char *>(ioContext->bufIO), bytes);
            uv_fs_write(context->loop, req, context->fsOpenReq.result, &iov, 1, index, context->cb);
        }
        ret = true;
        break;
    }
    if (!ret) {
        if (ioContext != nullptr) {
            delete ioContext;
            ioContext = nullptr;
        }
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
        cirbuf.Free(buf);
#else
        delete[] buf;
#endif
        return -1;
    }
    return bytes;
}

void HdcTransferBase::OnFileClose(uv_fs_t *req)
{
    StartTraceScope("HdcTransferBase::OnFileClose");
    uv_fs_req_cleanup(req);
    CtxFile *context = (CtxFile *)req->data;
    context->closeReqSubmitted = false;
    HdcTransferBase *thisClass = (HdcTransferBase *)context->thisClass;
    if (context->closeNotify) {
        // close-step2
        // maybe successful finish or failed finish
        thisClass->WhenTransferFinish(context);
    }
    --thisClass->refCount;
    return;
}

void HdcTransferBase::SetFileTime(CtxFile *context)
{
    if (!context->transferConfig.holdTimestamp) {
        return;
    }
    if (!context->transferConfig.mtime) {
        return;
    }
    uv_fs_t fs;
    double aTimeSec = static_cast<long double>(context->transferConfig.atime) / HDC_TIME_CONVERT_BASE;
    double mTimeSec = static_cast<long double>(context->transferConfig.mtime) / HDC_TIME_CONVERT_BASE;
    uv_fs_futime(nullptr, &fs, context->fsOpenReq.result, aTimeSec, mTimeSec, nullptr);
    uv_fs_req_cleanup(&fs);
}

bool HdcTransferBase::SendIOPayload(CtxFile *context, uint64_t index, uint8_t *data, int dataSize)
{
    TransferPayload payloadHead;
    string head;
    int compressSize = 0;
    int sendBufSize = payloadPrefixReserve + dataSize;
    uint8_t *sendBuf = data - payloadPrefixReserve;
    bool ret = false;

    StartTraceScope("HdcTransferBase::SendIOPayload");
    payloadHead.compressType = context->transferConfig.compressType;
    payloadHead.uncompressSize = dataSize;
    payloadHead.index = index;
    if (dataSize > 0) {
        switch (payloadHead.compressType) {
#ifdef HARMONY_PROJECT
            case COMPRESS_LZ4: {
                sendBuf = new uint8_t[sendBufSize]();
                if (!sendBuf) {
                    WRITE_LOG(LOG_FATAL, "alloc LZ4 buffer failed");
                    return false;
                }
                compressSize = LZ4_compress_default((const char *)data, (char *)sendBuf + payloadPrefixReserve,
                                                    dataSize, dataSize);
                break;
            }
#endif
            default: {  // COMPRESS_NONE
                compressSize = dataSize;
                break;
            }
        }
    }
    payloadHead.compressSize = compressSize;
    head = SerialStruct::SerializeToString(payloadHead);
    if (head.size() + 1 > payloadPrefixReserve) {
        goto out;
    }
    if (EOK != memcpy_s(sendBuf, sendBufSize, head.c_str(), head.size() + 1)) {
        goto out;
    }
    ret = SendToAnother(commandData, sendBuf, payloadPrefixReserve + compressSize) > 0;

out:
    if (dataSize > 0 && payloadHead.compressType == COMPRESS_LZ4) {
        delete[] sendBuf;
    }
    return ret;
}

void HdcTransferBase::OnFileIO(uv_fs_t *req)
{
    CtxFileIO *contextIO = reinterpret_cast<CtxFileIO *>(req->data);
    CtxFile *context = reinterpret_cast<CtxFile *>(contextIO->context);
    HdcTransferBase *thisClass = (HdcTransferBase *)context->thisClass;
    uint8_t *bufIO = contextIO->bufIO;
    StartTraceScope("HdcTransferBase::OnFileIO");
    uv_fs_req_cleanup(req);
    while (true) {
        if (context->ioFinish) {
            break;
        }
        if (req->result < 0) {
            constexpr int bufSize = 1024;
            char buf[bufSize] = { 0 };
            uv_strerror_r((int)req->result, buf, bufSize);
            WRITE_LOG(LOG_DEBUG, "OnFileIO error: %s", buf);
            context->closeNotify = true;
            context->lastErrno = abs(req->result);
            context->ioFinish = true;
            break;
        }
        context->indexIO += req->result;
        if (req->fs_type == UV_FS_READ) {
#ifdef HDC_DEBUG
            WRITE_LOG(LOG_DEBUG, "read file data %" PRIu64 "/%" PRIu64 "", context->indexIO,
                      context->fileSize);
#endif // HDC_DEBUG
            if (!thisClass->SendIOPayload(context, context->indexIO - req->result, bufIO, req->result)) {
                context->ioFinish = true;
                break;
            }
            if (req->result == 0) {
                context->ioFinish = true;
                WRITE_LOG(LOG_DEBUG, "path:%s fd:%d eof",
                    context->localPath.c_str(), context->fsOpenReq.result);
                break;
            }
            if (context->indexIO < context->fileSize) {
                thisClass->SimpleFileIO(context, context->indexIO, nullptr, context->isStableBufSize ?
                    (Base::GetMaxBufSizeStable() * thisClass->maxTransferBufFactor) :
                    (Base::GetMaxBufSize() * thisClass->maxTransferBufFactor));
            } else {
                context->ioFinish = true;
            }
        } else if (req->fs_type == UV_FS_WRITE) {  // write
#ifdef HDC_DEBUG
            WRITE_LOG(LOG_DEBUG, "write file data %" PRIu64 "/%" PRIu64 "", context->indexIO,
                      context->fileSize);
#endif // HDC_DEBUG
            if (context->indexIO >= context->fileSize || req->result == 0) {
                // The active end must first read it first, but you can't make Finish first, because Slave may not
                // end.Only slave receives complete talents Finish
                context->closeNotify = true;
                context->ioFinish = true;
                thisClass->SetFileTime(context);
            }
        } else {
            context->ioFinish = true;
        }
        break;
    }
    if (context->ioFinish) {
        // close-step1
        ++thisClass->refCount;
        if (req->fs_type == UV_FS_WRITE) {
            uv_fs_fsync(thisClass->loopTask, &context->fsCloseReq, context->fsOpenReq.result, nullptr);
        }
        WRITE_LOG(LOG_DEBUG, "channelId:%u result:%d, closeReqSubmitted:%d",
                  thisClass->taskInfo->channelId, context->fsOpenReq.result, context->closeReqSubmitted);
        if (context->lastErrno == 0 && !context->closeReqSubmitted) {
            context->closeReqSubmitted = true;
            WRITE_LOG(LOG_DEBUG, "OnFileIO fs_close, channelId:%u", thisClass->taskInfo->channelId);
            uv_fs_close(thisClass->loopTask, &context->fsCloseReq, context->fsOpenReq.result, OnFileClose);
            context->isFdOpen = false;
        } else {
            thisClass->WhenTransferFinish(context);
            --thisClass->refCount;
        }
    }
#ifndef CONFIG_USE_JEMALLOC_DFX_INIF
    thisClass->cirbuf.Free(bufIO - payloadPrefixReserve);
#else
    delete [] (bufIO - payloadPrefixReserve);
#endif
    --thisClass->refCount;
    delete contextIO;  // Req is part of the Contextio structure, no free release
}

void HdcTransferBase::OnFileOpen(uv_fs_t *req)
{
    CtxFile *context = (CtxFile *)req->data;
    HdcTransferBase *thisClass = (HdcTransferBase *)context->thisClass;
    StartTraceScope("HdcTransferBase::OnFileOpen");
    uv_fs_req_cleanup(req);
    WRITE_LOG(LOG_DEBUG, "Filemod openfile:%s channelId:%u result:%d",
        context->localPath.c_str(), thisClass->taskInfo->channelId, context->fsOpenReq.result);
    --thisClass->refCount;
    if (req->result <= 0) {
        constexpr int bufSize = 1024;
        char buf[bufSize] = { 0 };
        uv_strerror_r((int)req->result, buf, bufSize);
        thisClass->LogMsg(MSG_FAIL, "Error opening file: %s, path:%s", buf,
                          context->localPath.c_str());
        WRITE_LOG(LOG_FATAL, "open path:%s error:%s", context->localPath.c_str(), buf);
        if (context->isDir && context->master) {
            uint8_t payload = 1;
            thisClass->CommandDispatch(CMD_FILE_FINISH, &payload, 1);
        } else if (context->isDir && !context->master) {
            uint8_t payload = 1;
            thisClass->SendToAnother(CMD_FILE_FINISH, &payload, 1);
        } else {
            thisClass->TaskFinish();
        }
        return;
    }
    thisClass->ResetCtx(context);
    context->isFdOpen = true;
    if (context->master) { // master just read, and slave just write.
        // init master
        uv_fs_t fs = {};
        uv_fs_fstat(nullptr, &fs, context->fsOpenReq.result, nullptr);
        WRITE_LOG(LOG_DEBUG, "uv_fs_fstat result:%d fileSize:%llu",
            context->fsOpenReq.result, fs.statbuf.st_size);
        TransferConfig &st = context->transferConfig;
        st.fileSize = fs.statbuf.st_size;
        st.optionalName = context->localName;
        if (st.holdTimestamp) {
            st.atime = fs.statbuf.st_atim.tv_sec * HDC_TIME_CONVERT_BASE + fs.statbuf.st_atim.tv_nsec;
            st.mtime = fs.statbuf.st_mtim.tv_sec * HDC_TIME_CONVERT_BASE + fs.statbuf.st_mtim.tv_nsec;
        }
        st.path = context->remotePath;
        // update ctxNow=context child value
        context->fileSize = st.fileSize;
        context->fileMode.perm = fs.statbuf.st_mode;
        context->fileMode.uId = fs.statbuf.st_uid;
        context->fileMode.gId = fs.statbuf.st_gid;
#if (!(defined(HOST_MINGW)||defined(HOST_MAC))) && defined(SURPPORT_SELINUX)
        char *con = nullptr;
        getfilecon(context->localPath.c_str(), &con);
        if (con != nullptr) {
            context->fileMode.context = con;
            freecon(con);
        }
#endif
        uv_fs_req_cleanup(&fs);
        thisClass->CheckMaster(context);
    } else {  // write
        if (context->fileModeSync) {
            FileMode &mode = context->fileMode;
            uv_fs_t fs = {};
            uv_fs_chmod(nullptr, &fs, context->localPath.c_str(), mode.perm, nullptr);
            uv_fs_chown(nullptr, &fs, context->localPath.c_str(), mode.uId, mode.gId, nullptr);
            uv_fs_req_cleanup(&fs);

#if (!(defined(HOST_MINGW)||defined(HOST_MAC))) && defined(SURPPORT_SELINUX)
            if (!mode.context.empty()) {
                WRITE_LOG(LOG_DEBUG, "setfilecon from master = %s", mode.context.c_str());
                setfilecon(context->localPath.c_str(), mode.context.c_str());
            }
#endif
        }
        union FeatureFlagsUnion f{};
        if (!thisClass->AddFeatures(f)) {
            WRITE_LOG(LOG_FATAL, "AddFeatureFlag failed");
            thisClass->SendToAnother(thisClass->commandBegin, nullptr, 0);
        } else {
            thisClass->SendToAnother(thisClass->commandBegin, f.raw, sizeof(f));
        }
    }
}

bool HdcTransferBase::MatchPackageExtendName(string fileName, string extName)
{
    bool match = false;
    int subfixIndex = fileName.rfind(extName);
    if ((fileName.size() - subfixIndex) != extName.size()) {
        return false;
    }
    match = true;
    return match;
}

// filter can be empty
int HdcTransferBase::GetSubFiles(const char *path, string filter, vector<string> *out)
{
    int retNum = 0;
    uv_fs_t req = {};
    uv_dirent_t dent;
    vector<string> filterStrings;
    if (!strlen(path)) {
        return retNum;
    }
    if (filter.size()) {
        Base::SplitString(filter, ";", filterStrings);
    }

    if (uv_fs_scandir(nullptr, &req, path, 0, nullptr) < 0) {
        uv_fs_req_cleanup(&req);
        return retNum;
    }
    while (uv_fs_scandir_next(&req, &dent) != UV_EOF) {
        // Skip. File
        if (strcmp(dent.name, ".") == 0 || strcmp(dent.name, "..") == 0) {
            continue;
        }
        if (!(static_cast<uint32_t>(dent.type) & UV_DIRENT_FILE)) {
            continue;
        }
        string fileName = dent.name;
        for (auto &&s : filterStrings) {
            int subfixIndex = fileName.rfind(s);
            if ((fileName.size() - subfixIndex) != s.size())
                continue;
            string fullPath = string(path) + Base::GetPathSep();
            fullPath += fileName;
            out->push_back(fullPath);
            ++retNum;
        }
    }
    uv_fs_req_cleanup(&req);
    return retNum;
}


int HdcTransferBase::GetSubFilesRecursively(string path, string currentDirname, vector<string> *out)
{
    int retNum = 0;
    uv_fs_t req = {};
    uv_dirent_t dent;

    WRITE_LOG(LOG_DEBUG, "GetSubFiles path = %s currentDirname = %s", path.c_str(), currentDirname.c_str());

    if (!path.size()) {
        return retNum;
    }

    if (uv_fs_scandir(nullptr, &req, path.c_str(), 0, nullptr) < 0) {
        uv_fs_req_cleanup(&req);
        return retNum;
    }

    uv_fs_t fs = {};
    int ret = uv_fs_stat(nullptr, &fs, path.c_str(), nullptr);
    if (ret == 0) {
        FileMode mode;
        mode.fullName = currentDirname;
        mode.perm = fs.statbuf.st_mode;
        mode.uId = fs.statbuf.st_uid;
        mode.gId = fs.statbuf.st_gid;

#if (!(defined(HOST_MINGW)||defined(HOST_MAC))) && defined(SURPPORT_SELINUX)
        char *con = nullptr;
        getfilecon(path.c_str(), &con);
        if (con != nullptr) {
            mode.context = con;
            freecon(con);
        }
#endif
        ctxNow.dirMode.push_back(mode);
    }
    while (uv_fs_scandir_next(&req, &dent) != UV_EOF) {
        // Skip. File
        if (strcmp(dent.name, ".") == 0 || strcmp(dent.name, "..") == 0) {
            continue;
        }
        if (!(static_cast<uint32_t>(dent.type) & UV_DIRENT_FILE)) {
            WRITE_LOG(LOG_DEBUG, "subdir dent.name fileName = %s", dent.name);
            GetSubFilesRecursively(path + Base::GetPathSep() + dent.name,
                currentDirname + Base::GetPathSep() + dent.name, out);
            continue;
        }
        string fileName = dent.name;
        WRITE_LOG(LOG_DEBUG, "GetSubFiles fileName = %s", fileName.c_str());

        out->push_back(currentDirname + Base::GetPathSep() + fileName);
    }
    uv_fs_req_cleanup(&req);
    return retNum;
}


bool HdcTransferBase::CheckLocalPath(string &localPath, string &optName, string &errStr)
{
    // If optName show this is directory mode, check localPath and try create each layer
    WRITE_LOG(LOG_DEBUG, "CheckDirectory localPath = %s optName = %s", localPath.c_str(), optName.c_str());
    if ((optName.find('/') == string::npos) && (optName.find('\\') == string::npos)) {
        WRITE_LOG(LOG_DEBUG, "Not directory mode optName = %s,  return", optName.c_str());
        return true;
    }
    ctxNow.isDir = true;
    uv_fs_t req;
    int r = uv_fs_lstat(nullptr, &req, localPath.c_str(), nullptr);
    mode_t mode = req.statbuf.st_mode;
    uv_fs_req_cleanup(&req);

    if (r) {
        vector<string> dirsOflocalPath;
        string split(1, Base::GetPathSep());
        Base::SplitString(localPath, split, dirsOflocalPath);

        WRITE_LOG(LOG_DEBUG, "localPath = %s dir layers = %zu", localPath.c_str(), dirsOflocalPath.size());
        string makedirPath;

        if (!Base::IsAbsolutePath(localPath)) {
            makedirPath = ".";
        }

        for (auto dir : dirsOflocalPath) {
            WRITE_LOG(LOG_DEBUG, "CheckLocalPath create dir = %s", dir.c_str());

            if (dir == ".") {
                continue;
            } else {
#ifdef _WIN32
                if (dir.find(":") == 1) {
                    makedirPath = dir;
                    continue;
                }
#endif
                makedirPath = makedirPath + Base::GetPathSep() + dir;
                if (!Base::TryCreateDirectory(makedirPath, errStr)) {
                    return false;
                }
            }
        }
        // set flag to remove first layer directory of filename from master
        ctxNow.targetDirNotExist = true;
    } else if (!(mode & S_IFDIR)) {
        WRITE_LOG(LOG_WARN, "Not a directory, path:%s", localPath.c_str());
        errStr = "Not a directory, path:";
        errStr += localPath.c_str();
        return false;
    }
    return true;
}

bool HdcTransferBase::CheckFilename(string &localPath, string &optName, string &errStr)
{
    string localPathBackup = localPath;
    if (ctxNow.targetDirNotExist) {
        // If target directory not exist, the first layer directory from master should remove
        if (optName.find('/') != string::npos) {
            optName = optName.substr(optName.find('/') + 1);
        } else if (optName.find('\\') != string::npos) {
            optName = optName.substr(optName.find('\\') + 1);
        }
    }
    vector<string> dirsOfOptName;

    if (optName.find('/') != string::npos) {
        Base::SplitString(optName, "/", dirsOfOptName);
    } else if (optName.find('\\') != string::npos) {
        Base::SplitString(optName, "\\", dirsOfOptName);
    } else {
        WRITE_LOG(LOG_DEBUG, "No need create dir for file = %s", optName.c_str());
        return true;
    }

    // If filename still include dir, try create each layer
    optName = dirsOfOptName.back();
    dirsOfOptName.pop_back();

    for (auto s : dirsOfOptName) {
        // Add each layer directory to localPath
        localPath = localPath + Base::GetPathSep() + s;
        if (!Base::TryCreateDirectory(localPath, errStr)) {
            return false;
        }
        if (ctxNow.fileModeSync) {
            string resolvedPath = Base::CanonicalizeSpecPath(localPath);
            auto pos = resolvedPath.find(localPathBackup);
            if (pos == 0) {
                string shortPath = resolvedPath.substr(localPathBackup.size());
                if (shortPath.at(0) == Base::GetPathSep()) {
                    shortPath = shortPath.substr(1);
                }
                WRITE_LOG(LOG_DEBUG, "pos = %zu, shortPath = %s", pos, shortPath.c_str());

                // set mode
                auto it = ctxNow.dirModeMap.find(shortPath);
                if (it != ctxNow.dirModeMap.end()) {
                    auto mode = it->second;
                    uv_fs_t fs = {};
                    uv_fs_chmod(nullptr, &fs, localPath.c_str(), mode.perm, nullptr);
                    uv_fs_chown(nullptr, &fs, localPath.c_str(), mode.uId, mode.gId, nullptr);
                    uv_fs_req_cleanup(&fs);
#if (!(defined(HOST_MINGW) || defined(HOST_MAC))) && defined(SURPPORT_SELINUX)
                    if (!mode.context.empty()) {
                        WRITE_LOG(LOG_DEBUG, "setfilecon from master = %s", mode.context.c_str());
                        setfilecon(localPath.c_str(), mode.context.c_str());
                    }
#endif
                }
            }
        }
    }

    WRITE_LOG(LOG_DEBUG, "CheckFilename finish localPath:%s optName:%s", localPath.c_str(), optName.c_str());
    return true;
}

// https://en.cppreference.com/w/cpp/filesystem/is_directory
// return true if file exist， false if file not exist
bool HdcTransferBase::SmartSlavePath(string &cwd, string &localPath, const char *optName)
{
    string errStr;
    if (taskInfo->serverOrDaemon) {
        // slave and server
        ExtractRelativePath(cwd, localPath);
    }
    mode_t mode = mode_t(~S_IFMT);
    if (Base::CheckDirectoryOrPath(localPath.c_str(), true, false, errStr, mode)) {
        WRITE_LOG(LOG_DEBUG, "%s", errStr.c_str());
        return true;
    }

    uv_fs_t req;
    int r = uv_fs_lstat(nullptr, &req, localPath.c_str(), nullptr);
    uv_fs_req_cleanup(&req);
    if (r == 0 && (req.statbuf.st_mode & S_IFDIR)) {  // is dir
        localPath = localPath + Base::GetPathSep() + optName;
    }
    if (r != 0 && (localPath.back() == Base::GetPathSep())) { // not exist and is dir
        localPath = localPath + optName;
    }
    return false;
}

bool HdcTransferBase::RecvIOPayload(CtxFile *context, uint8_t *data, int dataSize)
{
    if (dataSize < static_cast<int>(payloadPrefixReserve)) {
        WRITE_LOG(LOG_WARN, "unable to parse TransferPayload: invalid dataSize %d", dataSize);
        return false;
    }
    uint8_t *clearBuf = nullptr;
    string serialString(reinterpret_cast<char *>(data), payloadPrefixReserve);
    TransferPayload pld;
    Base::ZeroStruct(pld);
    bool ret = false;
    SerialStruct::ParseFromString(pld, serialString);
    int clearSize = 0;
    StartTraceScope("HdcTransferBase::RecvIOPayload");
    if (pld.compressSize > static_cast<uint32_t>(dataSize) || pld.uncompressSize > MAX_SIZE_IOBUF) {
        WRITE_LOG(LOG_FATAL, "compress size is greater than the dataSize. pld.compressSize = %d", pld.compressSize);
        return false;
    }
    if (pld.compressSize > 0) {
        switch (pld.compressType) {
#ifdef HARMONY_PROJECT
            case COMPRESS_LZ4: {
                clearBuf = new uint8_t[pld.uncompressSize]();
                if (!clearBuf) {
                    WRITE_LOG(LOG_FATAL, "alloc LZ4 buffer failed");
                    return false;
                }
                clearSize = LZ4_decompress_safe((const char *)data + payloadPrefixReserve, (char *)clearBuf,
                                                pld.compressSize, pld.uncompressSize);
                break;
            }
#endif
            default: {  // COMPRESS_NONE
                clearBuf = data + payloadPrefixReserve;
                clearSize = pld.compressSize;
                break;
            }
        }
    }
    while (true) {
        if (static_cast<uint32_t>(clearSize) != pld.uncompressSize || dataSize - payloadPrefixReserve < clearSize) {
            WRITE_LOG(LOG_WARN, "invalid data size for fileIO: %d", clearSize);
            break;
        }
        if (SimpleFileIO(context, pld.index, clearBuf, clearSize) < 0) {
            break;
        }
        ret = true;
        break;
    }
    if (pld.compressSize > 0 && pld.compressType != COMPRESS_NONE) {
        delete[] clearBuf;
    }
    return ret;
}

bool HdcTransferBase::CommandDispatch(const uint16_t command, uint8_t *payload, const int payloadSize)
{
    StartTraceScope("HdcTransferBase::CommandDispatch");
    bool ret = true;
    while (true) {
        if (command == commandBegin) {
            CtxFile *context = &ctxNow;
            if (!CheckFeatures(context, payload, payloadSize)) {
                WRITE_LOG(LOG_FATAL, "CommandDispatch CheckFeatures command:%u", command);
                ret = false;
                break;
            }
            int ioRet = SimpleFileIO(context, context->indexIO, nullptr, (context->isStableBufSize) ?
                Base::GetMaxBufSizeStable() * maxTransferBufFactor :
                Base::GetMaxBufSize() * maxTransferBufFactor);
            if (ioRet < 0) {
                WRITE_LOG(LOG_FATAL, "CommandDispatch SimpleFileIO ioRet:%d", ioRet);
                ret = false;
                break;
            }
            context->transferBegin = Base::GetRuntimeMSec();
        } else if (command == commandData) {
            if (static_cast<uint32_t>(payloadSize) > HDC_BUF_MAX_BYTES || payloadSize < 0) {
                WRITE_LOG(LOG_FATAL, "CommandDispatch payloadSize:%d", payloadSize);
                ret = false;
                break;
            }
            // Note, I will trigger FileIO after multiple times.
            CtxFile *context = &ctxNow;
            if (!RecvIOPayload(context, payload, payloadSize)) {
                WRITE_LOG(LOG_DEBUG, "RecvIOPayload return false. channelId:%u lastErrno:%u result:%d",
                    taskInfo->channelId, ctxNow.lastErrno, ctxNow.fsOpenReq.result);
                uv_fs_close(nullptr, &ctxNow.fsCloseReq, ctxNow.fsOpenReq.result, nullptr);
                ctxNow.isFdOpen = false;
                HdcTransferBase *thisClass = (HdcTransferBase *)context->thisClass;
                thisClass->CommandDispatch(CMD_FILE_FINISH, payload, 1);
                ret = false;
                break;
            }
        } else {
            // Other subclass commands
        }
        break;
    }
    return ret;
}

void HdcTransferBase::ExtractRelativePath(string &cwd, string &path)
{
    bool absPath = Base::IsAbsolutePath(path);
    if (!absPath) {
        path = cwd + path;
    }
}

bool HdcTransferBase::AddFeatures(FeatureFlagsUnion &feature)
{
    feature.bits.hugeBuf = !isStableBuf;
    return true;
}

bool HdcTransferBase::CheckFeatures(CtxFile *context, uint8_t *payload, const int payloadSize)
{
    if (payloadSize == FEATURE_FLAG_MAX_SIZE) {
        union FeatureFlagsUnion feature{};
        if (memcpy_s(&feature, sizeof(feature), payload, payloadSize) != EOK) {
            WRITE_LOG(LOG_FATAL, "CheckFeatures memcpy_s failed");
            return false;
        }
        WRITE_LOG(LOG_DEBUG, "isStableBuf:%d, hugeBuf:%d", isStableBuf, feature.bits.hugeBuf);
        context->isStableBufSize = isStableBuf ? true : (!feature.bits.hugeBuf);
        return true;
    } else if (payloadSize == 0) {
        WRITE_LOG(LOG_DEBUG, "FileBegin CheckFeatures payloadSize:%d, use default feature.", payloadSize);
        context->isStableBufSize = true;
        return true;
    } else {
        WRITE_LOG(LOG_FATAL, "CheckFeatures payloadSize:%d", payloadSize);
        return false;
    }
}
}  // namespace Hdc
