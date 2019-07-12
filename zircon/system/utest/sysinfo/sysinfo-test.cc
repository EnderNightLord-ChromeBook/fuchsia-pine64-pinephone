// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <unistd.h>

#include <fbl/unique_fd.h>
#include <fuchsia/sysinfo/c/fidl.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/directory.h>
#include <lib/zx/channel.h>
#include <lib/zx/handle.h>
#include <zircon/boot/image.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>
#include <zxtest/zxtest.h>

namespace sysinfo {

namespace {

constexpr char kSysinfoPath[] = "/dev/misc/sysinfo";

}  // namespace

TEST(SysinfoTest, GetRootResource) {
    // Get the resource handle from the driver.
    fbl::unique_fd fd(open(kSysinfoPath, O_RDWR));
    ASSERT_TRUE(fd.is_valid(), "Can't open sysinfo");

    zx::channel channel;
    ASSERT_OK(fdio_get_service_handle(fd.release(), channel.reset_and_get_address()),
              "Failed to get channel");

    zx::handle root_resource;
    zx_status_t status;
    ASSERT_OK(fuchsia_sysinfo_DeviceGetRootResource(channel.get(), &status,
                                                    root_resource.reset_and_get_address()),
              "Failed to get root resource");
    ASSERT_OK(status, "Failed to get root resource");

    // Make sure it's a resource with the expected rights.
    zx_info_handle_basic_t info;
    ASSERT_OK(root_resource.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr),
              "Can't get handle info");
    EXPECT_EQ(info.type, ZX_OBJ_TYPE_RESOURCE, "Unexpected type");
    EXPECT_EQ(info.rights, ZX_RIGHT_TRANSFER, "Unexpected rights");
}

TEST(SysinfoTest, GetBoardName) {
    // Get the resource handle from the driver.
    fbl::unique_fd fd(open(kSysinfoPath, O_RDWR));
    ASSERT_TRUE(fd.is_valid(), "Can't open sysinfo");

    zx::channel channel;
    ASSERT_OK(fdio_get_service_handle(fd.release(), channel.reset_and_get_address()),
              "Failed to get channel");

    // Test fuchsia_sysinfo_DeviceGetBoardName().
    char board_name[ZBI_BOARD_NAME_LEN];
    zx_status_t status;
    size_t actual_size;
    zx_status_t fidl_status = fuchsia_sysinfo_DeviceGetBoardName(channel.get(), &status, board_name,
                                                                sizeof(board_name), &actual_size);
    ASSERT_OK(fidl_status, "Failed to get board name");
    ASSERT_OK(status, "Failed to get board name");
    ASSERT_LE(actual_size, sizeof(board_name), "GetBoardName returned too much data");
    EXPECT_GT(strlen(board_name), 0, "board name is empty");
}

TEST(SysinfoTest, GetInterruptControllerInfo) {
    // Get the resource handle from the driver.
    fbl::unique_fd fd(open(kSysinfoPath, O_RDWR));
    ASSERT_TRUE(fd.is_valid(), "Can't open sysinfo");

    zx::channel channel;
    ASSERT_OK(fdio_get_service_handle(fd.release(), channel.reset_and_get_address()),
              "Failed to get channel");

    // Test fuchsia_sysinfo_DeviceGetInterruptControllerInfo().
    fuchsia_sysinfo_InterruptControllerInfo info;
    zx_status_t status;
    ASSERT_OK(fuchsia_sysinfo_DeviceGetInterruptControllerInfo(channel.get(), &status, &info),
              "Failed to get interrupt controller info");
    ASSERT_OK(status, "Failed to get interrupt controller info");
    EXPECT_NE(info.type, fuchsia_sysinfo_InterruptControllerType_UNKNOWN,
              "interrupt controller type is unknown");
}

}  // namespace sysinfo
