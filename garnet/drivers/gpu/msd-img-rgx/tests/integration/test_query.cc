// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>

#include <thread>

#include "gtest/gtest.h"
#include "helper/platform_device_helper.h"
#include "magma.h"
#include "magma_util/macros.h"
#include "no_hardware_testing.h"

namespace {

#ifndef NO_HARDWARE
#error Test should only be built for running against the no hardware driver.
#endif

TEST(ImgtecNoHardware, QueryReturnsBuffer) {
  int fd = open("/dev/test/msd-img-rgx-no-hardware", O_RDONLY);
  uint32_t buffer_id;
  EXPECT_EQ(MAGMA_STATUS_OK,
            magma_query_returns_buffer(fd, no_hardware_testing::kDummyQueryId, &buffer_id));

  magma_connection_t connection;
  EXPECT_EQ(MAGMA_STATUS_OK, magma_create_connection(fd, &connection));

  magma_buffer_t buffer;
  EXPECT_EQ(MAGMA_STATUS_OK, magma_import(connection, buffer_id, &buffer));
  void* data;
  EXPECT_EQ(MAGMA_STATUS_OK, magma_map(connection, buffer, &data));
  EXPECT_EQ(no_hardware_testing::kDummyQueryResult, *reinterpret_cast<uint32_t*>(data));
}

}  // namespace
