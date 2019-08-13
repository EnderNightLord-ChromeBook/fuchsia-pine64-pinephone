// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_SYSCALLS_PRIV_H_
#define ZIRCON_KERNEL_SYSCALLS_PRIV_H_

#include <lib/user_copy/user_ptr.h>
#include <zircon/syscalls/types.h>
#include <zircon/types.h>

// Forward declaration so it can be used in abigen-generated sys_* prototypes.
class user_out_handle;

#define ZX_SYSCALL_PARAM_ATTR(x)
#include <zircon/syscall-definitions.h>

#include <object/handle.h>
#include <object/process_dispatcher.h>

// This is the type of handle result parameters in system call
// implementation functions (sys_*).  abigen recognizes return values of
// type zx_handle_t and converts them into user_out_handle* instead of into
// user_out_ptr<zx_handle_t>.  System call implementation functions use the
// make, dup, or transfer method to turn a Dispatcher pointer or another
// handle into a handle received by the user.
class user_out_handle final {
 public:
  zx_status_t make(fbl::RefPtr<Dispatcher> dispatcher, zx_rights_t rights) {
    h_ = Handle::Make(ktl::move(dispatcher), rights);
    return h_ ? ZX_OK : ZX_ERR_NO_MEMORY;
  }

  // Note that if this call fails to allocate the Handle, the underlying
  // Dispatcher's on_zero_handles() will be called.
  zx_status_t make(KernelHandle<Dispatcher> handle, zx_rights_t rights) {
    h_ = Handle::Make(ktl::move(handle), rights);
    return h_ ? ZX_OK : ZX_ERR_NO_MEMORY;
  }

  zx_status_t dup(Handle* source, zx_rights_t rights) {
    h_ = Handle::Dup(source, rights);
    return h_ ? ZX_OK : ZX_ERR_NO_MEMORY;
  }

  zx_status_t transfer(HandleOwner&& source) {
    h_.swap(source);
    return ZX_OK;
  }

  // These methods are called by the abigen-generated wrapper_* functions
  // (syscall-kernel-wrappers.inc).  See KernelWrapperGenerator::syscall.

  bool begin_copyout(ProcessDispatcher* current_process, user_out_ptr<zx_handle_t> out) const {
    if (h_)
      return out.copy_to_user(current_process->MapHandleToValue(h_));
    return false;
  }

  void finish_copyout(ProcessDispatcher* current_process) {
    if (h_)
      current_process->AddHandle(ktl::move(h_));
  }

 private:
  HandleOwner h_;
};

#endif  // ZIRCON_KERNEL_SYSCALLS_PRIV_H_
