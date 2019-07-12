// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/arch_x64_helpers.h"

#include <gtest/gtest.h>

#include "src/developer/debug/ipc/register_test_support.h"
#include "src/developer/debug/shared/arch_x86.h"
#include "src/developer/debug/shared/logging/file_line_function.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/logging.h"

using namespace debug_ipc;

namespace debug_agent {
namespace arch {

namespace {

void SetupHWBreakpointTest(debug_ipc::FileLineFunction file_line,
                           zx_thread_state_debug_regs_t* debug_regs,
                           uint64_t address, zx_status_t expected_result) {
  zx_status_t result = SetupHWBreakpoint(address, debug_regs);
  ASSERT_EQ(result, expected_result)
      << "[" << file_line.ToString() << "] "
      << "Got: " << debug_ipc::ZxStatusToString(result)
      << ", expected: " << debug_ipc::ZxStatusToString(expected_result);
}

void RemoveHWBreakpointTest(debug_ipc::FileLineFunction file_line,
                            zx_thread_state_debug_regs_t* debug_regs,
                            uint64_t address, zx_status_t expected_result) {
  zx_status_t result = RemoveHWBreakpoint(address, debug_regs);
  ASSERT_EQ(result, expected_result)
      << "[" << file_line.ToString() << "] "
      << "Got: " << debug_ipc::ZxStatusToString(result)
      << ", expected: " << debug_ipc::ZxStatusToString(expected_result);
}

void SetupWatchpointTest(debug_ipc::FileLineFunction file_line,
                         zx_thread_state_debug_regs_t* debug_regs,
                         uint64_t address, zx_status_t expected_result) {
  zx_status_t result = SetupWatchpoint(address, debug_regs);
  ASSERT_EQ(result, expected_result)
      << "[" << file_line.ToString() << "] "
      << "Got: " << debug_ipc::ZxStatusToString(result)
      << ", expected: " << debug_ipc::ZxStatusToString(expected_result);
}

void RemoveWatchpointTest(debug_ipc::FileLineFunction file_line,
                          zx_thread_state_debug_regs_t* debug_regs,
                          uint64_t address, zx_status_t expected_result) {
  zx_status_t result = RemoveWatchpoint(address, debug_regs);
  ASSERT_EQ(result, expected_result)
      << "[" << file_line.ToString() << "] "
      << "Got: " << debug_ipc::ZxStatusToString(result)
      << ", expected: " << debug_ipc::ZxStatusToString(expected_result);
}

uint64_t GetHWBreakpointDR7Mask(size_t index) {
  FXL_DCHECK(index < 4);
  // Mask is: L = 1, RW = 00, LEN = 0
  static uint64_t dr_masks[4] = {
      X86_FLAG_MASK(DR7L0),
      X86_FLAG_MASK(DR7L1),
      X86_FLAG_MASK(DR7L2),
      X86_FLAG_MASK(DR7L3),
  };
  return dr_masks[index];
}

uint64_t GetWatchpointDR7Mask(size_t index) {
  FXL_DCHECK(index < 4);
  FXL_DCHECK(index < 4);
  // Mask is: L = 1, RW = 0b01, LEN = 10 (8 bytes).
  static uint64_t masks[4] = {
      X86_FLAG_MASK(DR7L0) | 0b01 << kDR7RW0Shift | 0b10 << kDR7LEN0Shift,
      X86_FLAG_MASK(DR7L1) | 0b01 << kDR7RW1Shift | 0b10 << kDR7LEN1Shift,
      X86_FLAG_MASK(DR7L2) | 0b01 << kDR7RW2Shift | 0b10 << kDR7LEN2Shift,
      X86_FLAG_MASK(DR7L3) | 0b01 << kDR7RW3Shift | 0b10 << kDR7LEN3Shift,
  };
  return masks[index];
}

// Merges into |val| the flag values for active hw breakpoints within |indices|.
uint64_t JoinDR7HWBreakpointMask(uint64_t val,
                                 std::initializer_list<size_t> indices = {}) {
  for (size_t index : indices) {
    FXL_DCHECK(index < 4);
    val |= GetHWBreakpointDR7Mask(index);
  }

  return val;
}

// Merges into |val| the flag values for active watchpoints within |indices|.
uint64_t JoinDR7WatchpointMask(uint64_t val,
                               std::initializer_list<size_t> indices = {}) {
  for (size_t index : indices) {
    FXL_DCHECK(index < 4);
    val |= GetWatchpointDR7Mask(index);
  }

  return val;
}

constexpr uint64_t kAddress1 = 0x0123;
constexpr uint64_t kAddress2 = 0x4567;
constexpr uint64_t kAddress3 = 0x89ab;
constexpr uint64_t kAddress4 = 0xcdef;
constexpr uint64_t kAddress5 = 0xdeadbeef;

}  // namespace

// General Registers -----------------------------------------------------------

TEST(x64Helpers, WritingGeneralRegs) {
  std::vector<debug_ipc::Register> regs;
  regs.push_back(CreateRegisterWithData(debug_ipc::RegisterID::kX64_rax, 8));
  regs.push_back(CreateRegisterWithData(debug_ipc::RegisterID::kX64_rbx, 8));
  regs.push_back(CreateRegisterWithData(debug_ipc::RegisterID::kX64_r14, 8));
  regs.push_back(CreateRegisterWithData(debug_ipc::RegisterID::kX64_rflags, 8));

  zx_thread_state_general_regs_t out = {};
  zx_status_t res = WriteGeneralRegisters(regs, &out);
  ASSERT_EQ(res, ZX_OK) << "Expected ZX_OK, got "
                        << debug_ipc::ZxStatusToString(res);

  EXPECT_EQ(out.rax, 0x0102030405060708u);
  EXPECT_EQ(out.rbx, 0x0102030405060708u);
  EXPECT_EQ(out.rcx, 0u);
  EXPECT_EQ(out.rdx, 0u);
  EXPECT_EQ(out.rsi, 0u);
  EXPECT_EQ(out.rdi, 0u);
  EXPECT_EQ(out.rbp, 0u);
  EXPECT_EQ(out.rsp, 0u);
  EXPECT_EQ(out.r8, 0u);
  EXPECT_EQ(out.r9, 0u);
  EXPECT_EQ(out.r10, 0u);
  EXPECT_EQ(out.r11, 0u);
  EXPECT_EQ(out.r12, 0u);
  EXPECT_EQ(out.r13, 0u);
  EXPECT_EQ(out.r14, 0x0102030405060708u);
  EXPECT_EQ(out.r15, 0u);
  EXPECT_EQ(out.rip, 0u);
  EXPECT_EQ(out.rflags, 0x0102030405060708u);

  regs.clear();
  regs.push_back(CreateUint64Register(debug_ipc::RegisterID::kX64_rax, 0xaabb));
  regs.push_back(CreateUint64Register(debug_ipc::RegisterID::kX64_rdx, 0xdead));
  regs.push_back(CreateUint64Register(debug_ipc::RegisterID::kX64_r10, 0xbeef));

  res = WriteGeneralRegisters(regs, &out);
  ASSERT_EQ(res, ZX_OK) << "Expected ZX_OK, got "
                        << debug_ipc::ZxStatusToString(res);

  EXPECT_EQ(out.rax, 0xaabbu);
  EXPECT_EQ(out.rbx, 0x0102030405060708u);
  EXPECT_EQ(out.rcx, 0u);
  EXPECT_EQ(out.rdx, 0xdeadu);
  EXPECT_EQ(out.rsi, 0u);
  EXPECT_EQ(out.rdi, 0u);
  EXPECT_EQ(out.rbp, 0u);
  EXPECT_EQ(out.rsp, 0u);
  EXPECT_EQ(out.r8, 0u);
  EXPECT_EQ(out.r9, 0u);
  EXPECT_EQ(out.r10, 0xbeefu);
  EXPECT_EQ(out.r11, 0u);
  EXPECT_EQ(out.r12, 0u);
  EXPECT_EQ(out.r13, 0u);
  EXPECT_EQ(out.r14, 0x0102030405060708u);
  EXPECT_EQ(out.r15, 0u);
  EXPECT_EQ(out.rip, 0u);
  EXPECT_EQ(out.rflags, 0x0102030405060708u);
}

TEST(x64Helpers, InvalidWritingGeneralRegs) {
  zx_thread_state_general_regs_t out;
  std::vector<debug_ipc::Register> regs;

  // Invalid length.
  regs.push_back(CreateRegisterWithData(debug_ipc::RegisterID::kX64_rax, 4));
  EXPECT_EQ(WriteGeneralRegisters(regs, &out), ZX_ERR_INVALID_ARGS);

  // Invalid register.
  regs.push_back(CreateRegisterWithData(debug_ipc::RegisterID::kX64_ymm2, 8));
  EXPECT_EQ(WriteGeneralRegisters(regs, &out), ZX_ERR_INVALID_ARGS);
}

// HW Breakpoints --------------------------------------------------------------

TEST(x64Helpers, SettingHWBreakpoints) {
  zx_thread_state_debug_regs_t debug_regs = {};

  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], 0u);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0}));

  // Adding the same breakpoint should detect that the same already exists.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1,
                        ZX_ERR_ALREADY_BOUND);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], 0u);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0}));

  // Continuing adding should append.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress2, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1}));

  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], kAddress3);
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 2}));

  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress4, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], kAddress3);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 2, 3}));

  // No more registers left should not change anything.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5,
                        ZX_ERR_NO_RESOURCES);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], kAddress3);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 2, 3}));
}

TEST(x64Helpers, RemovingHWBreakpoint) {
  zx_thread_state_debug_regs_t debug_regs = {};

  // Previous state verifies the state of this calls.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress2, ZX_OK);
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3, ZX_OK);
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress4, ZX_OK);
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5,
                        ZX_ERR_NO_RESOURCES);

  RemoveHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 3}));

  // Removing same breakpoint should not work.
  RemoveHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3,
                         ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 3}));

  // Removing an unknown address should warn and change nothing.
  RemoveHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, 0xaaaaaaa,
                         ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(debug_regs.dr[0], kAddress1);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 3}));

  RemoveHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], 0u);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {1, 3}));

  // Adding again should work.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], kAddress5);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 3}));

  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], kAddress5);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], kAddress1);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 2, 3}));

  // Already exists should not change.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5,
                        ZX_ERR_ALREADY_BOUND);
  EXPECT_EQ(debug_regs.dr[0], kAddress5);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], kAddress1);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 2, 3}));

  // No more resources.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3,
                        ZX_ERR_NO_RESOURCES);
  EXPECT_EQ(debug_regs.dr[0], kAddress5);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], kAddress1);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 2, 3}));

  // Attempting to remove a watchpoint should not work.
  RemoveWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3,
                       ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(debug_regs.dr[0], kAddress5);
  EXPECT_EQ(debug_regs.dr[1], kAddress2);
  EXPECT_EQ(debug_regs.dr[2], kAddress1);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7HWBreakpointMask(0, {0, 1, 2, 3}));
}

// Watchpoints -----------------------------------------------------------------

namespace {

inline uint64_t AlignedAddress(uint64_t address) { return address & ~0b111; }

}  // namespace

TEST(x64Helpers, SettingWatchpoints) {
  zx_thread_state_debug_regs_t debug_regs = {};

  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], 0u);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7WatchpointMask(0, {0}));

  // Adding the same breakpoint should detect that the same already exists.
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1,
                      ZX_ERR_ALREADY_BOUND);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], 0u);
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7WatchpointMask(0, {0}));

  // Continuing adding should append.
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress2, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7WatchpointMask(0, {0, 1}));

  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], AlignedAddress(kAddress3));
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  EXPECT_EQ(debug_regs.dr7, JoinDR7WatchpointMask(0, {0, 1, 2}));

  // Setup a HW breakpoint also.
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress4, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], AlignedAddress(kAddress3));
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1, 2});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // No more registers left should not change anything.
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5,
                      ZX_ERR_NO_RESOURCES);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], AlignedAddress(kAddress3));
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1, 2});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }
}

TEST(x64Helpers, RemovingWatchpoints) {
  zx_thread_state_debug_regs_t debug_regs = {};

  // Previous state verifies the state of this calls.
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress2, ZX_OK);
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3, ZX_OK);
  SetupHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress4, ZX_OK);
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5,
                      ZX_ERR_NO_RESOURCES);

  RemoveWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // Removing same watchpoint should not work.
  RemoveWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3,
                       ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // Removing an unknown address should warn and change nothing.
  RemoveWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, 0xaaaaaaa,
                       ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // Attempting to remove a HW breakpoint should not work.
  RemoveHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1,
                         ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    ASSERT_EQ(debug_regs.dr7, mask);
  }

  RemoveWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], 0u);
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {1});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // Adding again should work.
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress5));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], 0u);
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress1, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress5));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1, 2});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // Already exists should not change.
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress5,
                      ZX_ERR_ALREADY_BOUND);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress5));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1, 2});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // No more resources.
  SetupWatchpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress3,
                      ZX_ERR_NO_RESOURCES);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress5));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[3], kAddress4);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1, 2});
    mask = JoinDR7HWBreakpointMask(mask, {3});
    EXPECT_EQ(debug_regs.dr7, mask);
  }

  // Remove the breakpoint.
  RemoveHWBreakpointTest(FROM_HERE_NO_FUNC, &debug_regs, kAddress4, ZX_OK);
  EXPECT_EQ(debug_regs.dr[0], AlignedAddress(kAddress5));
  EXPECT_EQ(debug_regs.dr[1], AlignedAddress(kAddress2));
  EXPECT_EQ(debug_regs.dr[2], AlignedAddress(kAddress1));
  EXPECT_EQ(debug_regs.dr[3], 0u);
  EXPECT_EQ(debug_regs.dr6, 0u);
  {
    uint64_t mask = JoinDR7WatchpointMask(0, {0, 1, 2});
    EXPECT_EQ(debug_regs.dr7, mask);
  }
}

}  // namespace arch
}  // namespace debug_agent
