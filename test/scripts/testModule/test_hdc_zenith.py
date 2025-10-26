#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright (C) 2025 Huawei Device Co., Ltd.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import time
import pytest
from utils import GP, check_hdc_cmd, check_hdc_targets, check_shell, load_gp


class TestHdcdCrashAndBadfd:
    @pytest.mark.L0
    def test_hdcd_crash_and_badfd(self):
        assert check_hdc_targets()
        assert not check_shell(f"shell ls data/log/faultlog/faultlogger/", "-hdcd-")