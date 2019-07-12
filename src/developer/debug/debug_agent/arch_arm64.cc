// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/arch_arm64.h"

#include <zircon/status.h>
#include <zircon/syscalls/exception.h>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/arch_arm64_helpers.h"
#include "src/developer/debug/debug_agent/debugged_thread.h"
#include "src/developer/debug/ipc/register_desc.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace debug_agent {
namespace arch {

namespace {

using debug_ipc::RegisterID;

debug_ipc::Register CreateRegister(RegisterID id, uint32_t length,
                                   const void* val_ptr) {
  debug_ipc::Register reg;
  reg.id = id;
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(val_ptr);
  reg.data.assign(ptr, ptr + length);
  return reg;
}

zx_status_t ReadGeneralRegs(const zx::thread& thread,
                            std::vector<debug_ipc::Register>* out) {
  zx_thread_state_general_regs gen_regs;
  zx_status_t status = thread.read_state(ZX_THREAD_STATE_GENERAL_REGS,
                                         &gen_regs, sizeof(gen_regs));
  if (status != ZX_OK)
    return status;

  ArchProvider::SaveGeneralRegs(gen_regs, out);
  return ZX_OK;
}

zx_status_t ReadVectorRegs(const zx::thread& thread,
                           std::vector<debug_ipc::Register>* out) {
  zx_thread_state_vector_regs vec_regs;
  zx_status_t status = thread.read_state(ZX_THREAD_STATE_VECTOR_REGS, &vec_regs,
                                         sizeof(vec_regs));
  if (status != ZX_OK)
    return status;

  out->push_back(CreateRegister(RegisterID::kARMv8_fpcr, 4u, &vec_regs.fpcr));
  out->push_back(CreateRegister(RegisterID::kARMv8_fpsr, 4u, &vec_regs.fpsr));

  auto base = static_cast<uint32_t>(RegisterID::kARMv8_v0);
  for (size_t i = 0; i < 32; i++) {
    auto reg_id = static_cast<RegisterID>(base + i);
    out->push_back(CreateRegister(reg_id, 16u, &vec_regs.v[i]));
  }

  return ZX_OK;
}

zx_status_t ReadDebugRegs(const zx::thread& thread,
                          std::vector<debug_ipc::Register>* out) {
  zx_thread_state_debug_regs_t debug_regs;
  zx_status_t status = thread.read_state(ZX_THREAD_STATE_DEBUG_REGS,
                                         &debug_regs, sizeof(debug_regs));
  if (status != ZX_OK)
    return status;

  if (debug_regs.hw_bps_count >= AARCH64_MAX_HW_BREAKPOINTS) {
    FXL_LOG(ERROR) << "Received too many HW breakpoints: "
                   << debug_regs.hw_bps_count
                   << " (max: " << AARCH64_MAX_HW_BREAKPOINTS << ").";
    return ZX_ERR_INVALID_ARGS;
  }

  auto bcr_base = static_cast<uint32_t>(RegisterID::kARMv8_dbgbcr0_el1);
  auto bvr_base = static_cast<uint32_t>(RegisterID::kARMv8_dbgbvr0_el1);
  for (size_t i = 0; i < debug_regs.hw_bps_count; i++) {
    auto bcr_id = static_cast<RegisterID>(bcr_base + i);
    out->push_back(CreateRegister(bcr_id, sizeof(debug_regs.hw_bps[i].dbgbcr),
                                  &debug_regs.hw_bps[i].dbgbcr));

    auto bvr_id = static_cast<RegisterID>(bvr_base + i);
    out->push_back(CreateRegister(bvr_id, sizeof(debug_regs.hw_bps[i].dbgbvr),
                                  &debug_regs.hw_bps[i].dbgbvr));
  }

  // TODO(donosoc): Currently this registers that are platform information are
  //                being hacked out as HW breakpoint values in order to know
  //                what the actual settings are.
  //                This should be changed to get the actual values instead, but
  //                check in for now in order to continue.
  out->push_back(CreateRegister(
      RegisterID::kARMv8_id_aa64dfr0_el1, 8u,
      &debug_regs.hw_bps[AARCH64_MAX_HW_BREAKPOINTS - 1].dbgbvr));
  out->push_back(CreateRegister(
      RegisterID::kARMv8_mdscr_el1, 8u,
      &debug_regs.hw_bps[AARCH64_MAX_HW_BREAKPOINTS - 2].dbgbvr));

  return ZX_OK;
}

debug_ipc::NotifyException::Type DecodeHWException(
    const DebuggedThread& thread) {
  zx_thread_state_debug_regs_t debug_regs;
  zx_status_t status =
      thread.thread().read_state(ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                                 sizeof(zx_thread_state_debug_regs_t));
  if (status != ZX_OK)
    return debug_ipc::NotifyException::Type::kNone;

  auto decoded_type = DecodeESR(debug_regs.esr);

  DEBUG_LOG(ArchArm64) << "Decoded ESR 0x" << std::hex << debug_regs.esr
                       << " (EC: 0x" << Arm64ExtractECFromESR(debug_regs.esr)
                       << ") as "
                       << debug_ipc::NotifyException::TypeToString(
                              decoded_type);

  if (decoded_type == debug_ipc::NotifyException::Type::kSingleStep ||
      decoded_type == debug_ipc::NotifyException::Type::kHardware) {
    return decoded_type;
  }

  FXL_NOTREACHED() << "Received invalid ESR value: 0x" << std::hex
                   << debug_regs.esr;
  return debug_ipc::NotifyException::Type::kNone;
}

}  // namespace

// "BRK 0" instruction.
// - Low 5 bits = 0.
// - High 11 bits = 11010100001
// - In between 16 bits is the argument to the BRK instruction (in this case
//   zero).
const BreakInstructionType kBreakInstruction = 0xd4200000;

uint64_t ArchProvider::BreakpointInstructionForSoftwareExceptionAddress(
    uint64_t exception_addr) {
  // ARM reports the exception for the exception instruction itself.
  return exception_addr;
}

uint64_t ArchProvider::NextInstructionForSoftwareExceptionAddress(
    uint64_t exception_addr) {
  // For software exceptions, the exception address is the one that caused it,
  // so next one is just 4 bytes following.
  //
  // TODO(brettw) handle THUMB. When a software breakpoint is hit, ESR_EL1
  // will contain the "instruction length" field which for T32 instructions
  // will be 0 (indicating 16-bits). This exception state somehow needs to be
  // plumbed down to our exception handler.
  return exception_addr + 4;
}

uint64_t ArchProvider::NextInstructionForWatchpointHit(uint64_t) {
  FXL_NOTREACHED() << "Not implemented.";
  return 0;
}

uint64_t ArchProvider::InstructionForWatchpointHit(const DebuggedThread&) {
  FXL_NOTREACHED() << "Not implemented.";
  return 0;
}

bool ArchProvider::IsBreakpointInstruction(zx::process& process,
                                           uint64_t address) {
  BreakInstructionType data;
  size_t actual_read = 0;
  if (process.read_memory(address, &data, sizeof(BreakInstructionType),
                          &actual_read) != ZX_OK ||
      actual_read != sizeof(BreakInstructionType))
    return false;

  // The BRK instruction could have any number associated with it, even though
  // we only write "BRK 0", so check for the low 5 and high 11 bytes as
  // described above.
  constexpr BreakInstructionType kMask = 0b11111111111000000000000000011111;
  return (data & kMask) == kBreakInstruction;
}

void ArchProvider::SaveGeneralRegs(const zx_thread_state_general_regs& input,
                                   std::vector<debug_ipc::Register>* out) {
  // Add the X0-X29 registers.
  uint32_t base = static_cast<uint32_t>(RegisterID::kARMv8_x0);
  for (int i = 0; i < 30; i++) {
    RegisterID type = static_cast<RegisterID>(base + i);
    out->push_back(CreateRegister(type, 8u, &input.r[i]));
  }

  // Add the named ones.
  out->push_back(CreateRegister(RegisterID::kARMv8_lr, 8u, &input.lr));
  out->push_back(CreateRegister(RegisterID::kARMv8_sp, 8u, &input.sp));
  out->push_back(CreateRegister(RegisterID::kARMv8_pc, 8u, &input.pc));
  out->push_back(CreateRegister(RegisterID::kARMv8_cpsr, 8u, &input.cpsr));
}

uint64_t* ArchProvider::IPInRegs(zx_thread_state_general_regs* regs) {
  return &regs->pc;
}
uint64_t* ArchProvider::SPInRegs(zx_thread_state_general_regs* regs) {
  return &regs->sp;
}
uint64_t* ArchProvider::BPInRegs(zx_thread_state_general_regs* regs) {
  return &regs->r[29];
}

::debug_ipc::Arch ArchProvider::GetArch() { return ::debug_ipc::Arch::kArm64; }

zx_status_t ArchProvider::ReadRegisters(
    const debug_ipc::RegisterCategory::Type& cat, const zx::thread& thread,
    std::vector<debug_ipc::Register>* out) {
  switch (cat) {
    case debug_ipc::RegisterCategory::Type::kGeneral:
      return ReadGeneralRegs(thread, out);
    case debug_ipc::RegisterCategory::Type::kFP:
      // No FP registers
      return true;
    case debug_ipc::RegisterCategory::Type::kVector:
      return ReadVectorRegs(thread, out);
    case debug_ipc::RegisterCategory::Type::kDebug:
      return ReadDebugRegs(thread, out);
    default:
      FXL_LOG(ERROR) << "Invalid category: " << static_cast<uint32_t>(cat);
      return ZX_ERR_INVALID_ARGS;
  }
}

zx_status_t ArchProvider::WriteRegisters(const debug_ipc::RegisterCategory&,
                                         zx::thread*) {
  // TODO(donosoc): Implement.
  return ZX_ERR_NOT_SUPPORTED;
}

debug_ipc::NotifyException::Type HardwareNotificationType(const zx::thread&) {
  // TODO: For now zxdb only supports single step.
  return debug_ipc::NotifyException::Type::kSingleStep;
}

debug_ipc::NotifyException::Type ArchProvider::DecodeExceptionType(
    const DebuggedThread& thread, uint32_t exception_type) {
  switch (exception_type) {
    case ZX_EXCP_SW_BREAKPOINT:
      return debug_ipc::NotifyException::Type::kSoftware;
    case ZX_EXCP_HW_BREAKPOINT:
      return DecodeHWException(thread);
    default:
      return debug_ipc::NotifyException::Type::kGeneral;
  }

  FXL_NOTREACHED();
  return debug_ipc::NotifyException::Type::kLast;
}

// HW Breakpoints --------------------------------------------------------------

uint64_t ArchProvider::BreakpointInstructionForHardwareExceptionAddress(
    uint64_t exception_addr) {
  // arm64 will return the address of the instruction *about* to be executed.
  return exception_addr;
}

debug_ipc::NotifyException::Type HardwareNotificationType(
    const DebuggedThread& thread) {
  // TODO(donosoc): Implement hw exception detection logic.
  return debug_ipc::NotifyException::Type::kSingleStep;
}

zx_status_t ArchProvider::InstallHWBreakpoint(zx::thread* thread,
                                              uint64_t address) {
  FXL_DCHECK(thread);
  // NOTE: Thread needs for the thread to be stopped. Will fail otherwise.
  zx_status_t status;
  zx_thread_state_debug_regs_t debug_regs;
  status = thread->read_state(ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                              sizeof(zx_thread_state_debug_regs_t));
  if (status != ZX_OK)
    return status;

  DEBUG_LOG(ArchArm64) << "Before installing HW breakpoint: " << std::endl
                       << DebugRegistersToString(debug_regs);

  status = SetupHWBreakpoint(address, &debug_regs);
  if (status != ZX_OK)
    return status;

  DEBUG_LOG(ArchArm64) << "After installing HW breakpoint: " << std::endl
                       << DebugRegistersToString(debug_regs);

  return thread->write_state(ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                             sizeof(zx_thread_state_debug_regs_t));
}

zx_status_t ArchProvider::UninstallHWBreakpoint(zx::thread* thread,
                                                uint64_t address) {
  FXL_DCHECK(thread);
  // NOTE: Thread needs for the thread to be stopped. Will fail otherwise.
  zx_status_t status;
  zx_thread_state_debug_regs_t debug_regs;
  status = thread->read_state(ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                              sizeof(zx_thread_state_debug_regs_t));
  if (status != ZX_OK)
    return status;

  DEBUG_LOG(ArchArm64) << "Before uninstalling HW breakpoint: " << std::endl
                       << DebugRegistersToString(debug_regs);

  status = RemoveHWBreakpoint(address, &debug_regs);
  if (status != ZX_OK)
    return status;

  DEBUG_LOG(ArchArm64) << "After uninstalling HW breakpoint: " << std::endl
                       << DebugRegistersToString(debug_regs);

  return thread->write_state(ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                             sizeof(zx_thread_state_debug_regs_t));
}

zx_status_t ArchProvider::InstallWatchpoint(zx::thread*,
                                            const debug_ipc::AddressRange&) {
  FXL_NOTIMPLEMENTED();
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t ArchProvider::UninstallWatchpoint(zx::thread*,
                                              const debug_ipc::AddressRange&) {
  FXL_NOTIMPLEMENTED();
  return ZX_ERR_NOT_SUPPORTED;
}

}  // namespace arch
}  // namespace debug_agent
