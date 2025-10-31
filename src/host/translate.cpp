/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "translate.h"
// #include "host_updater.h"

namespace Hdc {
namespace TranslateCommand {
    string Usage()
    {
        string ret = "";
        ret = "\n                         OpenHarmony device connector(HDC) ...\n\n"
              "---------------------------------global commands:----------------------------------\n"
              " -h/help [verbose]                     - Print hdc help, 'verbose' for more other cmds\n"
              " -v/version                            - Print hdc version\n"
              " -t connectkey                         - Use device with given connect key\n"
              "\n"
              "---------------------------------component commands:-------------------------------\n"
              "session commands(on server):\n"
              " list targets [-v]                     - List all devices status, -v for detail\n"
              " start [-r]                            - Start server. If with '-r', will be restart server\n"
              " kill [-r]                             - Kill server. If with '-r', will be restart server\n"
              "\n"
              "service commands(on daemon):\n"
              " target mount                          - Set /system /vendor partition read-write\n"
              " wait                                  - Wait for the device to become available\n"
              " target boot [MODE]                    - Reboot the into MODE.\n"
              " smode [-r]                            - Restart daemon with root permissions, '-r' to cancel root\n"
              "                                         permissions\n"
              " tmode port [port]                     - Reboot the device, listening on TCP port\n"
              "\n"
              "---------------------------------task commands:-------------------------------------\n"
              "file commands:\n"
              " file send [option] local remote       - Send file to device\n"
              " file recv [option] remote local       - Recv file from device\n"
              "                                         option is -a|-s|-z\n"
              "                                         -a: hold target file timestamp\n"
              "                                         -sync: just update newer file\n"
              "                                         -z: compress transfer\n"
              "                                         -m: mode sync\n"
              "\n"
              "forward commands:\n"
              " fport localnode remotenode            - Forward local traffic to remote device\n"
              " rport remotenode localnode            - Reserve remote traffic to local host\n"
              "                                         node config name format 'schema:content'\n"
              "                                         examples are below:\n"
              "                                         tcp:<port>\n"
              "                                         localfilesystem:<unix domain socket name>\n"
              "                                         localreserved:<unix domain socket name>\n"
              "                                         localabstract:<unix domain socket name>\n"
              "                                         dev:<device name>\n"
              "                                         jdwp:<pid> (remote only)\n"
              " fport ls                              - Display forward/reverse tasks\n"
              " fport rm taskstr                      - Remove forward/reverse task by taskstring\n"
              "\n"
              "app commands:\n"
              " install [-r|-s] src                   - Send package(s) to device and install them\n"
              "                                         src examples: single or multiple packages and directories\n"
              "                                         (.hap .hsp)\n"
              "                                         -r: replace existing application\n"
              "                                         -s: install shared bundle for multi-apps\n"
              " uninstall [-k] [-s] package           - Remove application package from device\n"
              "                                         -k: keep the data and cache directories\n"
              "                                         -s: remove shared bundle\n"
              "\n"
              "debug commands:\n"
              " hilog [-h]                            - Show device log, -h for detail\n"
              " shell [COMMAND...]                    - Run shell command (interactive shell if no command given)\n"
              " bugreport [FILE]                      - Return all information from the device, stored in file if "
              "FILE is specified\n"
              " jpid                                  - List PIDs of processes hosting a JDWP transport\n"
              " track-jpid [-a|-p]                    - Track PIDs of debug processes hosting a JDWP transport\n"
              "                                         -a: include debug and release processes\n"
              "                                         -p: don't display debug and release tags\n"
              "\n"
              "security commands:\n"
              " keygen FILE                           - Generate public/private key; key stored in FILE and FILE.pub\n"
              "\n";
        return ret;
    }

    string Verbose()
    {
        string ret = "\n                         OpenHarmony device connector(HDC) ...\n\n"
            "---------------------------------global commands:----------------------------------\n"
            " -h/help [verbose]                     - Print hdc help, 'verbose' for more other cmds\n"
            " -v/version                            - Print hdc version\n"
            " -l[0-5]                               - Set runtime loglevel\n"
            " -t connectkey                         - Use device with given connect key\n"
            " checkserver                           - check client-server version\n"
            " checkdevice                           - check server-daemon version(only uart)\n"
            "\n"
            "---------------------------------component commands:-------------------------------\n"
            "session commands(on server):\n"
            " discover                              - Discover devices listening on TCP via LAN broadcast\n"
            " list targets [-v]                     - List all devices status, -v for detail\n"
            " tconn key                             - Connect device via key, TCP use ip:port\n"
            "                                         example:192.168.0.100:10178/192.168.0.100\n"
            "                                         TCP need to connect manually\n"
            " start [-r]                            - Start server. If with '-r', will be restart server\n"
            " kill [-r]                             - Kill server. If with '-r', will be restart server\n"
            " -s [ip:]port                          - Set hdc server listen config\n"
            "\n"
            "service commands(on daemon):\n"
            " target mount                          - Set /system /vendor partition read-write\n"
            " target boot [MODE]                    - Reboot the into MODE.\n"
            " smode [-r]                            - Restart daemon with root permissions, '-r' to cancel root\n"
            "                                         permissions\n"
            " tmode port [port]                     - Reboot the device, listening on TCP port\n"
            "\n"
            "---------------------------------task commands:-------------------------------------\n"
            "file commands:\n"
            " file send [option] local remote       - Send file to device\n"
            " file recv [option] remote local       - Recv file from device\n"
            "                                         option is -a|-s|-z\n"
            "                                         -a: hold target file timestamp\n"
            "                                         -sync: just update newer file\n"
            "                                         -z: compress transfer\n"
            "                                         -m: mode sync\n"
            "\n"
            "forward commands:\n"
            " fport localnode remotenode            - Forward local traffic to remote device\n"
            " rport remotenode localnode            - Reserve remote traffic to local host\n"
            "                                         node config name format 'schema:content'\n"
            "                                         examples are below:\n"
            "                                         tcp:<port>\n"
            "                                         localfilesystem:<unix domain socket name>\n"
            "                                         localreserved:<unix domain socket name>\n"
            "                                         localabstract:<unix domain socket name>\n"
            "                                         dev:<device name>\n"
            "                                         jdwp:<pid> (remote only)\n"
            " fport ls                              - Display forward/reverse tasks\n"
            " fport rm taskstr                      - Remove forward/reverse task by taskstring\n"
            "\n"
            "app commands:\n"
            " install [-r|-s] src                   - Send package(s) to device and install them\n"
            "                                         src examples: single or multiple packages and directories\n"
            "                                         (.hap .hsp)\n"
            "                                         -r: replace existing application\n"
            "                                         -s: install shared bundle for multi-apps\n"
            " uninstall [-k] [-s] package           - Remove application package from device\n"
            "                                         -k: keep the data and cache directories\n"
            "                                         -s: remove shared bundle\n"
            "\n"
            "debug commands:\n"
            " hilog [-h]                            - Show device log, -h for detail\n"
            " shell [COMMAND...]                    - Run shell command (interactive shell if no command given)\n"
            " bugreport [FILE]                      - Return all information from the device, stored in file if FILE "
            "is specified\n"
            " jpid                                  - List PIDs of processes hosting a JDWP transport\n"
            "\n"
            "security commands:\n"
            " keygen FILE                           - Generate public/private key; key stored in FILE and FILE.pub\n"
            "\n"
            "---------------------------------external commands:------------------------------------\n"
            "extconn key                             - Connect external device via key, TCP use ip:port(remian)\n"
            "-S [ip:]port                            - Set hdc external server listen config\n"
            "\n";
        return ret;
    }

    string TargetConnect(FormatCommand *outCmd)
    {
        string stringError;
        if (Base::StringEndsWith(outCmd->parameters, " -remove")) {
            outCmd->parameters = outCmd->parameters.substr(0, outCmd->parameters.size() - CMD_REMOTE_SIZE);
            outCmd->cmdFlag = CMD_KERNEL_TARGET_DISCONNECT;
        } else {
            outCmd->cmdFlag = CMD_KERNEL_TARGET_CONNECT;
            constexpr int maxKeyLength = 50; // 50: tcp max=21,USB max=8bytes, serial device name maybe long
            if (outCmd->parameters.size() > maxKeyLength) {
                stringError = "Error connect key's size";
                outCmd->bJumpDo = true;
            }
        }
        size_t pos = outCmd->parameters.find(":");
        if (pos != std::string::npos) {
            // tcp mode
            string ip = outCmd->parameters.substr(0, pos);
            string sport = outCmd->parameters.substr(pos + 1);
            if (sport.empty()) {
                stringError = "Port incorrect";
                outCmd->bJumpDo = true;
                return stringError;
            }
            if (ip == "localhost") {
                ip = "127.0.0.1";
                outCmd->parameters.replace(0, pos, ip);
            }
            int port = std::stoi(sport);
            sockaddr_in addr;
            if ((port <= 0 || port > MAX_IP_PORT) || uv_ip4_addr(ip.c_str(), port, &addr) < 0) {
                stringError = "IP:Port incorrect";
                outCmd->bJumpDo = true;
            }
        } else {
            // All wired modes (USB/UART) are removed, only TCP (ip:port) is allowed
            stringError = "Only TCP (ip:port) connections are supported.";
            outCmd->bJumpDo = true;
        }
        return stringError;
    }

    string ForwardPort(const char *input, FormatCommand *outCmd)
    {
        string stringError;
        const char *pExtra = input + 6;  // CMDSTR_FORWARD_FPORT CMDSTR_FORWARD_RPORT + " " size
        if (!strncmp(input, CMDSTR_FORWARD_FPORT.c_str(), CMDSTR_FORWARD_FPORT.size()) && !strcmp(pExtra, "ls")) {
            outCmd->cmdFlag = CMD_FORWARD_LIST;
        } else if (!strncmp(input, CMDSTR_FORWARD_FPORT.c_str(), CMDSTR_FORWARD_FPORT.size()) &&
            !strncmp(pExtra, "rm", 2)) { // 2: "rm" size
            outCmd->cmdFlag = CMD_FORWARD_REMOVE;
            if (strcmp(pExtra, "rm")) {
                outCmd->parameters = input + FORWORD_PORT_RM_BUF_SIZE;
            }
        } else {
            const char *p = input + FORWORD_PORT_OTHER_BUF_SIZE;
            // clang-format off
            if (strncmp(p, "tcp:", 4) && // 4: "tcp:" size
                strncmp(p, "localabstract:", 14) && // 14: "localabstract:" size
                strncmp(p, "localreserved:", 14) && // 14: "localreserved:" size
                strncmp(p, "localfilesystem:", 16) && // 16: "localfilesystem:" size
                strncmp(p, "dev:", 4) && // 4: "dev:" size
                strncmp(p, "jdwp:", 5) && // 5: "jdwp:" size
                strncmp(p, "ark:", 4)) { // 4: "ark:" size
                stringError = "Incorrect forward command";
                outCmd->bJumpDo = true;
            }
            // clang-format on
            outCmd->cmdFlag = CMD_FORWARD_INIT;
            outCmd->parameters = input;
        }
        return stringError;
    }

    string RunMode(const char *inputRaw, FormatCommand *outCmd)
    {
        string stringError;
        outCmd->cmdFlag = CMD_UNITY_RUNMODE;
        outCmd->parameters = inputRaw + CMDSTR_TARGET_MODE.size() + 1;  // with  ' '
        
        if (!strncmp(outCmd->parameters.c_str(), CMDSTR_TMODE_USB.c_str(), CMDSTR_TMODE_USB.size())) {
             stringError = "tmode usb is disabled in wireless-only mode.";
             outCmd->bJumpDo = true;
        } else if (!strncmp(outCmd->parameters.c_str(), "port", 4)) { // 4:"port"
            // 保留 tmode port 逻辑
            if (!strncmp(outCmd->parameters.c_str(), "port ", 5)) { // 5:"port "
                 const char *tmp = inputRaw + strlen("tmode port ");
                 if (strcmp(tmp, "close") == 0) {
                     return stringError;
                 }
                 int port = atoi(tmp);
                 if (port > MAX_IP_PORT || port <= 0) {
                     stringError = "Incorrect port range";
                     outCmd->bJumpDo = true;
                 }
            }
        } else {
             stringError = "Error tmode command";
             outCmd->bJumpDo = true;
        }
        return stringError;
    }

    void TargetReboot(const char *inputRaw, FormatCommand *outCmd)
    {
        outCmd->cmdFlag = CMD_UNITY_REBOOT;
        if (strcmp(inputRaw, CMDSTR_TARGET_REBOOT.c_str())) {
            outCmd->parameters = inputRaw + CMDSTR_TARGET_REBOOT.size() + 1;  // with  ' '
            if (outCmd->parameters == "-bootloader" || outCmd->parameters == "-recovery" ||
                outCmd->parameters == "-flashd") {
                // 阻止这些模式
                outCmd->bJumpDo = true;
            }
        }
    }

    // command input
    // client side:Enter string data formatting conversion to module see internal processing command
    string String2FormatCommand(const char *inputRaw, int sizeInputRaw, FormatCommand *outCmd)
    {
        string stringError;
        string input = string(inputRaw, sizeInputRaw);
        if (!strncmp(input.c_str(), CMDSTR_SOFTWARE_HELP.c_str(), CMDSTR_SOFTWARE_HELP.size())) {
            outCmd->cmdFlag = CMD_KERNEL_HELP;
            outCmd->bJumpDo = true;
            if (strstr(input.c_str(), " verbose")) {
                stringError = Verbose();
            } else {
                stringError = Usage();
            }
        } else if (!strcmp(input.c_str(), CMDSTR_SOFTWARE_VERSION.c_str())) {
            outCmd->cmdFlag = CMD_KERNEL_HELP;
            stringError = Base::GetVersion();
            outCmd->bJumpDo = true;
        } else if (!strcmp(input.c_str(), CMDSTR_TARGET_DISCOVER.c_str())) {
            outCmd->cmdFlag = CMD_KERNEL_TARGET_DISCOVER;
        } else if (!strncmp(input.c_str(), CMDSTR_LIST_TARGETS.c_str(), CMDSTR_LIST_TARGETS.size())) {
            outCmd->cmdFlag = CMD_KERNEL_TARGET_LIST;
            if (strstr(input.c_str(), " -v")) {
                outCmd->parameters = "v";
            }
        } else if (!strncmp(input.c_str(), CMDSTR_CHECK_SERVER.c_str(), CMDSTR_CHECK_SERVER.size())) {
            outCmd->cmdFlag = CMD_CHECK_SERVER;
        } else if (!strncmp(input.c_str(), CMDSTR_CHECK_DEVICE.c_str(), CMDSTR_CHECK_DEVICE.size())) {
            outCmd->parameters = input.c_str() + CMDSTR_CHECK_DEVICE.size() + 1;  // with ' '
            outCmd->cmdFlag = CMD_CHECK_DEVICE;
        } else if (!strncmp(input.c_str(), CMDSTR_WAIT_FOR.c_str(), CMDSTR_WAIT_FOR.size())) {
            outCmd->cmdFlag = CMD_WAIT_FOR;
        } else if (!strcmp(input.c_str(), CMDSTR_CONNECT_ANY.c_str())) {
            outCmd->cmdFlag = CMD_KERNEL_TARGET_ANY;
        } else if (!strncmp(input.c_str(), CMDSTR_CONNECT_TARGET.c_str(), CMDSTR_CONNECT_TARGET.size())) {
            outCmd->parameters = input.c_str() + CMDSTR_CONNECT_TARGET.size() + 1;  // with ' '
            stringError = TargetConnect(outCmd);
        } else if (!strncmp(input.c_str(), (CMDSTR_SHELL + " ").c_str(), CMDSTR_SHELL.size() + 1)) {
            outCmd->cmdFlag = CMD_UNITY_EXECUTE;
            outCmd->parameters = input.c_str() + CMDSTR_SHELL.size() + 1;
        } else if (!strcmp(input.c_str(), CMDSTR_SHELL.c_str())) {
            outCmd->cmdFlag = CMD_SHELL_INIT;
        } else if (!strncmp(input.c_str(), CMDSTR_FILE_SEND.c_str(), CMDSTR_FILE_SEND.size()) ||
                   !strncmp(input.c_str(), CMDSTR_FILE_RECV.c_str(), CMDSTR_FILE_RECV.size())) {
            outCmd->cmdFlag = CMD_FILE_INIT;
            outCmd->parameters = input.c_str() + strlen("file ");
        } else if (!strncmp(input.c_str(), string(CMDSTR_FORWARD_FPORT + " ").c_str(), CMDSTR_FORWARD_FPORT.size() + 1)
                   || !strncmp(input.c_str(), string(CMDSTR_FORWARD_RPORT + " ").c_str(),
                               CMDSTR_FORWARD_RPORT.size() + 1)) {
            stringError = ForwardPort(input.c_str(), outCmd);
        } else if (!strncmp(input.c_str(), CMDSTR_APP_INSTALL.c_str(), CMDSTR_APP_INSTALL.size())) {
            outCmd->cmdFlag = CMD_APP_INIT;
            outCmd->parameters = input;
        } else if (!strncmp(input.c_str(), CMDSTR_APP_UNINSTALL.c_str(), CMDSTR_APP_UNINSTALL.size())) {
            outCmd->cmdFlag = CMD_APP_UNINSTALL;
            outCmd->parameters = input;
            if (outCmd->parameters.size() > MAX_APP_PARAM_SIZE || outCmd->parameters.size() < MIN_APP_PARAM_SIZE) {
                stringError = "Package's path incorrect";
                outCmd->bJumpDo = true;
            }
        } else if (!strcmp(input.c_str(), CMDSTR_TARGET_MOUNT.c_str())) {
            outCmd->cmdFlag = CMD_UNITY_REMOUNT;
        } else if (!strcmp(input.c_str(), CMDSTR_LIST_JDWP.c_str())) {
            outCmd->cmdFlag = CMD_JDWP_LIST;
        } else if (!strncmp(input.c_str(), CMDSTR_TRACK_JDWP.c_str(), CMDSTR_TRACK_JDWP.size())) {
            outCmd->cmdFlag = CMD_JDWP_TRACK;
            if (strstr(input.c_str(), " -p")) {
                outCmd->parameters = "p";
            } else if (strstr(input.c_str(), " -a")) {
                outCmd->parameters = "a";
            }
        } else if (!strncmp(input.c_str(), CMDSTR_TARGET_REBOOT.c_str(), CMDSTR_TARGET_REBOOT.size())) {
            TargetReboot(inputRaw, outCmd);
            if (outCmd->bJumpDo) {
                 stringError = "Booting to bootloader/recovery/flashd is disabled in wireless-only mode.";
            }
        } else if (!strncmp(input.c_str(), "sideload", 8)) {
            stringError = "sideload is disabled in wireless-only mode.";
            outCmd->bJumpDo = true;
        } else if (!strncmp(input.c_str(), CMDSTR_TARGET_MODE.c_str(), CMDSTR_TARGET_MODE.size())) {
            stringError = RunMode(inputRaw, outCmd);
        } else if (!strncmp(input.c_str(), CMDSTR_HILOG.c_str(), CMDSTR_HILOG.size())) {
            outCmd->cmdFlag = CMD_UNITY_HILOG;
            if (strstr(input.c_str(), " -h")) {
                outCmd->parameters = "h";
            }
        } else if (!strncmp(input.c_str(), CMDSTR_STARTUP_MODE.c_str(), CMDSTR_STARTUP_MODE.size())) {
            outCmd->cmdFlag = CMD_UNITY_ROOTRUN;
            if (strstr(input.c_str(), " -r")) {
                outCmd->parameters = "r";
            }
        } else if (!strncmp(input.c_str(), CMDSTR_BUGREPORT.c_str(), CMDSTR_BUGREPORT.size())) {
            outCmd->cmdFlag = CMD_UNITY_BUGREPORT_INIT;
            outCmd->parameters = input;
            if (outCmd->parameters.size() == CMDSTR_BUGREPORT.size() + 1) { // 1 is sizeInputRaw = input.size() + 1
                outCmd->parameters = CMDSTR_BUGREPORT + " ";
            }
        }
        // Inner command, protocol uses only
        else if (!strncmp(input.c_str(), CMDSTR_INNER_ENABLE_KEEPALIVE.c_str(), CMDSTR_INNER_ENABLE_KEEPALIVE.size())) {
            outCmd->cmdFlag = CMD_KERNEL_ENABLE_KEEPALIVE;
        }
        // } else if (HostUpdater::CheckMatchUpdate(input, *outCmd)) { // -- 移除刷机相关命令
        //     outCmd->parameters = input;
        // } 
        else {
            stringError = "Unknown command...";
            outCmd->bJumpDo = true;
        }
#ifdef HDC_DEBUG
        WRITE_LOG(LOG_DEBUG, "String2FormatCommand cmdFlag:%d", outCmd->cmdFlag);
#endif
        // nl
        if (stringError.size()) {
            stringError += "\n";
        }
        return stringError;
    };
}
}  // namespace Hdc