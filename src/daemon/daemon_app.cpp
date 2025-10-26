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
#include "daemon_app.h"
#include "decompress.h"

namespace Hdc {
HdcDaemonApp::HdcDaemonApp(HTaskInfo hTaskInfo)
    : HdcTransferBase(hTaskInfo)
{
    commandBegin = CMD_APP_BEGIN;
    commandData = CMD_APP_DATA;
    funcAppModFinish = nullptr;
}

HdcDaemonApp::~HdcDaemonApp()
{
    WRITE_LOG(LOG_INFO, "~HdcDaemonApp channelId:%u", taskInfo->channelId);
}

bool HdcDaemonApp::ReadyForRelease()
{
    if (!HdcTaskBase::ReadyForRelease()) {
        WRITE_LOG(LOG_WARN, "HdcTaskBase not ready for release channelId:%u", taskInfo->channelId);
        return false;
    }
    if (!asyncCommand.ReadyForRelease()) {
        WRITE_LOG(LOG_WARN, "asyncCommand not ready for release channelId:%u", taskInfo->channelId);
        return false;
    }
    WRITE_LOG(LOG_INFO, "ReadyForRelease channelId:%u", taskInfo->channelId);
    return true;
}

void HdcDaemonApp::MakeCtxForAppCheck(uint8_t *payload, const int payloadSize)
{
    string dstPath = "/data/local/tmp/";
    string bufString(reinterpret_cast<char *>(payload), payloadSize);
    SerialStruct::ParseFromString(ctxNow.transferConfig, bufString);
    // update transferconfig to main context
    ctxNow.master = false;
#ifdef HDC_PCDEBUG
    char tmpPath[256] = "";
    size_t size = 256;
    uv_os_tmpdir(tmpPath, &size);
    dstPath = tmpPath;
    dstPath += Base::GetPathSep();
#endif
    dstPath += ctxNow.transferConfig.optionalName;
    ctxNow.localPath = dstPath;
    ctxNow.transferBegin = Base::GetRuntimeMSec();
    ctxNow.fileSize = ctxNow.transferConfig.fileSize;
    return;
}

bool HdcDaemonApp::CommandDispatch(const uint16_t command, uint8_t *payload, const int payloadSize)
{
    if (!HdcTransferBase::CommandDispatch(command, payload, payloadSize)) {
        WRITE_LOG(LOG_WARN, "HdcTransferBase::CommandDispatch false command:%u channelId:%u",
            command, taskInfo->channelId);
        return false;
    }
    bool ret = true;
    switch (command) {
        case CMD_APP_CHECK: {
            uv_fs_t *openReq = new uv_fs_t;
            if (openReq == nullptr) {
                WRITE_LOG(LOG_FATAL, "HdcDaemonApp::CommandDispatch new uv_fs_t failed");
                return false;
            }
            (void)memset_s(openReq, sizeof(uv_fs_t), 0, sizeof(uv_fs_t));
            MakeCtxForAppCheck(payload, payloadSize);
            openReq->data = &ctxNow;
            ++refCount;
            WRITE_LOG(LOG_INFO, "CMD_APP_CHECK cid:%u sid:%u uv_fs_open local:%s remote:%s",
                taskInfo->channelId, taskInfo->sessionId, Hdc::MaskString(ctxNow.localPath).c_str(),
                Hdc::MaskString(ctxNow.remotePath).c_str());
            uv_fs_open(loopTask, openReq, ctxNow.localPath.c_str(),
                       UV_FS_O_TRUNC | UV_FS_O_CREAT | UV_FS_O_WRONLY, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH,
                       OnFileOpen);
            break;
        }
        case CMD_APP_UNINSTALL: {
            // This maybe has a command implanting risk, since it is a controllable device, it can be ignored
            string bufString(reinterpret_cast<char *>(payload), payloadSize);
            string options = "";
            string packages = "";
            vector<string> segments;
            Base::SplitString(bufString, " ", segments);
            for (auto seg: segments) {
                if (seg[0] == '-') {
                    options += " " + seg;
                } else {
                    packages += " " + seg;
                }
            }
            PackageShell(false, options.c_str(), packages);
            break;
        }
        default:
            break;
    }
    return ret;
};

bool HdcDaemonApp::AsyncInstallFinish(bool finish, int64_t exitStatus, const string result)
{
    if (mode == APPMOD_INSTALL) {
        unlink(ctxNow.localPath.c_str());
        string::size_type rindex = ctxNow.localPath.rfind(".tar");
        if (rindex != string::npos) {
            string dir = ctxNow.localPath.substr(0, rindex);
            RemovePath(dir);
            WRITE_LOG(LOG_DEBUG, "RemovePath dir:%s", dir.c_str());
        }
    }
    asyncCommand.DoRelease();
    string echo = result;
    echo = Base::ReplaceAll(echo, "\n", " ");
    vector<uint8_t> vecBuf;
    vecBuf.push_back(mode);
    vecBuf.push_back(exitStatus == 0);
    vecBuf.insert(vecBuf.end(), (uint8_t *)echo.c_str(), (uint8_t *)echo.c_str() + echo.size());
    SendToAnother(CMD_APP_FINISH, vecBuf.data(), vecBuf.size());
    --refCount;
#ifdef UNIT_TEST
    Base::WriteBinFile((UT_TMP_PATH + "/appinstall.result").c_str(), (uint8_t *)MESSAGE_SUCCESS.c_str(),
                       MESSAGE_SUCCESS.size(), true);
#endif
    return true;
}

void HdcDaemonApp::PackageShell(bool installOrUninstall, const char *options, const string package)
{
    ++refCount;
    // asynccmd Other processes, no RunningProtect protection
    chmod(package.c_str(), 0755);
    string doBuf;
    string opts = string(options);
    if (installOrUninstall) { // either -p or -s is always required in install
        if (opts.find("p") == string::npos && opts.find("s") == string::npos) {
            // basic mode: blank options or both "-s" / "-p" are omitted
            // eg. hdc install x.hap --> bm install -p x.hap
            // eg. hdc install -r x.hap --> bm install -r -p x.hap
            doBuf = Base::StringFormat("bm install %s -p %s", options, package.c_str());
        } else {
            // advansed mode for -p/-r/-s and some other options in the future
            doBuf = Base::StringFormat("bm install %s %s", options, package.c_str());
        }
    } else {  // -n is always required in uninstall
        if (opts.find("n") == string::npos) {
            // basic mode: blank options or "-n" is omitted
            // eg. hdc uninstall com.xx.xx --> bm uninstall -n com.xx.xx
            // eg. hdc uninstall -s com.xx.xx --> bm uninstall -s -n com.xx.xx
            doBuf = Base::StringFormat("bm uninstall %s -n %s", options, package.c_str());
        } else {
            // advansed mode for -s/-n and some other options in the future
            doBuf = Base::StringFormat("bm uninstall %s %s", options, package.c_str());
        }
    }

    funcAppModFinish = [this](bool finish, int64_t exitStatus, const string result) -> bool {
        return this->AsyncInstallFinish(finish, exitStatus, result);
    };
    if (installOrUninstall) {
        mode = APPMOD_INSTALL;
    } else {
        mode = APPMOD_UNINSTALL;
    }
    asyncCommand.Initial(loopTask, funcAppModFinish, AsyncCmd::OPTION_COMMAND_ONETIME);
    asyncCommand.ExecuteCommand(doBuf);
}

void HdcDaemonApp::Sideload(const char *pathOTA)
{
    mode = APPMOD_SIDELOAD;
    LogMsg(MSG_OK, "[placeholders] sideload %s", pathOTA);
    TaskFinish();
    unlink(pathOTA);
}

string HdcDaemonApp::Tar2Dir(const char *path)
{
    string dir;
    string tarpath = path;
    string::size_type rindex = tarpath.rfind(".tar");
    if (rindex != string::npos) {
        dir = tarpath.substr(0, rindex) + Base::GetPathSep();
        WRITE_LOG(LOG_DEBUG, "path:%s dir:%s", path, dir.c_str());
        Decompress dc(tarpath);
        dc.DecompressToLocal(dir);
    }
    return dir;
}

int HdcDaemonApp::RemoveDir(const string &dir)
{
    DIR *pdir = opendir(dir.c_str());
    if (pdir == nullptr) {
        WRITE_LOG(LOG_FATAL, "opendir failed dir:%s", dir.c_str());
        return -1;
    }
    struct dirent *ent;
    struct stat st;
    while ((ent = readdir(pdir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        std::string subpath = dir + Base::GetPathSep() + ent->d_name;
        if (lstat(subpath.c_str(), &st) == -1) {
            WRITE_LOG(LOG_WARN, "lstat failed subpath:%s", subpath.c_str());
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (RemoveDir(subpath) == -1) {
                closedir(pdir);
                return -1;
            }
            rmdir(subpath.c_str());
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            unlink(subpath.c_str());
        } else {
            WRITE_LOG(LOG_DEBUG, "lstat st_mode:%07o subpath:%s", st.st_mode, subpath.c_str());
        }
    }
    if (rmdir(dir.c_str()) == -1) {
        closedir(pdir);
        return -1;
    }
    closedir(pdir);
    return 0;
}

void HdcDaemonApp::RemovePath(const string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) == -1) {
        WRITE_LOG(LOG_WARN, "lstat failed path:%s", path.c_str());
        return;
    }
    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        unlink(path.c_str());
    } else if (S_ISDIR(st.st_mode)) {
        if (path == "." || path == "..") {
            return;
        }
        int rc = RemoveDir(path);
        WRITE_LOG(LOG_INFO, "RemoveDir rc:%d path:%s", rc, path.c_str());
    }
}

void HdcDaemonApp::WhenTransferFinish(CtxFile *context)
{
    if (context->lastErrno > 0) {
        constexpr int bufSize = 1024;
        char buf[bufSize] = { 0 };
        uv_strerror_r(static_cast<int>(-context->lastErrno), buf, bufSize);
        WRITE_LOG(LOG_DEBUG, "HdcDaemonApp WhenTransferFinish with errno:%d", context->lastErrno);
        LogMsg(MSG_FAIL, "Transfer App at:%lld/%lld(Bytes), Reason: %s",
               context->indexIO, context->fileSize, buf);
        return;
    }
    if (ctxNow.transferConfig.functionName == CMDSTR_APP_SIDELOAD) {
        Sideload(context->localPath.c_str());
    } else if (ctxNow.transferConfig.functionName == CMDSTR_APP_INSTALL) {
        string dir = Tar2Dir(context->localPath.c_str());
        if (!dir.empty()) {
            PackageShell(true, context->transferConfig.options.c_str(), dir.c_str());
        } else {
            PackageShell(true, context->transferConfig.options.c_str(), context->localPath.c_str());
        }
    } else {
    }
};
}
