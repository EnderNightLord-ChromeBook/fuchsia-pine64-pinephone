#!/usr/bin/env python2.7
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import shutil
import unittest
import tempfile

import test_env
from lib.args import Args
from lib.cipd import Cipd
from lib.fuzzer import Fuzzer

from cipd_mock import MockCipd
from device_mock import MockDevice


class TestCipd(unittest.TestCase):
    def test_from_args(self):
        fuzzer = Fuzzer(MockDevice(), u'mock-package1', u'mock-target3')
        parser = Args.make_parser('description')

        args = parser.parse_args(['1/3'])
        cipd = Cipd.from_args(fuzzer, args)
        self.assertTrue(os.path.exists(cipd.root))

        tmp_dir = tempfile.mkdtemp()
        try:
            args = parser.parse_args(['1/3', '--staging', tmp_dir])
            cipd = Cipd.from_args(fuzzer, args)
            self.assertEqual(tmp_dir, cipd.root)
        finally:
            shutil.rmtree(tmp_dir)

    def test_install(self):
        mock_cipd = MockCipd()
        mock_cipd.install()
        self.assertIn(
            'CWD=' + mock_cipd.root + ' ' + mock_cipd._bin +
            ' install fuchsia/test_data/fuzzing/' + str(
                mock_cipd.fuzzer) + ' latest', mock_cipd.history)
        mock_cipd.label = 'integration:f581cfbbb07fd975f2008ddfed7a9d9f92491375'
        mock_cipd.install()
        self.assertIn(
            'CWD=' + mock_cipd.root + ' ' + mock_cipd._bin +
            ' install fuchsia/test_data/fuzzing/' + str(mock_cipd.fuzzer) +
            ' s4BNxLyjxJIdZWumbZZtIf1a5TdrjGSpvEbSxQhQBE4C', mock_cipd.history)

    def test_create(self):
        mock_cipd = MockCipd()
        mock_cipd.create()
        self.assertIn(
            mock_cipd._bin + ' create --pkg-def ' + os.path.join(
                mock_cipd.root, 'cipd.yaml') +
            ' --ref latest --tag integration:' + mock_cipd.host.snapshot(),
            mock_cipd.history)


if __name__ == '__main__':
    unittest.main()
