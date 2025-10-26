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
#include "usb.h"

namespace Hdc {
HdcUSBBase::HdcUSBBase(const bool serverOrDaemonIn, void *ptrMainBase)
{
    serverOrDaemon = serverOrDaemonIn;
    clsMainBase = ptrMainBase;
    modRunning = true;
}

HdcUSBBase::~HdcUSBBase()
{
}

void HdcUSBBase::ReadUSB(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    StartTraceScope("HdcUSBBase::ReadUSB");
    HSession hSession = (HSession)stream->data;
    HdcSessionBase *hSessionBase = (HdcSessionBase *)hSession->classInstance;
    CALLSTAT_GUARD(hSession->childLoopStatus, stream->loop, "HdcUSBBase::ReadUSB");
    if (hSessionBase->FetchIOBuf(hSession, hSession->ioBuf, nread) < 0) {
        WRITE_LOG(LOG_FATAL, "ReadUSB FetchIOBuf error sessionId:%u", hSession->sessionId);
        hSessionBase->FreeSession(hSession->sessionId);
    }
}

bool HdcUSBBase::ReadyForWorkThread(HSession hSession)
{
    // Server-end USB IO is handed over to each sub-thread, only the daemon is still read by the main IO to distribute
    // to each sub-thread by DataPipe.
    uv_tcp_t *stream = &hSession->dataPipe[STREAM_WORK];
    if (uv_tcp_init(&hSession->childLoop, stream) ||
        uv_tcp_open(stream, hSession->dataFd[STREAM_WORK])) {
        WRITE_LOG(LOG_FATAL, "USBBase ReadyForWorkThread init child TCP failed, sid:%u", hSession->sessionId);
        return false;
    }
    stream->data = hSession;
    HdcSessionBase *pSession = (HdcSessionBase *)hSession->classInstance;
#ifdef HDC_HOST
    Base::SetTcpOptions(stream, HOST_SOCKETPAIR_SIZE);
#else
    Base::SetTcpOptions(stream);
#endif
    if (uv_read_start((uv_stream_t *)stream, pSession->AllocCallback, ReadUSB)) {
        WRITE_LOG(LOG_FATAL, "USBBase ReadyForWorkThread child TCP read failed, sid:%u", hSession->sessionId);
        return false;
    }
    WRITE_LOG(LOG_INFO, "USBBase ReadyForWorkThread finish dataFd[STREAM_WORK]:%d, sid:%u",
        hSession->dataFd[STREAM_WORK], hSession->sessionId);
    return true;
};

vector<uint8_t> HdcUSBBase::BuildPacketHeader(uint32_t sessionId, uint8_t option, uint32_t dataSize)
{
    vector<uint8_t> vecData;
    USBHead head;
    head.sessionId = htonl(sessionId);
    for (size_t i = 0; i < sizeof(head.flag); i++) {
        head.flag[i] = USB_PACKET_FLAG.data()[i];
    }
    head.option = option;
    head.dataSize = htonl(dataSize);
    vecData.insert(vecData.end(), (uint8_t *)&head, (uint8_t *)&head + sizeof(USBHead));
    return vecData;
}

// USB big data stream, block transmission, mainly to prevent accidental data packets from writing through EP port,
// inserting the send queue causes the program to crash
int HdcUSBBase::SendUSBBlock(HSession hSession, uint8_t *data, const int length)
{
    int childRet = 0;
    int ret = ERR_IO_FAIL;
    StartTraceScope("HdcUSBBase::SendUSBBlock");
    std::lock_guard<std::mutex> lock(hSession->hUSB->lockSendUsbBlock);
    auto header = BuildPacketHeader(hSession->sessionId, USB_OPTION_HEADER, length);
    do {
        if ((SendUSBRaw(hSession, header.data(), header.size())) <= 0) {
            WRITE_LOG(LOG_FATAL, "SendUSBRaw index failed");
            break;
        }
        if ((childRet = SendUSBRaw(hSession, data, length)) <= 0) {
            WRITE_LOG(LOG_FATAL, "SendUSBRaw body failed");
            break;
        }
        if (childRet > 0 && (childRet % hSession->hUSB->wMaxPacketSizeSend == 0)) {
            // win32 send ZLP will block winusb driver and LIBUSB_TRANSFER_ADD_ZERO_PACKET not effect
            // so, we send dummy packet to prevent zero packet generate
            auto dummy = BuildPacketHeader(hSession->sessionId, 0, 0);
            if ((SendUSBRaw(hSession, dummy.data(), dummy.size())) <= 0) {
                WRITE_LOG(LOG_FATAL, "SendUSBRaw dummy failed");
                break;
            }
        }
        ret = length;
    } while (false);
    return ret;
}

bool HdcUSBBase::IsUsbPacketHeader(uint8_t *ioBuf, int ioBytes)
{
    StartTraceScope("HdcUSBBase::IsUsbPacketHeader");
    USBHead *usbPayloadHeader = reinterpret_cast<struct USBHead *>(ioBuf);
    uint32_t maybeSize = ntohl(usbPayloadHeader->dataSize);
    bool isHeader = false;
    do {
        if (memcmp(usbPayloadHeader->flag, USB_PACKET_FLAG.c_str(), USB_PACKET_FLAG.size())) {
            break;
        }
        if (ioBytes != sizeof(USBHead)) {
            break;
        }
        if (maybeSize == 0) {
            isHeader = true;  // nop packet
            break;
        } else {  // maybeSize != 0
            if (usbPayloadHeader->option & USB_OPTION_HEADER) {
                isHeader = true;
                break;
            }
        }
    } while (false);
    return isHeader;
}

void HdcUSBBase::PreSendUsbSoftReset(HSession hSession, uint32_t sessionIdOld)
{
    StartTraceScope("HdcUSBBase::PreSendUsbSoftReset");
    HUSB hUSB = hSession->hUSB;
    if (hSession->serverOrDaemon && !hUSB->resetIO) {
        hUSB->lockSendUsbBlock.lock();
        WRITE_LOG(LOG_WARN, "SendToHdcStream check, sessionId not matched");
        auto header = BuildPacketHeader(sessionIdOld, USB_OPTION_RESET, 0);
        if (SendUSBRaw(hSession, header.data(), header.size()) <= 0) {
            WRITE_LOG(LOG_FATAL, "PreSendUsbSoftReset send failed");
        }
        hUSB->lockSendUsbBlock.unlock();
        hUSB->resetIO = true;
    }
}

int HdcUSBBase::CheckPacketOption(HSession hSession, uint8_t *appendData, int dataSize)
{
    HUSB hUSB = hSession->hUSB;
    // special short packet
    USBHead *header = reinterpret_cast<USBHead *>(appendData);
    header->sessionId = ntohl(header->sessionId);
    header->dataSize = ntohl(header->dataSize);
    if (header->sessionId != hSession->sessionId) {
        // Only server do it here, daemon 'SendUsbSoftReset' no use
        // hilog + ctrl^C to reproduction scene
        //
        // Because the USB-reset API does not work on all platforms, the last session IO data may be
        // recveived, we need to ignore it.
        WRITE_LOG(LOG_WARN, "CheckPacketOption softreset header->sessionId:%u sessionId:%u",
            header->sessionId, hSession->sessionId);
        PreSendUsbSoftReset(hSession, header->sessionId);
        return 0;
    }
    if (header->option & USB_OPTION_HEADER) {
        // header packet
        hUSB->payloadSize = header->dataSize;
    }
    // soft ZLP
    return hUSB->payloadSize;
}

// return value: <0 error; = 0 all finish; >0 need size
int HdcUSBBase::SendToHdcStream(HSession hSession, uv_stream_t *stream, uint8_t *appendData, int dataSize)
{
    StartTraceScope("HdcUSBBase::SendToHdcStream");
    int childRet = 0;
    HUSB hUSB = hSession->hUSB;
    if (IsUsbPacketHeader(appendData, dataSize)) {
        return CheckPacketOption(hSession, appendData, dataSize);
    }
    if (hUSB->payloadSize <= static_cast<uint32_t>(childRet)) {
        // last session data
        WRITE_LOG(LOG_WARN, "SendToHdcStream softreset dataSize:%d payloadSize:%u childRet:%d",
            dataSize, hUSB->payloadSize, childRet);
        PreSendUsbSoftReset(hSession, 0);  // 0 == reset current
        return 0;
    }
    if ((childRet = UsbToHdcProtocol(stream, appendData, dataSize)) < 0) {
        WRITE_LOG(LOG_FATAL, "Error usb send to stream dataSize:%d", dataSize);
        return ERR_IO_FAIL;
    }
    hUSB->payloadSize -= childRet;
    return hUSB->payloadSize;
}

}
