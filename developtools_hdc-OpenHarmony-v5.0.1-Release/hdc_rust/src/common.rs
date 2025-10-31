/*
 * Copyright (C) 2023 Huawei Device Co., Ltd.
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
//! common

pub mod base;
pub mod filemanager;
pub mod forward;
pub mod hdcfile;
pub mod hdctransfer;
#[cfg(not(feature = "host"))]
pub mod jdwp;
pub mod sendmsg;
pub mod taskbase;
#[cfg(not(target_os = "windows"))]
pub mod uds;
pub mod unittest;
