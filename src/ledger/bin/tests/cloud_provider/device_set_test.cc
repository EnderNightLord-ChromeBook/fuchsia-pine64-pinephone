// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ledger/cloud/cpp/fidl.h>
#include <gtest/gtest.h>

#include "src/ledger/bin/tests/cloud_provider/convert.h"
#include "src/ledger/bin/tests/cloud_provider/types.h"
#include "src/ledger/bin/tests/cloud_provider/validation_test.h"
#include "src/lib/fxl/logging.h"

namespace cloud_provider {
namespace {

class DeviceSetTest : public ValidationTest, public DeviceSetWatcher {
 public:
  DeviceSetTest() {}
  ~DeviceSetTest() override {}

 protected:
  ::testing::AssertionResult GetDeviceSet(DeviceSetSyncPtr* device_set) {
    *device_set = DeviceSetSyncPtr();
    Status status = Status::INTERNAL_ERROR;

    if (cloud_provider_->GetDeviceSet(device_set->NewRequest(), &status) != ZX_OK) {
      return ::testing::AssertionFailure()
             << "Failed to retrieve the device set due to channel error.";
    }

    if (status != Status::OK) {
      return ::testing::AssertionFailure() << "Failed to retrieve the device set, received status: "
                                           << fidl::ToUnderlying(status);
    }

    return ::testing::AssertionSuccess();
  }

  int on_cloud_erased_calls_ = 0;

 private:
  // DeviceSetWatcher:
  void OnCloudErased() override { on_cloud_erased_calls_++; }

  void OnError(Status status) override {
    // Do nothing - the validation test suite currently does not inject and test
    // for network errors.
    FXL_NOTIMPLEMENTED();
  }
};

TEST_F(DeviceSetTest, GetDeviceSet) {
  DeviceSetSyncPtr device_set;
  ASSERT_TRUE(GetDeviceSet(&device_set));
}

TEST_F(DeviceSetTest, CheckMissingFingerprint) {
  DeviceSetSyncPtr device_set;
  ASSERT_TRUE(GetDeviceSet(&device_set));

  Status status = Status::INTERNAL_ERROR;
  ASSERT_EQ(ZX_OK, device_set->CheckFingerprint(ToArray("bazinga"), &status));
  EXPECT_EQ(Status::NOT_FOUND, status);
}

TEST_F(DeviceSetTest, SetAndCheckFingerprint) {
  DeviceSetSyncPtr device_set;
  ASSERT_TRUE(GetDeviceSet(&device_set));

  Status status = Status::INTERNAL_ERROR;
  ASSERT_EQ(ZX_OK, device_set->SetFingerprint(ToArray("bazinga"), &status));
  EXPECT_EQ(Status::OK, status);

  ASSERT_EQ(ZX_OK, device_set->CheckFingerprint(ToArray("bazinga"), &status));
  EXPECT_EQ(Status::OK, status);
}

TEST_F(DeviceSetTest, WatchMisingFingerprint) {
  DeviceSetSyncPtr device_set;
  ASSERT_TRUE(GetDeviceSet(&device_set));
  Status status = Status::INTERNAL_ERROR;
  fidl::Binding<DeviceSetWatcher> binding(this);
  DeviceSetWatcherPtr watcher;
  binding.Bind(watcher.NewRequest());
  ASSERT_EQ(ZX_OK, device_set->SetWatcher(ToArray("bazinga"), std::move(watcher), &status));
  EXPECT_EQ(Status::NOT_FOUND, status);
}

TEST_F(DeviceSetTest, SetAndWatchFingerprint) {
  DeviceSetSyncPtr device_set;
  ASSERT_TRUE(GetDeviceSet(&device_set));

  Status status = Status::INTERNAL_ERROR;
  EXPECT_EQ(ZX_OK, device_set->SetFingerprint(ToArray("bazinga"), &status));
  EXPECT_EQ(Status::OK, status);

  fidl::Binding<DeviceSetWatcher> binding(this);
  DeviceSetWatcherPtr watcher;
  binding.Bind(watcher.NewRequest());
  ASSERT_EQ(ZX_OK, device_set->SetWatcher(ToArray("bazinga"), std::move(watcher), &status));
  EXPECT_EQ(Status::OK, status);
}

TEST_F(DeviceSetTest, EraseWhileWatching) {
  DeviceSetSyncPtr device_set;
  ASSERT_TRUE(GetDeviceSet(&device_set));

  Status status = Status::INTERNAL_ERROR;
  ASSERT_EQ(ZX_OK, device_set->SetFingerprint(ToArray("bazinga"), &status));
  EXPECT_EQ(Status::OK, status);

  fidl::Binding<DeviceSetWatcher> binding(this);
  DeviceSetWatcherPtr watcher;
  binding.Bind(watcher.NewRequest());
  ASSERT_EQ(ZX_OK, device_set->SetWatcher(ToArray("bazinga"), std::move(watcher), &status));
  EXPECT_EQ(Status::OK, status);

  EXPECT_EQ(0, on_cloud_erased_calls_);
  ASSERT_EQ(ZX_OK, device_set->Erase(&status));
  EXPECT_EQ(Status::OK, status);

  ASSERT_EQ(ZX_OK, binding.WaitForMessage());
  EXPECT_EQ(1, on_cloud_erased_calls_);
}

}  // namespace
}  // namespace cloud_provider
