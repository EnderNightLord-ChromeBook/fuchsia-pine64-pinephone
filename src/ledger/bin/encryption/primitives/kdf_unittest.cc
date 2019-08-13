// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/encryption/primitives/kdf.h"

#include <gtest/gtest.h>

#include "src/ledger/bin/encryption/primitives/crypto_test_util.h"

namespace encryption {
namespace {

TEST(KDF, Correctness) {
  std::string data = FromHex("0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B0B");
  std::string expected = FromHex(
      "8DA4E775A563C18F715F802A063C5A31B8A11F5C5EE1879EC3454E5F3C738D2D9D201395"
      "FAA4B61A96C8");
  EXPECT_EQ(HMAC256KDF(data, expected.size()), expected);
}
}  // namespace
}  // namespace encryption
