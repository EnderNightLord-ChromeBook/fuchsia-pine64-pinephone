// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "helper/platform_device_helper.h"
#include "msd_intel_gen_query.h"
#include "sys_driver/magma_driver.h"
#include "sys_driver/magma_system_connection.h"
#include "sys_driver/magma_system_context.h"
#include "sys_driver/magma_system_device.h"

// This test is meant to run on all devices and exercise
// the execution of command buffers from multiple connections
// simultaneously.  So doing requires some device specific knowledge
// (for example what instructions to put into the command buffer);
// and that may not be easily achieved so in practice this test
// may bail out early on some devices.
class TestMultithread {
 public:
  static std::unique_ptr<TestMultithread> Create() {
    auto driver = MagmaDriver::Create();
    if (!driver)
      return DRETP(nullptr, "no driver");

    auto device = driver->CreateDevice(GetTestDeviceHandle());
    if (!device)
      return DRETP(nullptr, "no device");

    return std::make_unique<TestMultithread>(std::move(driver),
                                             std::shared_ptr<MagmaSystemDevice>(std::move(device)));
  }

  TestMultithread(std::unique_ptr<MagmaDriver> driver, std::shared_ptr<MagmaSystemDevice> device)
      : driver_(std::move(driver)), device_(std::move(device)) {}

  void Test(uint32_t num_threads) {
    std::vector<std::thread> threads;

    for (uint32_t i = 0; i < num_threads; i++) {
      std::thread connection_thread(ConnectionThreadEntry, this);
      threads.emplace_back(std::move(connection_thread));
    }

    for (auto& thread : threads) {
      ASSERT_TRUE(thread.joinable());
      thread.join();
    }
  }

  static void ConnectionThreadEntry(TestMultithread* test) {
    return test->ConnectionThreadLoop(100);
  }

  void ConnectionThreadLoop(uint32_t num_iterations) {
    auto connection = std::make_unique<MagmaSystemConnection>(
        device_, MsdConnectionUniquePtr(msd_device_open(device_->msd_dev(), 0)));
    ASSERT_NE(connection, nullptr);

    uint64_t extra_page_count;
    EXPECT_EQ(MAGMA_STATUS_OK, msd_device_query(device_->msd_dev(), kMsdIntelGenQueryExtraPageCount,
                                                &extra_page_count));

    uint32_t context_id = ++context_id_;
    EXPECT_TRUE(connection->CreateContext(context_id));
    auto context = connection->LookupContext(context_id);
    ASSERT_NE(context, nullptr);

    uint64_t gpu_addr = 0;

    for (uint32_t i = 0; i < num_iterations; i++) {
      auto batch_buffer = magma::PlatformBuffer::Create(PAGE_SIZE, "test");

      uint32_t handle;
      EXPECT_TRUE(batch_buffer->duplicate_handle(&handle));

      uint64_t id;
      EXPECT_TRUE(connection->ImportBuffer(handle, &id));
      EXPECT_EQ(id, batch_buffer->id());

      if (!InitBatchBuffer(batch_buffer.get()))
        break;  // Abort the test

      EXPECT_TRUE(connection->MapBufferGpu(id, gpu_addr, 0, batch_buffer->size() / PAGE_SIZE, 0));
      gpu_addr += batch_buffer->size() + extra_page_count * PAGE_SIZE;

      auto command_buffer = magma::PlatformBuffer::Create(PAGE_SIZE, "test");

      EXPECT_TRUE(InitCommandBuffer(command_buffer.get(), id));

      EXPECT_TRUE(context->ExecuteCommandBuffer(std::move(command_buffer)));
    }
  }

  bool InitCommandBuffer(magma::PlatformBuffer* buffer, uint64_t batch_buffer_id) {
    void* vaddr;
    if (!buffer->MapCpu(&vaddr))
      return DRETF(false, "couldn't map buffer");

    auto command_buffer = reinterpret_cast<struct magma_system_command_buffer*>(vaddr);
    command_buffer->batch_buffer_resource_index = 0;
    command_buffer->batch_start_offset = 0;
    command_buffer->num_resources = 1;

    auto exec_resource = reinterpret_cast<struct magma_system_exec_resource*>(command_buffer + 1);
    exec_resource->buffer_id = batch_buffer_id;
    exec_resource->offset = 0;
    exec_resource->length = buffer->size();

    if (!buffer->UnmapCpu())
      return DRETF(false, "couldn't unmap buffer");

    return true;
  }

  bool InitBatchBuffer(magma::PlatformBuffer* buffer) {
    if (!TestPlatformPciDevice::is_intel_gen(device_->GetDeviceId()))
      return DRETF(false, "not an intel gen9 device");

    void* vaddr;
    if (!buffer->MapCpu(&vaddr))
      return DRETF(false, "couldn't map buffer");

    *reinterpret_cast<uint32_t*>(vaddr) = 0xA << 23;

    if (!buffer->UnmapCpu())
      return DRETF(false, "couldn't unmap buffer");

    return true;
  }

 private:
  std::unique_ptr<MagmaDriver> driver_;
  std::shared_ptr<MagmaSystemDevice> device_;
  uint32_t context_id_ = 0;
};

TEST(MagmaSystem, Multithread) {
  auto test = TestMultithread::Create();
  ASSERT_NE(test, nullptr);
  test->Test(2);
}
