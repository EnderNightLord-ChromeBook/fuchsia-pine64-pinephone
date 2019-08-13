// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/remote_api.h"

#include "src/developer/debug/zxdb/client/session.h"
#include "src/lib/fxl/logging.h"

namespace zxdb {

void RemoteAPI::Hello(const debug_ipc::HelloRequest& request,
                      fit::callback<void(const Err&, debug_ipc::HelloReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Launch(const debug_ipc::LaunchRequest& request,
                       fit::callback<void(const Err&, debug_ipc::LaunchReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Kill(const debug_ipc::KillRequest& request,
                     fit::callback<void(const Err&, debug_ipc::KillReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Attach(const debug_ipc::AttachRequest& request,
                       fit::callback<void(const Err&, debug_ipc::AttachReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::ConfigAgent(const debug_ipc::ConfigAgentRequest& request,
                            fit::callback<void(const Err&, debug_ipc::ConfigAgentReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Detach(const debug_ipc::DetachRequest& request,
                       fit::callback<void(const Err&, debug_ipc::DetachReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Modules(const debug_ipc::ModulesRequest& request,
                        fit::callback<void(const Err&, debug_ipc::ModulesReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Pause(const debug_ipc::PauseRequest& request,
                      fit::callback<void(const Err&, debug_ipc::PauseReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::QuitAgent(const debug_ipc::QuitAgentRequest& request,
                          fit::callback<void(const Err&, debug_ipc::QuitAgentReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Resume(const debug_ipc::ResumeRequest& request,
                       fit::callback<void(const Err&, debug_ipc::ResumeReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::ProcessTree(const debug_ipc::ProcessTreeRequest& request,
                            fit::callback<void(const Err&, debug_ipc::ProcessTreeReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::Threads(const debug_ipc::ThreadsRequest& request,
                        fit::callback<void(const Err&, debug_ipc::ThreadsReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::ReadMemory(const debug_ipc::ReadMemoryRequest& request,
                           fit::callback<void(const Err&, debug_ipc::ReadMemoryReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::ReadRegisters(const debug_ipc::ReadRegistersRequest& request,
                              fit::callback<void(const Err&, debug_ipc::ReadRegistersReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::WriteRegisters(const debug_ipc::WriteRegistersRequest& request,
                               fit::callback<void(const Err&, debug_ipc::WriteRegistersReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::AddOrChangeBreakpoint(
    const debug_ipc::AddOrChangeBreakpointRequest& request,
    fit::callback<void(const Err&, debug_ipc::AddOrChangeBreakpointReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::RemoveBreakpoint(
    const debug_ipc::RemoveBreakpointRequest& request,
    fit::callback<void(const Err&, debug_ipc::RemoveBreakpointReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::SysInfo(const debug_ipc::SysInfoRequest& request,
                        fit::callback<void(const Err&, debug_ipc::SysInfoReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::ThreadStatus(const debug_ipc::ThreadStatusRequest& request,
                             fit::callback<void(const Err&, debug_ipc::ThreadStatusReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::AddressSpace(const debug_ipc::AddressSpaceRequest& request,
                             fit::callback<void(const Err&, debug_ipc::AddressSpaceReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::JobFilter(const debug_ipc::JobFilterRequest& request,
                          fit::callback<void(const Err&, debug_ipc::JobFilterReply)> cb) {
  FXL_NOTREACHED();
}

void RemoteAPI::WriteMemory(const debug_ipc::WriteMemoryRequest& request,
                            fit::callback<void(const Err&, debug_ipc::WriteMemoryReply)> cb) {
  FXL_NOTREACHED();
}

}  // namespace zxdb
