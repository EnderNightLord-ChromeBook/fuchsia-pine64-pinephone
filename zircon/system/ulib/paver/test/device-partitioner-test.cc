// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device-partitioner.h"

#include <dirent.h>
#include <fcntl.h>

#include <fbl/auto_call.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/hardware/block/llcpp/fidl.h>
#include <fuchsia/hardware/nand/c/fidl.h>
#include <gpt/gpt.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/fzl/fdio.h>
#include <zircon/boot/image.h>
#include <zircon/hw/gpt.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>
#include <zxtest/zxtest.h>

#include <utility>

#include "test/test-utils.h"

namespace {

using devmgr_integration_test::IsolatedDevmgr;
using devmgr_integration_test::RecursiveWaitForFile;

constexpr uint8_t kEmptyType[GPT_GUID_LEN] = GUID_EMPTY_VALUE;
constexpr uint8_t kZirconAType[GPT_GUID_LEN] = GUID_ZIRCON_A_VALUE;
constexpr uint8_t kZirconBType[GPT_GUID_LEN] = GUID_ZIRCON_B_VALUE;
constexpr uint8_t kZirconRType[GPT_GUID_LEN] = GUID_ZIRCON_R_VALUE;
constexpr uint8_t kVbMetaAType[GPT_GUID_LEN] = GUID_VBMETA_A_VALUE;
constexpr uint8_t kVbMetaBType[GPT_GUID_LEN] = GUID_VBMETA_B_VALUE;
constexpr uint8_t kFvmType[GPT_GUID_LEN] = GUID_FVM_VALUE;

constexpr fuchsia_hardware_nand_RamNandInfo
    kNandInfo =
        {
            .vmo = ZX_HANDLE_INVALID,
            .nand_info =
                {
                    .page_size = kPageSize,
                    .pages_per_block = kPagesPerBlock,
                    .num_blocks = kNumBlocks,
                    .ecc_bits = 8,
                    .oob_size = kOobSize,
                    .nand_class = fuchsia_hardware_nand_Class_PARTMAP,
                    .partition_guid = {},
                },
            .partition_map =
                {
                    .device_guid = {},
                    .partition_count = 7,
                    .partitions =
                        {
                            {
                                .type_guid = {},
                                .unique_guid = {},
                                .first_block = 0,
                                .last_block = 3,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {},
                                .hidden = true,
                                .bbt = true,
                            },
                            {
                                .type_guid = GUID_BOOTLOADER_VALUE,
                                .unique_guid = {},
                                .first_block = 4,
                                .last_block = 7,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'b', 'o', 'o', 't', 'l', 'o', 'a', 'd', 'e', 'r'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_ZIRCON_A_VALUE,
                                .unique_guid = {},
                                .first_block = 8,
                                .last_block = 9,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'z', 'i', 'r', 'c', 'o', 'n', '-', 'a'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_ZIRCON_B_VALUE,
                                .unique_guid = {},
                                .first_block = 10,
                                .last_block = 11,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'z', 'i', 'r', 'c', 'o', 'n', '-', 'b'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_ZIRCON_R_VALUE,
                                .unique_guid = {},
                                .first_block = 12,
                                .last_block = 13,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'z', 'i', 'r', 'c', 'o', 'n', '-', 'r'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_VBMETA_A_VALUE,
                                .unique_guid = {},
                                .first_block = 14,
                                .last_block = 15,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'v', 'b', 'm', 'e', 't', 'a', '-', 'a'},
                                .hidden = false,
                                .bbt = false,
                            },
                            {
                                .type_guid = GUID_VBMETA_B_VALUE,
                                .unique_guid = {},
                                .first_block = 16,
                                .last_block = 17,
                                .copy_count = 0,
                                .copy_byte_offset = 0,
                                .name = {'v', 'b', 'm', 'e', 't', 'a', '-', 'b'},
                                .hidden = false,
                                .bbt = false,
                            },
                        },
                },
            .export_nand_config = true,
            .export_partition_map = true,
};

}  // namespace

class EfiPartitionerTests : public zxtest::Test {
 protected:
  EfiPartitionerTests() {
    devmgr_launcher::Args args;
    args.sys_device_driver = IsolatedDevmgr::kSysdevDriver;
    args.driver_search_paths.push_back("/boot/driver");
    args.use_system_svchost = true;
    args.disable_block_watcher = true;
    ASSERT_OK(IsolatedDevmgr::Create(std::move(args), &devmgr_));

    fbl::unique_fd fd;
    ASSERT_OK(RecursiveWaitForFile(devmgr_.devfs_root(), "misc/ramctl", &fd));
  }

  IsolatedDevmgr devmgr_;
};

TEST_F(EfiPartitionerTests, InitializeWithoutGptFails) {
  fbl::unique_ptr<BlockDevice> gpt_dev;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, &gpt_dev));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_NE(
      paver::EfiDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), paver::Arch::kX64,
                                              std::nullopt, &partitioner),
      ZX_OK);
}

TEST_F(EfiPartitionerTests, InitializeWithoutFvmFails) {
  fbl::unique_ptr<BlockDevice> gpt_dev;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, &gpt_dev));

  // Set up a valid GPT.
  fbl::unique_ptr<gpt::GptDevice> gpt;
  ASSERT_OK(gpt::GptDevice::Create(gpt_dev->fd(), kBlockSize, kBlockCount, &gpt));
  ASSERT_OK(gpt->Sync());

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_NE(
      paver::EfiDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), paver::Arch::kX64,
                                              std::nullopt, &partitioner),
      ZX_OK);
}

TEST_F(EfiPartitionerTests, AddPartitionZirconB) {
  fbl::unique_ptr<BlockDevice> gpt_dev;
  constexpr uint64_t kBlockCount = (1LU << 26) / kBlockSize;
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev));
  fbl::unique_fd gpt_fd(dup(gpt_dev->fd()));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));

  ASSERT_OK(partitioner->AddPartition(paver::Partition::kZirconB, nullptr));
}

TEST_F(EfiPartitionerTests, AddPartitionFvm) {
  fbl::unique_ptr<BlockDevice> gpt_dev;
  constexpr uint64_t kBlockCount = (1LU << 34) / kBlockSize;
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev));
  fbl::unique_fd gpt_fd(dup(gpt_dev->fd()));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));

  ASSERT_OK(partitioner->AddPartition(paver::Partition::kFuchsiaVolumeManager, nullptr));
}

TEST_F(EfiPartitionerTests, AddPartitionTooSmall) {
  fbl::unique_ptr<BlockDevice> gpt_dev;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, &gpt_dev));
  fbl::unique_fd gpt_fd(dup(gpt_dev->fd()));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));

  ASSERT_NE(partitioner->AddPartition(paver::Partition::kZirconB, nullptr), ZX_OK);
}

TEST_F(EfiPartitionerTests, AddedPartitionIsFindable) {
  fbl::unique_ptr<BlockDevice> gpt_dev;
  constexpr uint64_t kBlockCount = (1LU << 26) / kBlockSize;
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev));
  fbl::unique_fd gpt_fd(dup(gpt_dev->fd()));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));

  ASSERT_OK(partitioner->AddPartition(paver::Partition::kZirconB, nullptr));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, nullptr));
  ASSERT_NE(partitioner->FindPartition(paver::Partition::kZirconA, nullptr), ZX_OK);
}

TEST_F(EfiPartitionerTests, InitializePartitionsWithoutExplicitDevice) {
  fbl::unique_ptr<BlockDevice> gpt_dev;
  constexpr uint64_t kBlockCount = (1LU << 34) / kBlockSize;
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev));
  fbl::unique_fd gpt_fd(dup(gpt_dev->fd()));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));

  ASSERT_OK(partitioner->AddPartition(paver::Partition::kFuchsiaVolumeManager, nullptr));
  partitioner.reset();

  fbl::unique_fd fd;
  // Note that this time we don't pass in a block device fd.
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::nullopt, &partitioner));
}

TEST_F(EfiPartitionerTests, InitializeWithMultipleCandidateGPTsFailsWithoutExplicitDevice) {
  fbl::unique_ptr<BlockDevice> gpt_dev1, gpt_dev2;
  constexpr uint64_t kBlockCount = (1LU << 34) / kBlockSize;
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev1));
  fbl::unique_fd gpt_fd(dup(gpt_dev1->fd()));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));

  ASSERT_OK(partitioner->AddPartition(paver::Partition::kFuchsiaVolumeManager, nullptr));
  partitioner.reset();

  partitioner.reset();
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev2));
  gpt_fd.reset(dup(gpt_dev2->fd()));

  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));
  ASSERT_OK(partitioner->AddPartition(paver::Partition::kFuchsiaVolumeManager, nullptr));
  partitioner.reset();

  // Note that this time we don't pass in a block device fd.
  ASSERT_NE(
      paver::EfiDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), paver::Arch::kX64,
                                              std::nullopt, &partitioner),
      ZX_OK);
}

TEST_F(EfiPartitionerTests, InitializeWithTwoCandidateGPTsSucceedsAfterWipingOne) {
  fbl::unique_ptr<BlockDevice> gpt_dev1, gpt_dev2;
  constexpr uint64_t kBlockCount = (1LU << 34) / kBlockSize;
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev1));
  fbl::unique_fd gpt_fd(dup(gpt_dev1->fd()));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));

  ASSERT_OK(partitioner->AddPartition(paver::Partition::kFuchsiaVolumeManager, nullptr));
  partitioner.reset();

  partitioner.reset();
  ASSERT_NO_FATAL_FAILURES(
      BlockDevice::Create(devmgr_.devfs_root(), kEmptyType, kBlockCount, &gpt_dev2));
  gpt_fd.reset(dup(gpt_dev2->fd()));

  ASSERT_OK(paver::EfiDevicePartitioner::Initialize(
      devmgr_.devfs_root().duplicate(), paver::Arch::kX64, std::move(gpt_fd), &partitioner));
  ASSERT_OK(partitioner->AddPartition(paver::Partition::kFuchsiaVolumeManager, nullptr));
  ASSERT_OK(partitioner->WipeFvm());
  partitioner.reset();

  // Note that this time we don't pass in a block device fd.
  ASSERT_OK(
      paver::EfiDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), paver::Arch::kX64,
                                              std::nullopt, &partitioner));
}

class FixedDevicePartitionerTests : public zxtest::Test {
 protected:
  FixedDevicePartitionerTests() {
    devmgr_launcher::Args args;
    args.sys_device_driver = IsolatedDevmgr::kSysdevDriver;
    args.driver_search_paths.push_back("/boot/driver");
    args.use_system_svchost = true;
    args.disable_block_watcher = true;
    ASSERT_OK(IsolatedDevmgr::Create(std::move(args), &devmgr_));

    fbl::unique_fd fd;
    ASSERT_OK(RecursiveWaitForFile(devmgr_.devfs_root(), "misc/ramctl", &fd));
  }

  IsolatedDevmgr devmgr_;
};

TEST_F(FixedDevicePartitionerTests, UseBlockInterfaceTest) {
  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(
      paver::FixedDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), &partitioner));
  ASSERT_FALSE(partitioner->UseSkipBlockInterface());
}

TEST_F(FixedDevicePartitionerTests, AddPartitionTest) {
  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(
      paver::FixedDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), &partitioner));
  ASSERT_EQ(partitioner->AddPartition(paver::Partition::kZirconB, nullptr), ZX_ERR_NOT_SUPPORTED);
}

TEST_F(FixedDevicePartitionerTests, WipeFvmTest) {
  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(
      paver::FixedDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), &partitioner));
  ASSERT_OK(partitioner->WipeFvm());
}

TEST_F(FixedDevicePartitionerTests, FinalizePartitionTest) {
  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_OK(
      paver::FixedDevicePartitioner::Initialize(devmgr_.devfs_root().duplicate(), &partitioner));

  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconA));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconB));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconR));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaA));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaB));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kFuchsiaVolumeManager));
}

TEST_F(FixedDevicePartitionerTests, FindPartitionTest) {
  fbl::unique_ptr<BlockDevice> fvm, zircon_a, zircon_b, zircon_r, vbmeta_a, vbmeta_b;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kZirconAType, &zircon_a));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kZirconBType, &zircon_b));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kZirconRType, &zircon_r));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kVbMetaAType, &vbmeta_a));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kVbMetaBType, &vbmeta_b));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kFvmType, &fvm));

  auto partitioner =
      paver::DevicePartitioner::Create(devmgr_.devfs_root().duplicate(), paver::Arch::kArm64);
  ASSERT_NE(partitioner.get(), nullptr);

  fbl::unique_fd fd;
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
}

TEST_F(FixedDevicePartitionerTests, GetBlockSizeTest) {
  fbl::unique_ptr<BlockDevice> fvm, zircon_a, zircon_b, zircon_r, vbmeta_a, vbmeta_b;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kZirconAType, &zircon_a));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kZirconBType, &zircon_b));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kZirconRType, &zircon_r));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kVbMetaAType, &vbmeta_a));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kVbMetaBType, &vbmeta_b));
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devmgr_.devfs_root(), kFvmType, &fvm));

  auto partitioner =
      paver::DevicePartitioner::Create(devmgr_.devfs_root().duplicate(), paver::Arch::kArm64);
  ASSERT_NE(partitioner.get(), nullptr);

  fbl::unique_fd fd;
  uint32_t block_size;
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kBlockSize);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kBlockSize);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kBlockSize);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kBlockSize);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kBlockSize);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kBlockSize);
}

TEST(SkipBlockDevicePartitionerTests, UseSkipBlockInterfaceTest) {
  fbl::unique_ptr<SkipBlockDevice> device;
  SkipBlockDevice::Create(kNandInfo, &device);

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
            ZX_OK);
  ASSERT_TRUE(partitioner->UseSkipBlockInterface());
}

TEST(SkipBlockDevicePartitionerTests, ChooseSkipBlockPartitioner) {
  fbl::unique_ptr<SkipBlockDevice> device;
  SkipBlockDevice::Create(kNandInfo, &device);
  auto devfs_root = device->devfs_root();
  fbl::unique_ptr<BlockDevice> zircon_a;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devfs_root, kZirconAType, &zircon_a));

  auto partitioner = paver::DevicePartitioner::Create(std::move(devfs_root), paver::Arch::kArm64);
  ASSERT_NE(partitioner.get(), nullptr);
  ASSERT_TRUE(partitioner->UseSkipBlockInterface());
}

TEST(SkipBlockDevicePartitionerTests, AddPartitionTest) {
  fbl::unique_ptr<SkipBlockDevice> device;
  SkipBlockDevice::Create(kNandInfo, &device);

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
            ZX_OK);
  ASSERT_EQ(partitioner->AddPartition(paver::Partition::kZirconB, nullptr), ZX_ERR_NOT_SUPPORTED);
}

TEST(SkipBlockDevicePartitionerTests, WipeFvmTest) {
  fbl::unique_ptr<SkipBlockDevice> device;
  SkipBlockDevice::Create(kNandInfo, &device);

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
            ZX_OK);
  ASSERT_OK(partitioner->WipeFvm());
}

TEST(SkipBlockDevicePartitionerTests, FinalizePartitionTest) {
  fbl::unique_ptr<SkipBlockDevice> device;
  SkipBlockDevice::Create(kNandInfo, &device);

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(device->devfs_root(), &partitioner),
            ZX_OK);

  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kBootloader));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconA));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconB));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kZirconR));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaA));
  ASSERT_OK(partitioner->FinalizePartition(paver::Partition::kVbMetaB));
}

TEST(SkipBlockDevicePartitionerTests, FindPartitionTest) {
  fbl::unique_ptr<SkipBlockDevice> device;
  SkipBlockDevice::Create(kNandInfo, &device);
  auto devfs_root = device->devfs_root();
  fbl::unique_ptr<BlockDevice> fvm;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devfs_root, kFvmType, &fvm));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(std::move(devfs_root), &partitioner),
            ZX_OK);

  fbl::unique_fd fd;
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kBootloader, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));

  ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
}

TEST(SkipBlockDevicePartitionerTests, GetBlockSizeTest) {
  fbl::unique_ptr<SkipBlockDevice> device;
  SkipBlockDevice::Create(kNandInfo, &device);
  auto devfs_root = device->devfs_root();
  fbl::unique_ptr<BlockDevice> fvm;
  ASSERT_NO_FATAL_FAILURES(BlockDevice::Create(devfs_root, kFvmType, &fvm));

  fbl::unique_ptr<paver::DevicePartitioner> partitioner;
  ASSERT_EQ(paver::SkipBlockDevicePartitioner::Initialize(std::move(devfs_root), &partitioner),
            ZX_OK);

  fbl::unique_fd fd;
  uint32_t block_size;
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kBootloader, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconA, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconB, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kZirconR, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaA, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);
  ASSERT_OK(partitioner->FindPartition(paver::Partition::kVbMetaB, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kPageSize * kPagesPerBlock);

  ASSERT_OK(partitioner->FindPartition(paver::Partition::kFuchsiaVolumeManager, &fd));
  ASSERT_OK(partitioner->GetBlockSize(fd, &block_size));
  ASSERT_EQ(block_size, kBlockSize);
}
