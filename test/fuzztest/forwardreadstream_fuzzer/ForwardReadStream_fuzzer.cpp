/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
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

#include "ForwardReadStream_fuzzer.h"
#include <uv.h>
#include "forward.h"

namespace Hdc {

bool FuzzForwardReadStream(const uint8_t *data, size_t size)
{
    if (size <= 0) {
        return true;
    }
    uv_loop_t loop;
    uv_loop_init(&loop);
    LoopStatus ls(&loop, "not support");
    HTaskInfo hTaskInfo = new TaskInformation();
    hTaskInfo->runLoopStatus = &ls;
    HdcSessionBase *daemon = new HdcSessionBase(false);
    hTaskInfo->ownerSessionClass = daemon;
    HdcForwardBase *forward = new(std::nothrow) HdcForwardBase(hTaskInfo);
    if (forward == nullptr) {
        delete daemon;
        delete hTaskInfo;
        WRITE_LOG(LOG_FATAL, "FuzzForwardReadStream forward is null");
        return false;
    }
    HdcForwardBase::HCtxForward ctx = (HdcForwardBase::HCtxForward)forward->MallocContext(true);
    if (ctx == nullptr) {
        delete daemon;
        delete forward;
        delete hTaskInfo;
        WRITE_LOG(LOG_FATAL, "FuzzForwardReadStream ctx is null");
        return false;
    }
    uv_stream_t *stream = (uv_stream_t *)&ctx->tcp;
    uint8_t *base = new uint8_t[size];
    if (memcpy_s(base, size, const_cast<uint8_t *>(data), size) == EOK) {
        uv_buf_t rbf = uv_buf_init(reinterpret_cast<char *>(base), size);
        forward->ReadForwardBuf(stream, (ssize_t)size, &rbf);
    } else {
        delete[] base;
    }
    delete ctx;
    delete daemon;
    delete forward;
    delete hTaskInfo;
    uv_stop(&loop);
    uv_loop_close(&loop);
    return true;
}
} // namespace Hdc

/* Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Run your code on data */
    Hdc::FuzzForwardReadStream(data, size);
    return 0;
}
