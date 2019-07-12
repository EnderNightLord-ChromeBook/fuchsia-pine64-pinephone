// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_LIB_COROUTINE_CONTEXT_X64_CONTEXT_H_
#define SRC_LEDGER_LIB_COROUTINE_CONTEXT_X64_CONTEXT_H_

// Offset of all saved registers.
#define RBX_O 0
#define RBP_O 8
#define R12_O 16
#define R13_O 24
#define R14_O 32
#define R15_O 40
#define RDI_O 48
#define RSP_O 56
#define RIP_O 64

#if __has_feature(safe_stack)
#define UNSAFE_SP_O 72
#endif

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

namespace context {

enum Register {
  REG_RBX = 0,
  REG_RBP,
  REG_R12,
  REG_R13,
  REG_R14,
  REG_R15,
  REG_RDI,
  REG_RSP,
  REG_RIP,
#if __has_feature(safe_stack)
  REG_UNSAFE_SP,
#endif
  NUM_REGISTERS,

  // Special registers.
  REG_ARG0 = REG_RDI,
  REG_LR = REG_RIP,
  REG_SP = REG_RSP,
};

inline constexpr size_t kAdditionalStackAlignment = 8;

struct InternalContext {
  uint64_t registers[NUM_REGISTERS];
};

#define ASSERT_REGISTER_OFFSET(REG)                                                         \
  static_assert(offsetof(struct InternalContext, registers[context::REG_##REG]) == REG##_O, \
                "offset is incorrect")

ASSERT_REGISTER_OFFSET(RBX);
ASSERT_REGISTER_OFFSET(RBP);
ASSERT_REGISTER_OFFSET(R12);
ASSERT_REGISTER_OFFSET(R13);
ASSERT_REGISTER_OFFSET(R14);
ASSERT_REGISTER_OFFSET(R15);
ASSERT_REGISTER_OFFSET(RDI);
ASSERT_REGISTER_OFFSET(RSP);
ASSERT_REGISTER_OFFSET(RIP);
#if __has_feature(safe_stack)
ASSERT_REGISTER_OFFSET(UNSAFE_SP);
#endif

}  // namespace context

#endif  // __ASSEMBLER__

#endif  // SRC_LEDGER_LIB_COROUTINE_CONTEXT_X64_CONTEXT_H_
