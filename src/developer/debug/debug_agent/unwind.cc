// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/unwind.h"

#include <inttypes.h>
#include <ngunwind/fuchsia.h>
#include <ngunwind/libunwind.h>

#include <algorithm>

#include "garnet/third_party/libunwindstack/fuchsia/MemoryFuchsia.h"
#include "garnet/third_party/libunwindstack/fuchsia/RegsFuchsia.h"
#include "garnet/third_party/libunwindstack/include/unwindstack/Unwinder.h"
#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/process_info.h"

namespace debug_agent {

namespace {

using ModuleVector = std::vector<debug_ipc::Module>;

// Default unwinder type to use.
UnwinderType unwinder_type = UnwinderType::kNgUnwind;

zx_status_t UnwindStackAndroid(const zx::process& process,
                               uint64_t dl_debug_addr, const zx::thread& thread,
                               const zx_thread_state_general_regs& regs,
                               size_t max_depth,
                               std::vector<debug_ipc::StackFrame>* stack) {
  // Ignore errors getting modules, the empty case can at least give the
  // current location, and maybe more if there are stack pointers.
  ModuleVector modules;  // Sorted by load address.
  GetModulesForProcess(process, dl_debug_addr, &modules);
  std::sort(modules.begin(), modules.end(),
            [](auto& a, auto& b) { return a.base < b.base; });

  unwindstack::Maps maps;
  for (size_t i = 0; i < modules.size(); i++) {
    // Our module currently doesn't have a size so just report the next
    // address boundary.
    // TODO(brettw) hook up the real size.
    uint64_t end;
    if (i < modules.size() - 1)
      end = modules[i + 1].base;
    else
      end = std::numeric_limits<uint64_t>::max();

    // The offset of the module is the offset in the file where the memory map
    // starts. For libraries, we can currently always assume 0.
    uint64_t offset = 0;

    uint64_t flags = 0;  // We don't have flags.

    // Don't know what this is, it's not set by the Android impl that reads
    // from /proc.
    uint64_t load_bias = 0;

    maps.Add(modules[i].base, end, offset, flags, modules[i].name, load_bias);
  }

  unwindstack::RegsFuchsia unwind_regs;
  unwind_regs.Set(regs);

  auto memory = std::make_shared<unwindstack::MemoryFuchsia>(process.get());

  // Always ask for one more frame than requested so we can get the canonical
  // frame address for the frames we do return (the CFA is the previous frame's
  // stack pointer at the time of the call).
  unwindstack::Unwinder unwinder(max_depth + 1, &maps, &unwind_regs,
                                 std::move(memory), true);
  // We don't need names from the unwinder since those are computed in the client. This will
  // generally fail anyway since the target binaries don't usually have symbols, so turning off
  // makes it a little more efficient.
  unwinder.SetResolveNames(false);

  unwinder.Unwind();

  stack->reserve(unwinder.NumFrames());
  for (size_t i = 0; i < unwinder.NumFrames(); i++) {
    const auto& src = unwinder.frames()[i];

    if (i > 0) {
      // The next frame's canonical frame address is our stack pointer.
      debug_ipc::StackFrame* next_frame = &(*stack)[i - 1];
      next_frame->cfa = src.sp;
    }

    // This termination condition is in the middle here because we don't know
    // for sure if the unwinder was able to return the number of frames we
    // requested, and we always want to fill in the CFA (above) for the
    // returned frames if possible.
    if (i == max_depth)
      break;

    debug_ipc::StackFrame* dest = &stack->emplace_back();
    dest->ip = src.pc;
    dest->sp = src.sp;
    if (src.regs) {
      src.regs->IterateRegisters([&dest](const char* name, uint64_t val) {
        // TODO(sadmac): It'd be nice to be using some sort of ID constant
        // instead of a converted string here.
        auto id = debug_ipc::StringToRegisterID(name);
        if (id != debug_ipc::RegisterID::kUnknown) {
          dest->regs.emplace_back(id, val);
        }
      });
    }
  }

  return 0;
}

using ModuleVector = std::vector<debug_ipc::Module>;

// Callback for ngunwind.
int LookupDso(void* context, unw_word_t pc, unw_word_t* base,
              const char** name) {
  // Context is a ModuleVector sorted by load address, need to find the
  // largest one smaller than or equal to the pc.
  //
  // We could use lower_bound for better perf with lots of modules but we
  // expect O(10) modules.
  const ModuleVector* modules = static_cast<const ModuleVector*>(context);
  for (int i = static_cast<int>(modules->size()) - 1; i >= 0; i--) {
    const debug_ipc::Module& module = (*modules)[i];
    if (pc >= module.base) {
      *base = module.base;
      *name = module.name.c_str();
      return 1;
    }
  }
  return 0;
}

zx_status_t UnwindStackNgUnwind(const zx::process& process,
                                uint64_t dl_debug_addr,
                                const zx::thread& thread,
                                const zx_thread_state_general_regs& regs,
                                size_t max_depth,
                                std::vector<debug_ipc::StackFrame>* stack) {
  stack->clear();

  // Ignore errors getting modules, the empty case can at least give the
  // current location, and maybe more if there are stack pointers.
  ModuleVector modules;  // Sorted by load address.
  GetModulesForProcess(process, dl_debug_addr, &modules);
  std::sort(modules.begin(), modules.end(),
            [](auto& a, auto& b) { return a.base < b.base; });

  unw_fuchsia_info_t* fuchsia =
      unw_create_fuchsia(process.get(), thread.get(), &modules, &LookupDso);
  if (!fuchsia)
    return ZX_ERR_INTERNAL;

  unw_addr_space_t remote_aspace = unw_create_addr_space(
      const_cast<unw_accessors_t*>(&_UFuchsia_accessors), 0);
  if (!remote_aspace)
    return ZX_ERR_INTERNAL;

  unw_cursor_t cursor;
  if (unw_init_remote(&cursor, remote_aspace, fuchsia) < 0)
    return ZX_ERR_INTERNAL;

  // Compute the register IDs for this platform's IP/SP.
  debug_ipc::Arch arch = arch::ArchProvider::Get().GetArch();
  debug_ipc::RegisterID ip_reg_id =
      GetSpecialRegisterID(arch, debug_ipc::SpecialRegisterType::kIP);
  debug_ipc::RegisterID sp_reg_id =
      GetSpecialRegisterID(arch, debug_ipc::SpecialRegisterType::kSP);

  // Top stack frame.
  debug_ipc::StackFrame frame;
  frame.ip = *arch::ArchProvider::Get().IPInRegs(
      const_cast<zx_thread_state_general_regs*>(&regs));
  frame.sp = *arch::ArchProvider::Get().SPInRegs(
      const_cast<zx_thread_state_general_regs*>(&regs));
  frame.cfa = 0;
  arch::ArchProvider::Get().SaveGeneralRegs(regs, &frame.regs);
  stack->push_back(std::move(frame));

  while (frame.sp >= 0x1000000 && stack->size() < max_depth + 1) {
    int ret = unw_step(&cursor);
    if (ret <= 0)
      break;

    unw_word_t val;
    unw_get_reg(&cursor, UNW_REG_IP, &val);
    if (val == 0)
      break;  // Null code address means we're done.
    frame.ip = val;
    frame.regs.emplace_back(ip_reg_id, val);

    unw_get_reg(&cursor, UNW_REG_SP, &val);
    frame.sp = val;
    frame.regs.emplace_back(sp_reg_id, val);

    // Previous frame's CFA is our SP.
    if (!stack->empty())
      stack->back().cfa = val;

    // Note that libunwind may theoretically be able to give us all
    // callee-saved register values for a given frame. Currently asking for any
    // register always returns success, making it impossible to tell what is
    // valid and what is not.
    //
    // If we switch unwinders (maybe to LLVM's or a custom one), this should be
    // re-evaluated. We may be able to attach a vector of Register structs on
    // each frame for the values we know about.

    // This "if" statement prevents adding more than the max number of stack
    // entries since we requested one more from libunwind to get the CFA.
    if (stack->size() < max_depth)
      stack->push_back(frame);
  }

  // The last stack entry will typically have a 0 IP address. We want to send
  // this anyway because it will hold the initial stack pointer for the thread,
  // which in turn allows computation of the first real frame's fingerprint.

  return ZX_OK;
}

}  // namespace

void SetUnwinderType(UnwinderType type) { unwinder_type = type; }

zx_status_t UnwindStack(const zx::process& process, uint64_t dl_debug_addr,
                        const zx::thread& thread,
                        const zx_thread_state_general_regs& regs,
                        size_t max_depth,
                        std::vector<debug_ipc::StackFrame>* stack) {
  switch (unwinder_type) {
    case UnwinderType::kNgUnwind:
      return UnwindStackNgUnwind(process, dl_debug_addr, thread, regs,
                                 max_depth, stack);
    case UnwinderType::kAndroid:
      return UnwindStackAndroid(process, dl_debug_addr, thread, regs, max_depth,
                                stack);
  }
  return ZX_ERR_NOT_SUPPORTED;
}

}  // namespace debug_agent
