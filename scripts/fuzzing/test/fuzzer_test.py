#!/usr/bin/env python
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import shutil
import tempfile
import unittest

import test_env
from lib.args import Args
from lib.fuzzer import Fuzzer

from device_mock import MockDevice
from host_mock import MockHost


class TestFuzzer(unittest.TestCase):

  def test_filter(self):
    host = MockHost()
    fuzzers = host.fuzzers
    self.assertEqual(len(Fuzzer.filter(fuzzers, '')), 6)
    self.assertEqual(len(Fuzzer.filter(fuzzers, '/')), 6)
    self.assertEqual(len(Fuzzer.filter(fuzzers, 'mock')), 6)
    self.assertEqual(len(Fuzzer.filter(fuzzers, 'package1')), 3)
    self.assertEqual(len(Fuzzer.filter(fuzzers, 'target1')), 3)
    self.assertEqual(len(Fuzzer.filter(fuzzers, 'package2/target1')), 2)
    self.assertEqual(
        len(Fuzzer.filter(fuzzers, 'mock-package2/mock-target1')), 1)
    self.assertEqual(len(Fuzzer.filter(fuzzers, '1/2')), 1)
    self.assertEqual(len(Fuzzer.filter(fuzzers, 'target4')), 0)
    with self.assertRaises(Fuzzer.NameError):
      Fuzzer.filter(fuzzers, 'a/b/c')

  def test_from_args(self):
    mock_device = MockDevice()
    parser = Args.make_parser('description')
    with self.assertRaises(Fuzzer.NameError):
      args = parser.parse_args(['target'])
      fuzzer = Fuzzer.from_args(mock_device, args)
    with self.assertRaises(Fuzzer.NameError):
      args = parser.parse_args(['target4'])
      fuzzer = Fuzzer.from_args(mock_device, args)

  def test_measure_corpus(self):
    fuzzer = Fuzzer(MockDevice(), u'mock-package1', u'mock-target1')
    sizes = fuzzer.measure_corpus()
    self.assertEqual(sizes[0], 2)
    self.assertEqual(sizes[1], 1796 + 124)

  def test_list_artifacts(self):
    fuzzer = Fuzzer(MockDevice(), u'mock-package1', u'mock-target1')
    artifacts = fuzzer.list_artifacts()
    self.assertEqual(len(artifacts), 3)
    self.assertTrue('crash-deadbeef' in artifacts)
    self.assertTrue('leak-deadfa11' in artifacts)
    self.assertTrue('oom-feedface' in artifacts)
    self.assertFalse('fuzz-0.log' in artifacts)

  def test_is_running(self):
    mock_device = MockDevice()
    fuzzer1 = Fuzzer(mock_device, u'mock-package1', u'mock-target1')
    fuzzer2 = Fuzzer(mock_device, u'mock-package1', u'mock-target2')
    fuzzer3 = Fuzzer(mock_device, u'mock-package1', u'mock-target3')
    self.assertTrue(fuzzer1.is_running())
    self.assertTrue(fuzzer2.is_running())
    self.assertFalse(fuzzer2.is_running())
    self.assertFalse(fuzzer3.is_running())

  def test_require_stopped(self):
    mock_device = MockDevice()
    fuzzer1 = Fuzzer(mock_device, u'mock-package1', u'mock-target1')
    fuzzer2 = Fuzzer(mock_device, u'mock-package1', u'mock-target2')
    fuzzer3 = Fuzzer(mock_device, u'mock-package1', u'mock-target3')
    with self.assertRaises(Fuzzer.StateError):
      fuzzer1.require_stopped()
    with self.assertRaises(Fuzzer.StateError):
      fuzzer2.require_stopped()
    fuzzer2.require_stopped()
    fuzzer3.require_stopped()

  def test_run(self):
    mock_device = MockDevice()
    fuzzer = Fuzzer(mock_device, u'mock-package1', u'mock-target2')
    fuzzer.run(['-some-lf-arg=value'])
    self.assertIn(' '.join(
        mock_device.get_ssh_cmd([
            'ssh', '::1', 'run',
            fuzzer.url(), '-artifact_prefix=data/', '-some-lf-arg=value'
        ])), mock_device.history)

  def test_start(self):
    mock_device = MockDevice()
    base_dir = tempfile.mkdtemp()
    try:
      fuzzer = Fuzzer(
          mock_device, u'mock-package1', u'mock-target2', output=base_dir)
      fuzzer.start(['-some-lf-arg=value'])
      self.assertTrue(os.path.exists(fuzzer.results('symbolized.log')))
    finally:
      shutil.rmtree(base_dir)

  def test_stop(self):
    mock_device = MockDevice()
    pids = mock_device.getpids()
    fuzzer1 = Fuzzer(mock_device, u'mock-package1', u'mock-target1')
    fuzzer1.stop()
    self.assertIn(' '.join(
        mock_device.get_ssh_cmd(['ssh', '::1', 'kill',
                                 str(pids[fuzzer1.tgt])])), mock_device.history)
    fuzzer3 = Fuzzer(mock_device, u'mock-package1', u'mock-target3')
    fuzzer3.stop()

  def test_repro(self):
    mock_device = MockDevice()
    fuzzer = Fuzzer(mock_device, u'mock-package1', u'mock-target2')
    artifacts = ['data/' + artifact for artifact in fuzzer.list_artifacts()]
    fuzzer.repro(['-some-lf-arg=value'])
    self.assertIn(' '.join(
        mock_device.get_ssh_cmd([
            'ssh', '::1', 'run',
            fuzzer.url(), '-artifact_prefix=data/', '-some-lf-arg=value'
        ] + artifacts)), mock_device.history)

  def test_merge(self):
    mock_device = MockDevice()
    fuzzer = Fuzzer(mock_device, u'mock-package1', u'mock-target2')
    fuzzer.merge(['-some-lf-arg=value'])
    self.assertIn(' '.join(
        mock_device.get_ssh_cmd([
            'ssh', '::1', 'run',
            fuzzer.url(), '-artifact_prefix=data/', '-merge=1',
            '-merge_control_file=data/.mergefile',
            '-some-lf-arg=value data/corpus/', 'data/corpus.prev/'
        ])), mock_device.history)


if __name__ == '__main__':
  unittest.main()
