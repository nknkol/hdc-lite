/*
 * Copyright (C) 2024 Huawei Device Co., Ltd.
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
#include "cmd.h"
#include "log.h"
#include "base.h"
#include "usb_util.h"
#if defined(SURPPORT_SELINUX)
#include "selinux/selinux.h"
#endif
#include "parameter.h"
#include <string>
#include <cstdio>
#include <cerrno>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#include "sys_para.h"

namespace Hdc {
using namespace std;

static bool DropRootPrivileges()
{
    int ret;
    const char *userName = "shell";
    vector<const char *> groupsNames = { "shell", "log", "readproc", "file_manager", "user_data_rw" };
    struct passwd *user;
    gid_t *gids = nullptr;

    user = getpwnam(userName);
    if (user == nullptr) {
        WRITE_LOG(LOG_INFO, "getpwuid %s fail, %s", userName, strerror(errno));
        return false;
    }
    gids = static_cast<gid_t *>(calloc(groupsNames.size(), sizeof(gid_t)));
    if (gids == nullptr) {
        return false;
    }
    for (size_t i = 0; i < groupsNames.size(); i++) {
        struct group *group = getgrnam(groupsNames[i]);
        if (group == nullptr) {
            continue;
        }
        gids[i] = group->gr_gid;
    }
    ret = setuid(user->pw_uid);
    if (ret) {
        WRITE_LOG(LOG_WARN, "setuid %s fail, %s", userName, strerror(errno));
        free(gids);
        return false;
    }
    ret = setgid(user->pw_gid);
    if (ret) {
        WRITE_LOG(LOG_WARN, "setgid %s fail, %s", userName, strerror(errno));
        free(gids);
        return false;
    }
    ret = setgroups(groupsNames.size(), gids);
    if (ret) {
        WRITE_LOG(LOG_WARN, "setgroups %s fail, %s", userName, strerror(errno));
        free(gids);
        return false;
    }
    free(gids);
#if defined(SURPPORT_SELINUX)
    if (setcon("u:r:hdcd:s0") != 0) {
        WRITE_LOG(LOG_WARN, "setcon fail, errno %s", strerror(errno));
        return false;
    }
#endif
    return true;
}

extern "C"  bool NeedDropRootPrivileges()
{
    string rootMode;
    string debugMode;
    GetDevItem("const.debuggable", debugMode);
    GetDevItem("persist.hdc.root", rootMode);
    WRITE_LOG(LOG_WARN, "debuggable:[%s]", debugMode.c_str());
    WRITE_LOG(LOG_WARN, "param root:[%s]", rootMode.c_str());
    if (debugMode != "1") {
        return DropRootPrivileges();
    }
    if (rootMode == "0") {
        return DropRootPrivileges();
    }
    WRITE_LOG(LOG_WARN, "will keep current privilege", rootMode.c_str());
    return true;
}
extern "C" void Restart()
{
    execl("/system/bin/hdcd", "hdcd", nullptr, nullptr);
}
}
