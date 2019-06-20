// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <lib/gtest/test_loop_fixture.h>
#include <zircon/types.h>

#include "src/developer/memory/metrics/capture.h"
#include "src/developer/memory/metrics/tests/test_utils.h"

namespace memory {
namespace test {

using CaptureUnitTest = gtest::TestLoopFixture;

const static zx_info_kmem_stats _kmem = {
  .total_bytes = 300,
  .free_bytes = 100,
  .wired_bytes = 10,
  .total_heap_bytes = 20,
  .free_heap_bytes = 30,
  .vmo_bytes = 40,
  .mmu_overhead_bytes = 50,
  .ipc_bytes = 60,
  .other_bytes = 70
};
const static GetInfoResponse kmem_info = {
  TestUtils::kRootHandle, ZX_INFO_KMEM_STATS, &_kmem, sizeof(_kmem), 1, ZX_OK
};

const static zx_info_handle_basic_t _self = {.koid = TestUtils::kSelfKoid };
const static GetInfoResponse self_info = {
  TestUtils::kSelfHandle, ZX_INFO_HANDLE_BASIC, &_self, sizeof(_self), 1, ZX_OK
};

const zx_koid_t proc_koid = 10;
const zx_handle_t proc_handle = 100;
const char proc_name[] = "P1";
const static zx_info_task_stats _proc = {};
const static GetInfoResponse proc_info =
    {proc_handle, ZX_INFO_TASK_STATS, &_proc, sizeof(_proc), 1, ZX_OK};
const static GetPropertyResponse proc_prop =
    {proc_handle, ZX_PROP_NAME, proc_name, sizeof(proc_name), ZX_OK};
const static GetProcessesCallback proc_cb =
    {1, proc_handle, proc_koid, 0};

const zx_koid_t proc2_koid = 20;
const zx_handle_t proc2_handle = 200;
const char proc2_name[] = "P2";
const static zx_info_task_stats _proc2 = {};
const static GetInfoResponse proc2_info =
    {proc2_handle, ZX_INFO_TASK_STATS, &_proc2, sizeof(_proc2), 1, ZX_OK};
const static GetPropertyResponse proc2_prop =
    {proc2_handle, ZX_PROP_NAME, proc2_name, sizeof(proc2_name), ZX_OK};
const static GetProcessesCallback proc2_cb =
    {1, proc2_handle, proc2_koid, 0};

const zx_koid_t vmo_koid = 1000;
const uint64_t vmo_size = 10000;
const char vmo_name[] = "V1";
const static zx_info_vmo_t _vmo = {
  .koid = vmo_koid,
  .name = "V1",
  .size_bytes = vmo_size,
};
const static GetInfoResponse vmos_info =
    {proc_handle, ZX_INFO_PROCESS_VMOS, &_vmo, sizeof(_vmo), 1, ZX_OK};

const zx_koid_t vmo2_koid = 2000;
const uint64_t vmo2_size = 20000;
const char vmo2_name[] = "V2";
const static zx_info_vmo_t _vmo2 = {
  .koid = vmo2_koid,
  .name = "V2",
  .size_bytes = vmo2_size,
};
const static GetInfoResponse vmos2_info =
    {proc2_handle, ZX_INFO_PROCESS_VMOS, &_vmo2, sizeof(_vmo2), 1, ZX_OK};

TEST_F(CaptureUnitTest, KMEM) {
  Capture c;
  auto ret = TestUtils::GetCapture(c, KMEM, {
    .get_info = {self_info, kmem_info},
  });
  EXPECT_EQ(ZX_OK, ret);
  auto const& got_kmem = c.kmem();
  EXPECT_EQ(_kmem.total_bytes, got_kmem.total_bytes);
}

TEST_F(CaptureUnitTest, PROCESS) {
  Capture c;
  auto ret = TestUtils::GetCapture(c, PROCESS, {
    .get_info = {self_info, kmem_info, proc_info},
    .get_processes = {{ZX_OK, {proc_cb}}},
    .get_property = {proc_prop}
  });
  EXPECT_EQ(ZX_OK, ret);
  EXPECT_EQ(1U, c.koid_to_process().size());
  auto const& process = c.process_for_koid(proc_koid);
  EXPECT_EQ(proc_koid, process.koid);
  EXPECT_STREQ(proc_name, process.name);
}

TEST_F(CaptureUnitTest, VMO) {
  Capture c;
  auto ret = TestUtils::GetCapture(c, VMO, {
    .get_info = {self_info, kmem_info, proc_info, vmos_info, vmos_info},
    .get_processes = {{ZX_OK, {proc_cb}}},
    .get_property = {proc_prop}
  });
  EXPECT_EQ(ZX_OK, ret);
  EXPECT_EQ(1U, c.koid_to_process().size());
  auto const& process = c.process_for_koid(proc_koid);
  EXPECT_EQ(proc_koid, process.koid);
  EXPECT_STREQ(proc_name, process.name);
  EXPECT_EQ(1U, process.vmos.size());
  EXPECT_EQ(1U, c.koid_to_vmo().size());
  EXPECT_EQ(vmo_koid, process.vmos[0]);
  auto const& vmo = c.vmo_for_koid(vmo_koid);
  EXPECT_EQ(vmo_koid, vmo.koid);
  EXPECT_EQ(vmo_size, vmo.size_bytes);
  EXPECT_STREQ(vmo_name, vmo.name);
}

TEST_F(CaptureUnitTest, VMODouble) {
  Capture c;
  auto ret = TestUtils::GetCapture(c, VMO, {
    .get_info = {
      self_info,
      kmem_info,
      proc_info,
      vmos_info,
      vmos_info,
      proc2_info,
      vmos2_info,
      vmos2_info,
    },
    .get_processes = {{ZX_OK, {proc_cb, proc2_cb}}},
    .get_property = {proc_prop, proc2_prop}
  });
  EXPECT_EQ(ZX_OK, ret);
  EXPECT_EQ(2U, c.koid_to_process().size());
  EXPECT_EQ(2U, c.koid_to_vmo().size());

  auto const& process = c.process_for_koid(proc_koid);
  EXPECT_EQ(proc_koid, process.koid);
  EXPECT_STREQ(proc_name, process.name);
  EXPECT_EQ(1U, process.vmos.size());
  EXPECT_EQ(vmo_koid, process.vmos[0]);
  auto const& vmo = c.vmo_for_koid(vmo_koid);
  EXPECT_EQ(vmo_koid, vmo.koid);
  EXPECT_EQ(vmo_size, vmo.size_bytes);
  EXPECT_STREQ(vmo_name, vmo.name);

  auto const& process2 = c.process_for_koid(proc2_koid);
  EXPECT_EQ(proc2_koid, process2.koid);
  EXPECT_STREQ(proc2_name, process2.name);
  EXPECT_EQ(1U, process2.vmos.size());
  EXPECT_EQ(vmo2_koid, process2.vmos[0]);
  auto const& vmo2 = c.vmo_for_koid(vmo2_koid);
  EXPECT_EQ(vmo2_koid, vmo2.koid);
  EXPECT_EQ(vmo2_size, vmo2.size_bytes);
  EXPECT_STREQ(vmo2_name, vmo2.name);
}

TEST_F(CaptureUnitTest, ProcessPropBadState) {
  // If the process disappears we should ignore it and continue.
  Capture c;
  auto ret = TestUtils::GetCapture(c, PROCESS, {
    .get_info = {self_info, kmem_info, proc2_info},
    .get_processes = {{ZX_OK, {proc_cb, proc2_cb}}},
    .get_property = {
      {proc_handle, ZX_PROP_NAME, nullptr, 0, ZX_ERR_BAD_STATE},
      proc2_prop
    }
  });
  EXPECT_EQ(ZX_OK, ret);
  EXPECT_EQ(1U, c.koid_to_process().size());
  auto const& process = c.process_for_koid(proc2_koid);
  EXPECT_EQ(proc2_koid, process.koid);
  EXPECT_STREQ(proc2_name, process.name);
}

TEST_F(CaptureUnitTest, ProcessInfoBadState) {
  // If the process disappears we should ignore it and continue.
  Capture c;
  auto ret = TestUtils::GetCapture(c, PROCESS, {
    .get_info = {
      self_info,
      kmem_info,
      {proc_handle, ZX_INFO_TASK_STATS,
       &_proc, sizeof(_proc), 1, ZX_ERR_BAD_STATE},
      proc2_info
    },
    .get_processes = {{ZX_OK, {proc_cb, proc2_cb}}},
    .get_property = {proc_prop, proc2_prop},
  });
  EXPECT_EQ(ZX_OK, ret);
  EXPECT_EQ(1U, c.koid_to_process().size());
  auto const& process = c.process_for_koid(proc2_koid);
  EXPECT_EQ(proc2_koid, process.koid);
  EXPECT_STREQ(proc2_name, process.name);
}

TEST_F(CaptureUnitTest, VMOCountBadState) {
  // If the process disappears we should ignore it and continue.
  Capture c;
  auto ret = TestUtils::GetCapture(c, VMO, {
    .get_info = {
      self_info,
      kmem_info,
      proc_info,
      {proc_handle, ZX_INFO_PROCESS_VMOS,
       &_vmo, sizeof(_vmo), 1, ZX_ERR_BAD_STATE},
      proc2_info,
      vmos2_info,
      vmos2_info
    },
    .get_processes = {{ZX_OK, {proc_cb, proc2_cb}}},
    .get_property = {proc_prop, proc2_prop}
  });
  EXPECT_EQ(ZX_OK, ret);
  EXPECT_EQ(1U, c.koid_to_process().size());
  auto const& process = c.process_for_koid(proc2_koid);
  EXPECT_EQ(proc2_koid, process.koid);
  EXPECT_STREQ(proc2_name, process.name);
  EXPECT_EQ(1U, process.vmos.size());
  EXPECT_EQ(1U, c.koid_to_vmo().size());
  EXPECT_EQ(vmo2_koid, process.vmos[0]);
  auto const& vmo = c.vmo_for_koid(vmo2_koid);
  EXPECT_EQ(vmo2_koid, vmo.koid);
  EXPECT_EQ(vmo2_size, vmo.size_bytes);
  EXPECT_STREQ(vmo2_name, vmo.name);
}

TEST_F(CaptureUnitTest, VMOGetBadState) {
  // If the process disappears we should ignore it and continue.
  Capture c;
  auto ret = TestUtils::GetCapture(c, VMO, {
    .get_info = {
      self_info,
      kmem_info,
      proc_info,
      vmos_info,
      {proc_handle, ZX_INFO_PROCESS_VMOS,
       &_vmo, sizeof(_vmo), 1, ZX_ERR_BAD_STATE},
      proc2_info,
      vmos2_info,
      vmos2_info
    },
    .get_processes = {{ZX_OK, {proc_cb, proc2_cb}}},
    .get_property = {proc_prop, proc2_prop}
  });
  EXPECT_EQ(ZX_OK, ret);
  EXPECT_EQ(1U, c.koid_to_process().size());
  auto const& process = c.process_for_koid(proc2_koid);
  EXPECT_EQ(proc2_koid, process.koid);
  EXPECT_STREQ(proc2_name, process.name);
  EXPECT_EQ(1U, process.vmos.size());
  EXPECT_EQ(1U, c.koid_to_vmo().size());
  EXPECT_EQ(vmo2_koid, process.vmos[0]);
  auto const& vmo = c.vmo_for_koid(vmo2_koid);
  EXPECT_EQ(vmo2_koid, vmo.koid);
  EXPECT_EQ(vmo2_size, vmo.size_bytes);
  EXPECT_STREQ(vmo2_name, vmo.name);
}

}  // namespace test
}  // namespace memory
