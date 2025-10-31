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
#ifndef HDC_DEFINE_H
#define HDC_DEFINE_H
#include "define_plus.h"

namespace Hdc {
// ############################## config #######################################
#ifndef USE_CONFIG_UV_THREADS
constexpr size_t SIZE_THREAD_POOL = 16;
#endif
constexpr size_t SIZE_THREAD_POOL_MIN = 16;
constexpr size_t SIZE_THREAD_POOL_MAX = 256;
constexpr uint8_t GLOBAL_TIMEOUT = 30;
constexpr uint16_t DEFAULT_PORT = 8710;
constexpr uint16_t MAX_LOG_FILE_COUNT = 30;
constexpr bool ENABLE_IO_CHECKSUM = false;
const string IPV4_MAPPING_PREFIX = "::ffff:";
const string DEFAULT_SERVER_ADDR_IP = "::ffff:127.0.0.1";
const string DEFAULT_SERVER_ADDR = "::ffff:127.0.0.1:8710";
const string ENV_SERVER_PORT = "OHOS_HDC_SERVER_PORT";
const string ENV_SERVER_LOG = "OHOS_HDC_LOG_LEVEL";

// ################################ macro define ###################################
constexpr uint8_t MINOR_TIMEOUT = 5;
constexpr uint8_t MILL_SECONDS = 10;
constexpr uint8_t WAIT_SECONDS = 30;
constexpr uint8_t DWORD_SERIALIZE_SIZE = 4;
constexpr uint8_t OFFSET = 2;
constexpr uint8_t CMD_ARG1_COUNT = 2;
constexpr uint8_t COUNT = 10;
constexpr uint8_t STREAM_MAIN = 0;  // work at main thread
constexpr uint8_t STREAM_WORK = 1;  // work at work thread
constexpr uint8_t STREAM_SIZE = 2;
constexpr uint8_t FEATURE_FLAG_MAX_SIZE = 8;
constexpr uint16_t TIME_BUF_SIZE = 32;
constexpr uint16_t BUF_SIZE_MICRO = 16;
constexpr uint16_t BUF_SIZE_TINY = 64;
constexpr uint16_t BUF_SIZE_SMALL = 256;
constexpr uint16_t BUF_SIZE_MEDIUM = 512;
constexpr uint16_t BUF_SIZE_DEFAULT = 1024;
constexpr uint16_t BUF_SIZE_DEFAULT2 = BUF_SIZE_DEFAULT * 2;
constexpr uint16_t BUF_SIZE_DEFAULT4 = BUF_SIZE_DEFAULT * 4;
constexpr uint16_t MAX_IP_PORT = 65535;
constexpr uint16_t MAX_CONNECTKEY_SIZE = 32;  // usb sn/tcp ipport
constexpr uint16_t TIME_BASE = 1000;          // time unit conversion base value
constexpr uint16_t UV_DEFAULT_INTERVAL = 250;  // ms
constexpr uint16_t VER_PROTOCOL = 0x01;
constexpr uint16_t MAX_PACKET_SIZE_HISPEED = 512;
constexpr uint16_t DEVICE_CHECK_INTERVAL = 3000;  // ms
constexpr uint32_t MAX_SIZE_IOBUF = 511 * 1024; // 511KB, large file transfer speed up
constexpr uint32_t MAX_USBFFS_BULK = 512 * 1024; // 512KB, large file transfer speed up
constexpr uint32_t MAX_SIZE_IOBUF_STABLE = 60 * 1024; // 60KB, compatible with previous version
constexpr uint32_t MAX_USBFFS_BULK_STABLE = 61 * 1024; // 61KB, compatible with provious version
constexpr double DECODE_SCALE = 0.75;
constexpr uint16_t LAST_EQUAL_NUM = 2;
constexpr uint16_t UV_START_TIMEOUT = 10;
constexpr uint16_t UV_START_REPEAT = 10;
constexpr uint16_t UV_LISTEN_LBACKOG = 4;
constexpr uint16_t BUF_EXTEND_SIZE = 4;
constexpr uint16_t BUF_MULTIPLE = 2;
constexpr uint16_t EXPECTED_LEN = 9;
constexpr uint16_t MAX_APP_PARAM_SIZE = 512;
constexpr uint16_t MIN_APP_PARAM_SIZE = 4;
constexpr uint16_t FORWORD_PORT_RM_BUF_SIZE = 9;
constexpr uint16_t FORWORD_PORT_OTHER_BUF_SIZE = 6;
constexpr uint16_t CMD_REMOTE_SIZE = 8;
constexpr uint16_t PORT_MAX_LEN = 5;
constexpr uint16_t START_SERVER_FOR_CLIENT_TIME = 300;
constexpr uint16_t CMD_FILE_PENULT_PARAM = 2;
#ifdef HDC_HOST
constexpr uint16_t MAX_DELETED_SESSION_ID_RECORD_COUNT = 32;
#else
constexpr uint16_t MAX_DELETED_SESSION_ID_RECORD_COUNT = 10;
#endif
constexpr uint16_t TCP_CONNECT_MAX_RETRY_COUNT = 6;
constexpr uint16_t TCP_CONNECT_RETRY_TIME_MS = 500;
constexpr uint16_t NEW_SESSION_DROP_USB_DATA_TIME_MS = 1000;

// double-word(hex)=[0]major[1][2]minor[3][4]version[5]fix(a-p)[6][7]reserve
// |----------------------------------------------------------------|
// | 31-28 | 27-24 | 23-20 | 19-16 | 15-12 | 11-08 |     07-00      |
// |----------------------------------------------------------------|
// | major |reserve| minor |reserve|version|  fix  |   reserve      |
// |----------------------------------------------------------------|
// 0x30100100 is 3.1.0b
constexpr uint32_t HDC_VERSION_NUMBER = 0x30100100;
constexpr uint32_t HDC_BUF_MAX_BYTES = INT_MAX;
#ifdef HDC_HOST
constexpr uint32_t HOST_SOCKETPAIR_SIZE = 1024 * 1024;
#endif
constexpr uint32_t HDC_SOCKETPAIR_SIZE = MAX_SIZE_IOBUF * 2;
// "\f" asicc is 12
const string HDC_HOST_DAEMON_BUF_SEPARATOR = "\f";
constexpr int32_t RSA_KEY_BITS = 3072;

const string WHITE_SPACES = " \t\n\r";
const string UT_TMP_PATH = "/tmp/hdc-ut";
const string LOG_FILE_NAME = "hdc.log";
const string LOG_FILE_NAME_PREFIX = "hdc-";
const string LOG_CACHE_NAME = ".hdc.cache.log";
constexpr uint64_t LOG_FILE_MAX_SIZE = 104857600;
const string SERVER_NAME = "HDCServer";
const string STRING_EMPTY = "";
const string HANDSHAKE_MESSAGE = "OHOS HDC";  // sep not char '-', not more than 11 bytes
const string HANDSHAKE_FAILED = "HS FAILED";
const string PACKET_FLAG = "HW";              // must 2bytes
const string EMPTY_ECHO = "[Empty]";
const string MESSAGE_INFO = "[Info]";
const string MESSAGE_FAIL = "[Fail]";
const string MESSAGE_SUCCESS = "[Success]";
const char HUGE_BUF_TAG = 'H';                // support huge buffer
const size_t BANNER_FEATURE_TAG_OFFSET = 11;
const char WAIT_DEVICE_TAG = 'W';
const size_t WAIT_TAG_OFFSET = 11;
// input command
const string CMDSTR_SOFTWARE_VERSION = "version";
const string CMDSTR_SOFTWARE_HELP = "help";
const string CMDSTR_TARGET_DISCOVER = "discover";
const string CMDSTR_SERVICE_START = "start";
const string CMDSTR_SERVICE_KILL = "kill";
const string CMDSTR_GENERATE_KEY = "keygen";
const string CMDSTR_LIST_TARGETS = "list targets";
const string CMDSTR_CHECK_SERVER = "checkserver";
const string CMDSTR_CHECK_DEVICE = "checkdevice";
const string CMDSTR_WAIT_FOR = "wait";
const string CMDSTR_CONNECT_TARGET = "tconn";
const string CMDSTR_CONNECT_ANY = "any";
const string CMDSTR_SHELL = "shell";
const string CMDSTR_TARGET_REBOOT = "target boot";
const string CMDSTR_TARGET_MOUNT = "target mount";
const string CMDSTR_STARTUP_MODE = "smode";
const string CMDSTR_TARGET_MODE = "tmode";
const string CMDSTR_BUGREPORT = "bugreport";
const string CMDSTR_HILOG = "hilog";
const string CMDSTR_TMODE_USB = "usb";
#ifdef _WIN32
const string HILOG_NAME = "hilog.exe";
const string SPLIT = "\\";
#endif
// #ifdef HDC_SUPPORT_UART
// const string CMDSTR_TMODE_UART = "uart";
// const string UART_HDC_NODE = "/dev/ttyS4";
// const string CONSOLE_ACTIVE_NODE = "/sys/class/tty/console/active";
// constexpr int UART_IO_WAIT_TIME_100 = 100;
// constexpr int UART_IO_WAIT_TIME = 1000;
// constexpr uint16_t MAX_UART_SIZE_IOBUF = 4096; // MAX_SIZE_IOBUF;
// #endif
const string CMDSTR_TMODE_TCP = "tcp";
#ifdef HDC_EMULATOR
const string CMDSTR_TMODE_BRIDGE = "bridge";
#endif
const string CMDSTR_FILE_SEND = "file send";
const string CMDSTR_FILE_RECV = "file recv";
const string CMDSTR_REMOTE_PARAMETER = "remote";
const string CMDSTR_FORWARD_FPORT = "fport";
const string CMDSTR_FORWARD_RPORT = "rport";
const string CMDSTR_APP_INSTALL = "install";
const string CMDSTR_APP_UNINSTALL = "uninstall";
// const string CMDSTR_APP_SIDELOAD = "sideload";
const string CMDSTR_LIST_JDWP = "jpid";
const string CMDSTR_TRACK_JDWP = "track-jpid";
const string CMDSTR_INNER_ENABLE_KEEPALIVE = "alive";
const string CMDSTR_FLASHD_UPDATE = "update";
const string CMDSTR_FLASHD_FLASH = "flash";
const string CMDSTR_FLASHD_ERASE = "erase";
const string CMDSTR_FLASHD_FORMAT = "format";

// ################################ Error Message ###################################
const string TERMINAL_HDC_PROCESS_FAILED = "[E002101]:Terminal hdc process failed, "\
    "please terminal the hdc process in the task manager first.";

// ################################ Macro define ###################################
#ifdef IS_RELEASE_VERSION
#define WRITE_LOG(level, fmt, ...)   Base::PrintLogEx(__FUNCTION__, __LINE__, level, fmt, ##__VA_ARGS__)
#else
#define WRITE_LOG(level, fmt, ...)   Base::PrintLogEx(__FILE_NAME__, __LINE__, level, fmt, ##__VA_ARGS__)
#endif
#ifdef _WIN32
#define hdc_strerrno(buf) \
        char buf[1024] = { 0 }; \
        strerror_s(buf, 1024, errno)
#else
#define hdc_strerrno(buf)  \
        char buf[1024] = { 0 };  \
        strerror_r(errno, buf, 1024)
#endif
}  // namespace Hdc
#endif  // HDC_DEFINE_H
