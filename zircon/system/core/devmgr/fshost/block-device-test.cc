// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "block-device.h"

#include <fcntl.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/fdio/namespace.h>
#include <zircon/assert.h>
#include <zircon/hw/gpt.h>

#include <cobalt-client/cpp/collector.h>
#include <cobalt-client/cpp/in-memory-logger.h>
#include <minfs/format.h>
#include <ramdevice-client/ramdisk.h>
#include <zxtest/zxtest.h>

#include "filesystem-mounter.h"
#include "fs-manager.h"
#include "metrics.h"

namespace devmgr {
namespace {

using devmgr_integration_test::IsolatedDevmgr;

FsHostMetrics MakeMetrics(cobalt_client::InMemoryLogger** logger) {
  std::unique_ptr<cobalt_client::InMemoryLogger> logger_ptr =
      std::make_unique<cobalt_client::InMemoryLogger>();
  *logger = logger_ptr.get();
  return FsHostMetrics(std::make_unique<cobalt_client::Collector>(std::move(logger_ptr)));
}

class BlockDeviceHarness : public zxtest::Test {
 public:
  void SetUp() override {
    zx::event event;
    ASSERT_OK(zx::event::create(0, &event));
    ASSERT_OK(event.duplicate(ZX_RIGHT_SAME_RIGHTS, &event_));

    // Initialize FilesystemMounter.
    ASSERT_OK(FsManager::Create(std::move(event), MakeMetrics(&logger_), &manager_));

    // Fshost really likes mounting filesystems at "/fs".
    // Let's make that available in our namespace.
    zx::channel client, server;
    ASSERT_OK(zx::channel::create(0, &client, &server));
    ASSERT_OK(manager_->ServeRoot(std::move(server)));
    fdio_ns_t* ns;
    ASSERT_OK(fdio_ns_get_installed(&ns));
    ASSERT_OK(fdio_ns_bind(ns, "/fs", client.release()));
    manager_->WatchExit();

    devmgr_launcher::Args args;
    args.disable_block_watcher = true;
    args.sys_device_driver = devmgr_integration_test::IsolatedDevmgr::kSysdevDriver;
    args.load_drivers.push_back(devmgr_integration_test::IsolatedDevmgr::kSysdevDriver);
    args.driver_search_paths.push_back("/boot/driver");
    ASSERT_OK(IsolatedDevmgr::Create(std::move(args), &devmgr_));
    fbl::unique_fd ctl;
    ASSERT_EQ(
        devmgr_integration_test::RecursiveWaitForFile(devmgr_.devfs_root(), "misc/ramctl", &ctl),
        ZX_OK);
  }

  void TearDown() override {
    fdio_ns_t* ns;
    ASSERT_OK(fdio_ns_get_installed(&ns));
    fdio_ns_unbind(ns, "/fs");
  }

  std::unique_ptr<FsManager> TakeManager() { return std::move(manager_); }

  fbl::unique_fd devfs_root() { return devmgr_.devfs_root().duplicate(); }

 protected:
  cobalt_client::InMemoryLogger* logger_ = nullptr;

 private:
  zx::event event_;
  std::unique_ptr<FsManager> manager_;
  IsolatedDevmgr devmgr_;
};

TEST_F(BlockDeviceHarness, TestBadHandleDevice) {
  std::unique_ptr<FsManager> manager = TakeManager();
  bool netboot = false;
  bool check_filesystems = false;
  FilesystemMounter mounter(std::move(manager), netboot, check_filesystems);
  fbl::unique_fd fd;
  BlockDevice device(&mounter, std::move(fd));
  EXPECT_EQ(device.Netbooting(), netboot);
  EXPECT_EQ(device.GetFormat(), DISK_FORMAT_UNKNOWN);
  fuchsia_hardware_block_BlockInfo info;
  EXPECT_EQ(device.GetInfo(&info), ZX_ERR_BAD_HANDLE);
  fuchsia_hardware_block_partition_GUID guid;
  EXPECT_EQ(device.GetTypeGUID(&guid), ZX_ERR_BAD_HANDLE);
  EXPECT_EQ(device.AttachDriver("/foobar"), ZX_ERR_BAD_HANDLE);

  // Returns ZX_OK because zxcrypt currently passes the empty fd to a background
  // thread without observing the results.
  EXPECT_OK(device.UnsealZxcrypt());

  // Returns ZX_OK because filesystem checks are disabled.
  EXPECT_OK(device.CheckFilesystem());

  EXPECT_EQ(device.FormatFilesystem(), ZX_ERR_BAD_HANDLE);
  EXPECT_EQ(device.MountFilesystem(), ZX_ERR_BAD_HANDLE);
}

TEST_F(BlockDeviceHarness, TestEmptyDevice) {
  std::unique_ptr<FsManager> manager = TakeManager();
  bool netboot = false;
  bool check_filesystems = false;
  FilesystemMounter mounter(std::move(manager), netboot, check_filesystems);

  // Initialize Ramdisk.
  constexpr uint64_t kBlockSize = 512;
  constexpr uint64_t kBlockCount = 1 << 20;
  ramdisk_client_t* ramdisk;
  ASSERT_OK(ramdisk_create_at(devfs_root().get(), kBlockSize, kBlockCount, &ramdisk));
  fbl::unique_fd fd;
  ASSERT_EQ(
      devmgr_integration_test::RecursiveWaitForFile(devfs_root(), ramdisk_get_path(ramdisk), &fd),
      ZX_OK);
  ASSERT_TRUE(fd);

  BlockDevice device(&mounter, std::move(fd));
  EXPECT_EQ(device.Netbooting(), netboot);
  EXPECT_EQ(device.GetFormat(), DISK_FORMAT_UNKNOWN);
  fuchsia_hardware_block_BlockInfo info;
  EXPECT_OK(device.GetInfo(&info));
  EXPECT_EQ(info.block_count, kBlockCount);
  EXPECT_EQ(info.block_size, kBlockSize);

  // Black-box: Since we're caching info, double check that re-calling GetInfo
  // works correctly.
  memset(&info, 0, sizeof(info));
  EXPECT_OK(device.GetInfo(&info));
  EXPECT_EQ(info.block_count, kBlockCount);
  EXPECT_EQ(info.block_size, kBlockSize);

  fuchsia_hardware_block_partition_GUID guid;
  EXPECT_OK(device.GetTypeGUID(&guid));

  EXPECT_EQ(device.FormatFilesystem(), ZX_ERR_NOT_SUPPORTED);
  EXPECT_EQ(device.MountFilesystem(), ZX_ERR_NOT_SUPPORTED);
  ASSERT_OK(ramdisk_destroy(ramdisk));
}

TEST_F(BlockDeviceHarness, TestMinfsBadGUID) {
  std::unique_ptr<FsManager> manager = TakeManager();
  bool netboot = false;
  bool check_filesystems = false;
  FilesystemMounter mounter(std::move(manager), netboot, check_filesystems);

  // Initialize Ramdisk with an empty GUID.
  constexpr uint64_t kBlockSize = 512;
  constexpr uint64_t kBlockCount = 1 << 20;
  ramdisk_client_t* ramdisk;
  ASSERT_OK(ramdisk_create_at(devfs_root().get(), kBlockSize, kBlockCount, &ramdisk));
  fbl::unique_fd fd;
  ASSERT_EQ(
      devmgr_integration_test::RecursiveWaitForFile(devfs_root(), ramdisk_get_path(ramdisk), &fd),
      ZX_OK);
  ASSERT_TRUE(fd);

  // We started with an empty block device, but let's lie and say it
  // should have been a minfs device.
  BlockDevice device(&mounter, std::move(fd));
  device.SetFormat(DISK_FORMAT_MINFS);
  EXPECT_EQ(device.GetFormat(), DISK_FORMAT_MINFS);
  EXPECT_OK(device.FormatFilesystem());

  // Unlike earlier, where we received "ERR_NOT_SUPPORTED", we get "ERR_WRONG_TYPE"
  // because the ramdisk doesn't have a data GUID.
  EXPECT_EQ(device.MountFilesystem(), ZX_ERR_WRONG_TYPE);

  ASSERT_OK(ramdisk_destroy(ramdisk));
}

TEST_F(BlockDeviceHarness, TestMinfsGoodGUID) {
  std::unique_ptr<FsManager> manager = TakeManager();

  bool netboot = false;
  bool check_filesystems = false;
  FilesystemMounter mounter(std::move(manager), netboot, check_filesystems);

  // Initialize Ramdisk with a data GUID.
  constexpr uint64_t kBlockSize = 512;
  constexpr uint64_t kBlockCount = 1 << 20;
  ramdisk_client_t* ramdisk;
  const uint8_t data_guid[GPT_GUID_LEN] = GUID_DATA_VALUE;
  ASSERT_OK(ramdisk_create_at_with_guid(devfs_root().get(), kBlockSize, kBlockCount, data_guid,
                                        sizeof(data_guid), &ramdisk));
  fbl::unique_fd fd;
  ASSERT_EQ(
      devmgr_integration_test::RecursiveWaitForFile(devfs_root(), ramdisk_get_path(ramdisk), &fd),
      ZX_OK);
  ASSERT_TRUE(fd);

  BlockDevice device(&mounter, std::move(fd));
  device.SetFormat(DISK_FORMAT_MINFS);
  EXPECT_EQ(device.GetFormat(), DISK_FORMAT_MINFS);
  EXPECT_OK(device.FormatFilesystem());

  EXPECT_OK(device.MountFilesystem());
  EXPECT_EQ(device.MountFilesystem(), ZX_ERR_ALREADY_BOUND);

  ASSERT_OK(ramdisk_destroy(ramdisk));
}

TEST_F(BlockDeviceHarness, TestMinfsReformat) {
  std::unique_ptr<FsManager> manager = TakeManager();

  bool netboot = false;
  bool check_filesystems = true;
  FilesystemMounter mounter(std::move(manager), netboot, check_filesystems);

  // Initialize Ramdisk with a data GUID.
  constexpr uint64_t kBlockSize = 512;
  constexpr uint64_t kBlockCount = 1 << 20;
  ramdisk_client_t* ramdisk;
  const uint8_t data_guid[GPT_GUID_LEN] = GUID_DATA_VALUE;
  ASSERT_OK(ramdisk_create_at_with_guid(devfs_root().get(), kBlockSize, kBlockCount, data_guid,
                                        sizeof(data_guid), &ramdisk));
  fbl::unique_fd fd;
  ASSERT_EQ(
      devmgr_integration_test::RecursiveWaitForFile(devfs_root(), ramdisk_get_path(ramdisk), &fd),
      ZX_OK);

  ASSERT_TRUE(fd);

  BlockDevice device(&mounter, std::move(fd));
  device.SetFormat(DISK_FORMAT_MINFS);
  EXPECT_EQ(device.GetFormat(), DISK_FORMAT_MINFS);

  // Before formatting the device, this isn't a valid minfs partition.
  EXPECT_NOT_OK(device.CheckFilesystem());
  EXPECT_NOT_OK(device.MountFilesystem());

  // After formatting the device, it is a valid partition. We can check the device,
  // and also mount it.
  EXPECT_OK(device.FormatFilesystem());
  EXPECT_OK(device.CheckFilesystem());
  EXPECT_OK(device.MountFilesystem());

  ASSERT_OK(ramdisk_destroy(ramdisk));
}

TEST_F(BlockDeviceHarness, TestBlobfs) {
  std::unique_ptr<FsManager> manager = TakeManager();

  bool netboot = false;
  bool check_filesystems = true;
  FilesystemMounter mounter(std::move(manager), netboot, check_filesystems);

  // Initialize Ramdisk with a data GUID.
  constexpr uint64_t kBlockSize = 512;
  constexpr uint64_t kBlockCount = 1 << 20;
  ramdisk_client_t* ramdisk;
  const uint8_t data_guid[GPT_GUID_LEN] = GUID_BLOB_VALUE;
  ASSERT_OK(ramdisk_create_at_with_guid(devfs_root().get(), kBlockSize, kBlockCount, data_guid,
                                        sizeof(data_guid), &ramdisk));
  fbl::unique_fd fd;
  ASSERT_EQ(
      devmgr_integration_test::RecursiveWaitForFile(devfs_root(), ramdisk_get_path(ramdisk), &fd),
      ZX_OK);
  ASSERT_TRUE(fd);

  BlockDevice device(&mounter, std::move(fd));
  device.SetFormat(DISK_FORMAT_BLOBFS);
  EXPECT_EQ(device.GetFormat(), DISK_FORMAT_BLOBFS);

  // Before formatting the device, this isn't a valid blobfs partition.
  // However, as implemented, we always validate the consistency of the filesystem.
  EXPECT_OK(device.CheckFilesystem());
  EXPECT_NOT_OK(device.MountFilesystem());

  // Additionally, blobfs does not yet support reformatting within fshost.
  EXPECT_NOT_OK(device.FormatFilesystem());
  EXPECT_OK(device.CheckFilesystem());
  EXPECT_NOT_OK(device.MountFilesystem());

  ASSERT_OK(ramdisk_destroy(ramdisk));
}

TEST_F(BlockDeviceHarness, TestCorruptionEventLogged) {
  std::unique_ptr<FsManager> manager = TakeManager();

  bool netboot = false;
  bool check_filesystems = true;
  FilesystemMounter mounter(std::move(manager), netboot, check_filesystems);

  // Initialize Ramdisk with a data GUID.
  constexpr uint64_t kBlockSize = 512;
  constexpr uint64_t kBlockCount = 1 << 20;
  ramdisk_client_t* ramdisk;
  const uint8_t data_guid[GPT_GUID_LEN] = GUID_DATA_VALUE;
  ASSERT_OK(ramdisk_create_at_with_guid(devfs_root().get(), kBlockSize, kBlockCount, data_guid,
                                        sizeof(data_guid), &ramdisk));
  fbl::unique_fd fd;
  ASSERT_OK(
      devmgr_integration_test::RecursiveWaitForFile(devfs_root(), ramdisk_get_path(ramdisk), &fd));

  ASSERT_TRUE(fd);

  BlockDevice device(&mounter, std::move(fd));
  device.SetFormat(DISK_FORMAT_MINFS);
  EXPECT_EQ(device.GetFormat(), DISK_FORMAT_MINFS);
  // Format minfs.
  EXPECT_OK(device.FormatFilesystem());

  // Corrupt minfs.
  int ramdisk_fd = ramdisk_get_block_fd(ramdisk);
  uint64_t buffer_size = minfs::kMinfsBlockSize * 8;
  std::unique_ptr<uint8_t[]> zeroed_buffer(new uint8_t[buffer_size]);
  memset(zeroed_buffer.get(), 0, buffer_size);
  ASSERT_EQ(write(ramdisk_fd, zeroed_buffer.get(), buffer_size), buffer_size);

  EXPECT_NOT_OK(device.CheckFilesystem());

  // Verify a corruption event was logged.
  uint32_t metric_id = static_cast<std::underlying_type<fs_metrics::Event>::type>(
      fs_metrics::Event::kDataCorruption);
  ASSERT_NE(logger_->counters().find(metric_id), logger_->counters().end());
  ASSERT_EQ(logger_->counters().at(metric_id), 1);

  ASSERT_OK(ramdisk_destroy(ramdisk));
}

// TODO: Add tests for Zxcrypt binding.

}  // namespace
}  // namespace devmgr
