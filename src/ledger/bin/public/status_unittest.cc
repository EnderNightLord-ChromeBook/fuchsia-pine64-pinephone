// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/public/status.h"

#include <sstream>

#include "gtest/gtest.h"

namespace ledger {
namespace {
TEST(StatusTest, StatusToString) { EXPECT_EQ(StatusToString(Status::OK), "OK"); }

TEST(StatusTest, StatusToStream) {
  std::stringstream ss;
  ss << Status::OK;
  EXPECT_EQ(ss.str(), "OK");
}

}  // namespace
}  // namespace ledger
