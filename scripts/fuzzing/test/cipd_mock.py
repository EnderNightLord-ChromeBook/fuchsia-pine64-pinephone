#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import test_env
from lib.cipd import Cipd
from lib.fuzzer import Fuzzer

from device_mock import MockDevice


class MockCipd(Cipd):
    def __init__(self):
        self.fuzzer = Fuzzer(MockDevice(), u'mock-package1', u'mock-target3')
        self.history = []
        super(MockCipd, self).__init__(self.fuzzer)

    def _exec(self, cmd, cwd=None, quiet=False):
        """Overrides Cipd._exec for testing."""
        logged = 'CWD=' + cwd + ' ' if cwd else ''
        logged += self._bin + ' ' + ' '.join(cmd)
        self.history.append(logged)
