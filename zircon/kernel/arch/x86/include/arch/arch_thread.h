// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2009 Corey Tabaka
// Copyright (c) 2015 Intel Corporation
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_ARCH_THREAD_H_
#define ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_ARCH_THREAD_H_

#include <assert.h>
#include <sys/types.h>
#include <zircon/compiler.h>

#include <arch/x86/registers.h>

__BEGIN_CDECLS

struct arch_thread {
  vaddr_t sp;
#if __has_feature(safe_stack)
  vaddr_t unsafe_sp;
#endif
  vaddr_t fs_base;
  vaddr_t gs_base;

  // Which entry of |suspended_general_regs| to use.
  // One of X86_GENERAL_REGS_*.
  uint32_t general_regs_source;

  // Debugger access to userspace general regs while suspended or stopped
  // in an exception. See the description of X86_GENERAL_REGS_* for usage.
  // The regs are saved on the stack and then a pointer is stored here.
  // Nullptr if not suspended or not stopped in an exception.
  // TODO(ZX-563): Also nullptr for synthetic exceptions that don't provide
  // them yet.
  union {
    void *gregs;
    x86_syscall_general_regs_t *syscall;
    x86_iframe_t *iframe;
  } suspended_general_regs;

  /* Buffer to save fpu and extended register (e.g., PT) state */
  void *extended_register_state;
  uint8_t extended_register_buffer[X86_MAX_EXTENDED_REGISTER_SIZE + 64];

  /* if non-NULL, address to return to on page fault */
  void *page_fault_resume;

  /* |track_debug_state| tells whether the kernel should keep track of the whole debug state for
   * this thread. Normally this is set explicitly by an user that wants to make use of HW
   * breakpoints or watchpoints.
   * |debug_state| will still keep track of the status of the exceptions (DR6), as there are HW
   * exceptions that are triggered without explicit debug state setting (eg. single step).
   *
   * Userspace can still read the complete |debug_state| even if |track_debug_state| is false.
   * As normally the CPU only changes DR6, the |debug_state| will be up to date anyway. */
  bool track_debug_state;
  x86_debug_state_t debug_state;
};

static inline void x86_set_suspended_general_regs(struct arch_thread *thread, uint32_t source,
                                                  void *gregs) {
  DEBUG_ASSERT(thread->suspended_general_regs.gregs == NULL);
  DEBUG_ASSERT(gregs != NULL);
  DEBUG_ASSERT(source != X86_GENERAL_REGS_NONE);
  thread->general_regs_source = source;
  thread->suspended_general_regs.gregs = gregs;
}

static inline void x86_reset_suspended_general_regs(struct arch_thread *thread) {
  thread->general_regs_source = X86_GENERAL_REGS_NONE;
  thread->suspended_general_regs.gregs = NULL;
}

__END_CDECLS

#endif  // ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_ARCH_THREAD_H_
