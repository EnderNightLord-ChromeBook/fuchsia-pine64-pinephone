// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/console_context.h"

#include <inttypes.h>

#include "src/developer/debug/zxdb/client/breakpoint.h"
#include "src/developer/debug/zxdb/client/filter.h"
#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/setting_schema_definition.h"
#include "src/developer/debug/zxdb/client/symbol_server.h"
#include "src/developer/debug/zxdb/client/target.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/console/command.h"
#include "src/developer/debug/zxdb/console/command_utils.h"
#include "src/developer/debug/zxdb/console/console.h"
#include "src/developer/debug/zxdb/console/format_context.h"
#include "src/developer/debug/zxdb/console/format_target.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"
#include "src/developer/debug/zxdb/symbols/location.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

ConsoleContext::ConsoleContext(Session* session) : session_(session) {
  session->AddObserver(this);
  session->system().AddObserver(this);

  // Pick up any previously created targets. This will normally just be the
  // default one.
  for (Target* target : session->system().GetTargets())
    DidCreateTarget(target);

  for (JobContext* job_context : session->system().GetJobContexts())
    DidCreateJobContext(job_context);

  for (SymbolServer* symbol_server : session->system().GetSymbolServers())
    DidCreateSymbolServer(symbol_server);
}

ConsoleContext::~ConsoleContext() {
  // Unregister for all observers.
  session_->system().RemoveObserver(this);

  for (auto& target_pair : id_to_target_) {
    target_pair.second.target->RemoveObserver(this);

    Process* process = target_pair.second.target->GetProcess();
    if (process)
      process->RemoveObserver(this);

    for (auto& thread_pair : target_pair.second.id_to_thread)
      thread_pair.second.thread->RemoveObserver(this);
  }
}

int ConsoleContext::IdForTarget(const Target* target) const {
  const auto& found = target_to_id_.find(target);
  if (found == target_to_id_.end()) {
    FXL_NOTREACHED();
    return 0;
  }
  return found->second;
}

int ConsoleContext::IdForJobContext(const JobContext* job_context) const {
  const auto& found = job_context_to_id_.find(job_context);
  if (found == job_context_to_id_.end()) {
    FXL_NOTREACHED();
    return 0;
  }
  return found->second;
}

int ConsoleContext::IdForThread(const Thread* thread) const {
  const TargetRecord* record =
      const_cast<ConsoleContext*>(this)->GetTargetRecord(thread->GetProcess()->GetTarget());
  if (!record)
    return 0;

  auto found = record->thread_to_id.find(thread);
  if (found == record->thread_to_id.end()) {
    FXL_NOTREACHED();
    return 0;
  }
  return found->second;
}

int ConsoleContext::IdForFrame(const Frame* frame) const {
  // Find the frame in the thread's backtrace. We don't have to worry about
  // whether the frames have been synced, since if there is a frame here,
  // we know it's present in the thread's list.
  Thread* thread = frame->GetThread();
  const Stack& stack = thread->GetStack();
  for (size_t i = 0; i < stack.size(); i++) {
    if (stack[i] == frame)
      return static_cast<int>(i);
  }
  FXL_NOTREACHED();  // Should have found the frame.
  return 0;
}

int ConsoleContext::IdForSymbolServer(const SymbolServer* symbol_server) const {
  const auto& found = symbol_server_to_id_.find(symbol_server);
  if (found == symbol_server_to_id_.end()) {
    FXL_NOTREACHED();
    return 0;
  }
  return found->second;
}

int ConsoleContext::IdForBreakpoint(const Breakpoint* breakpoint) const {
  FXL_DCHECK(!breakpoint->IsInternal())
      << "Should not be trying to get the ID of internal breakpoints. The "
         "client layer should filter these out.";

  auto found = breakpoint_to_id_.find(breakpoint);
  if (found == breakpoint_to_id_.end()) {
    FXL_NOTREACHED();
    return 0;
  }
  return found->second;
}

int ConsoleContext::IdForFilter(const Filter* filter) const {
  auto found = filter_to_id_.find(filter);
  if (found == filter_to_id_.end()) {
    FXL_NOTREACHED();
    return 0;
  }
  return found->second;
}

void ConsoleContext::SetActiveJobContext(const JobContext* job_context) {
  auto found = job_context_to_id_.find(job_context);
  if (found == job_context_to_id_.end()) {
    FXL_NOTREACHED();
    return;
  }
  active_job_context_id_ = found->second;
}

int ConsoleContext::GetActiveJobContextId() const { return active_job_context_id_; }

JobContext* ConsoleContext::GetActiveJobContext() const {
  auto found = id_to_job_context_.find(active_job_context_id_);
  if (found == id_to_job_context_.end())
    return nullptr;
  return found->second.job_context;
}

void ConsoleContext::SetActiveTarget(const Target* target) {
  auto found = target_to_id_.find(target);
  if (found == target_to_id_.end()) {
    FXL_NOTREACHED();
    return;
  }
  active_target_id_ = found->second;
}

int ConsoleContext::GetActiveTargetId() const { return active_target_id_; }

Target* ConsoleContext::GetActiveTarget() const {
  auto found = id_to_target_.find(active_target_id_);
  if (found == id_to_target_.end())
    return nullptr;
  return found->second.target;
}

void ConsoleContext::SetActiveSymbolServer(const SymbolServer* symbol_server) {
  auto found = symbol_server_to_id_.find(symbol_server);
  if (found == symbol_server_to_id_.end()) {
    FXL_NOTREACHED();
    return;
  }
  active_symbol_server_id_ = found->second;
}

int ConsoleContext::GetActiveSymbolServerId() const { return active_symbol_server_id_; }

SymbolServer* ConsoleContext::GetActiveSymbolServer() const {
  auto found = id_to_symbol_server_.find(active_symbol_server_id_);
  if (found == id_to_symbol_server_.end())
    return nullptr;
  return found->second;
}

void ConsoleContext::SetActiveThreadForTarget(const Thread* thread) {
  TargetRecord* record = GetTargetRecord(thread->GetProcess()->GetTarget());
  if (!record)
    return;

  auto found = record->thread_to_id.find(thread);
  if (found == record->thread_to_id.end()) {
    FXL_NOTREACHED();
    return;
  }
  record->active_thread_id = found->second;
}

int ConsoleContext::GetActiveThreadIdForTarget(const Target* target) {
  const TargetRecord* record = GetTargetRecord(target);
  if (!record) {
    FXL_NOTREACHED();
    return 0;
  }
  return record->active_thread_id;
}

Thread* ConsoleContext::GetActiveThreadForTarget(const Target* target) {
  const TargetRecord* record = GetTargetRecord(target);
  if (!record) {
    FXL_NOTREACHED();
    return nullptr;
  }

  auto found = record->id_to_thread.find(record->active_thread_id);
  if (found == record->id_to_thread.end())
    return nullptr;
  return found->second.thread;
}

void ConsoleContext::SetActiveFrameForThread(const Frame* frame) {
  ThreadRecord* record = GetThreadRecord(frame->GetThread());
  if (!record) {
    FXL_NOTREACHED();
    return;
  }
  record->active_frame_id = IdForFrame(frame);
}

void ConsoleContext::SetActiveFrameIdForThread(const Thread* thread, int id) {
  ThreadRecord* record = GetThreadRecord(thread);
  if (!record) {
    FXL_NOTREACHED();
    return;
  }
  record->active_frame_id = id;
}

int ConsoleContext::GetActiveFrameIdForThread(const Thread* thread) const {
  const ThreadRecord* record = GetThreadRecord(thread);
  if (!record) {
    FXL_NOTREACHED();
    return 0;
  }

  // Should be a valid frame index in the thread (or no frames and == 0).
  FXL_DCHECK((thread->GetStack().empty() && record->active_frame_id == 0) ||
             (record->active_frame_id >= 0 &&
              record->active_frame_id < static_cast<int>(thread->GetStack().size())));
  return record->active_frame_id;
}

void ConsoleContext::SetActiveBreakpoint(const Breakpoint* breakpoint) {
  int id = IdForBreakpoint(breakpoint);
  if (id != 0)
    active_breakpoint_id_ = id;
}

int ConsoleContext::GetActiveBreakpointId() const { return active_breakpoint_id_; }

Breakpoint* ConsoleContext::GetActiveBreakpoint() const {
  if (active_breakpoint_id_ == 0)
    return nullptr;
  auto found = id_to_breakpoint_.find(active_breakpoint_id_);
  if (found == id_to_breakpoint_.end()) {
    FXL_NOTREACHED();
    return nullptr;
  }
  return found->second;
}

void ConsoleContext::SetActiveFilter(const Filter* filter) {
  int id = IdForFilter(filter);
  if (id != 0)
    active_filter_id_ = id;
}

int ConsoleContext::GetActiveFilterId() const { return active_filter_id_; }

Filter* ConsoleContext::GetActiveFilter() const {
  if (active_filter_id_ == 0)
    return nullptr;
  auto found = id_to_filter_.find(active_filter_id_);
  if (found == id_to_filter_.end()) {
    FXL_NOTREACHED();
    return nullptr;
  }
  return found->second;
}

SourceAffinity ConsoleContext::GetSourceAffinityForThread(const Thread* thread) const {
  const ThreadRecord* record = GetThreadRecord(thread);
  if (!record)
    return SourceAffinity::kSource;
  return record->source_affinity;
}

void ConsoleContext::SetSourceAffinityForThread(const Thread* thread,
                                                SourceAffinity source_affinity) {
  if (source_affinity == SourceAffinity::kNone)
    return;  // Don't change anything, previous command still stands.

  ThreadRecord* record = GetThreadRecord(thread);
  if (!record)
    return;
  record->source_affinity = source_affinity;
}

void ConsoleContext::OutputThreadContext(
    const Thread* thread, debug_ipc::NotifyException::Type type,
    const std::vector<fxl::WeakPtr<Breakpoint>>& hit_breakpoints) const {
  Target* target = thread->GetProcess()->GetTarget();

  Console* console = Console::get();
  OutputBuffer out;

  out.Append("🛑 ");

  // Only print out the process/thread when there's more than one.
  if (id_to_target_.size() > 1)
    out.Append(fxl::StringPrintf("Process %d ", IdForTarget(target)));
  if (thread->GetProcess()->GetThreads().size() > 1)
    out.Append(fxl::StringPrintf("Thread %d ", IdForThread(thread)));

  // Stop reason.
  if (!hit_breakpoints.empty()) {
    out.Append(DescribeHitBreakpoints(hit_breakpoints));
  } else if (type == debug_ipc::NotifyException::Type::kGeneral) {
    // Show exception type for non-debug exceptions. Most exceptions are
    // generated by the debugger internally so skip those to avoid noise.
    out.Append(
        fxl::StringPrintf("on %s exception ", debug_ipc::NotifyException::TypeToString(type)));
  }

  // Frame (current position will always be frame 0).
  const Stack& stack = thread->GetStack();
  if (stack.empty()) {
    out.Append(" (no location information)\n");
    console->Output(out);
  } else {
    const Location& location = stack[0]->GetLocation();
    out.Append(
        FormatLocation(thread->GetProcess()->GetTarget()->GetSymbols(), location, false, false));
    if (location.has_symbols()) {
      out.Append("\n");
    } else {
      out.Append(" (no symbol info)\n");
    }
    console->Output(out);
    Err err =
        OutputSourceContext(thread->GetProcess(), location, GetSourceAffinityForThread(thread));
    if (err.has_error())
      console->Output(err);
  }
}

Err ConsoleContext::FillOutCommand(Command* cmd) const {
  // JobContext.
  Err result = FillOutJobContext(cmd);
  if (result.has_error())
    return result;

  // Target.
  const TargetRecord* target_record = nullptr;
  result = FillOutTarget(cmd, &target_record);
  if (result.has_error())
    return result;

  // Thread.
  const ThreadRecord* thread_record = nullptr;
  result = FillOutThread(cmd, target_record, &thread_record);
  if (result.has_error())
    return result;

  // Frame.
  result = FillOutFrame(cmd, thread_record);
  if (result.has_error())
    return result;

  // Breakpoint.
  result = FillOutBreakpoint(cmd);
  if (result.has_error())
    return result;

  // Filter.
  result = FillOutFilter(cmd);
  if (result.has_error())
    return result;

  // SymbolServer.
  result = FillOutSymbolServer(cmd);
  if (result.has_error())
    return result;

  return Err();
}

void ConsoleContext::HandleNotification(NotificationType type, const std::string& msg) {
  OutputBuffer out;
  auto preamble = fxl::StringPrintf("[%s] ", NotificationTypeToString(type));
  switch (type) {
    case NotificationType::kError:
      out.Append(Syntax::kError, std::move(preamble));
    case NotificationType::kWarning:
      out.Append(Syntax::kWarning, std::move(preamble));
    case NotificationType::kProcessStdout:
    case NotificationType::kProcessStderr:
      break;
    case NotificationType::kNone:  // None is a no-op.
      return;
  }

  out.Append(msg);
  Console::get()->Output(std::move(out));
}

void ConsoleContext::DidCreateTarget(Target* target) {
  target->AddObserver(this);

  int new_id = next_target_id_;
  next_target_id_++;

  TargetRecord record;
  record.target_id = new_id;
  record.target = target;

  id_to_target_[new_id] = std::move(record);
  target_to_id_[target] = new_id;

  // Set the active target only if there's none already.
  if (active_target_id_ == 0)
    active_target_id_ = new_id;
}

void ConsoleContext::DidCreateJobContext(JobContext* job_context) {
  // TODO(anmittal): Add observer if required.
  int new_id = next_job_context_id_;
  next_job_context_id_++;

  JobContextRecord record;
  record.job_context_id = new_id;
  record.job_context = job_context;

  id_to_job_context_[new_id] = std::move(record);
  job_context_to_id_[job_context] = new_id;

  // Set the active job_context only if there's none already.
  if (active_job_context_id_ == 0)
    active_job_context_id_ = new_id;
}

void ConsoleContext::WillDestroyTarget(Target* target) {
  target->RemoveObserver(this);

  TargetRecord* record = GetTargetRecord(target);
  if (!record) {
    FXL_NOTREACHED();
    return;
  }

  if (active_target_id_ == record->target_id) {
    // Need to update the default target ID.
    if (id_to_target_.empty()) {
      // This should only happen in the shutting-down case.
      active_target_id_ = 0;
    } else {
      // Just pick the first target to be the active one. It might be nice to
      // have an ordering of which one the user had selected previously in
      // case they're toggling between two.
      active_target_id_ = id_to_target_.begin()->first;
    }
  }

  // There should be no threads by the time we erase the target mapping.
  FXL_DCHECK(record->id_to_thread.empty());
  FXL_DCHECK(record->thread_to_id.empty());

  target_to_id_.erase(target);
  id_to_target_.erase(record->target_id);
  // *record is now invalid.
}

void ConsoleContext::DidCreateBreakpoint(Breakpoint* breakpoint) {
  int id = next_breakpoint_id_;
  next_breakpoint_id_++;

  id_to_breakpoint_[id] = breakpoint;
  breakpoint_to_id_[breakpoint] = id;
}

void ConsoleContext::DidCreateFilter(Filter* filter) {
  int id = next_filter_id_;
  next_filter_id_++;

  id_to_filter_[id] = filter;
  filter_to_id_[filter] = id;
}

void ConsoleContext::WillDestroyBreakpoint(Breakpoint* breakpoint) {
  auto found_breakpoint = breakpoint_to_id_.find(breakpoint);
  if (found_breakpoint == breakpoint_to_id_.end()) {
    FXL_NOTREACHED();
    return;
  }
  int id = found_breakpoint->second;

  // Clear any active breakpoint if it's the deleted one.
  if (active_breakpoint_id_ == id)
    active_breakpoint_id_ = 0;

  id_to_breakpoint_.erase(id);
  breakpoint_to_id_.erase(found_breakpoint);
}

void ConsoleContext::DidCreateSymbolServer(SymbolServer* symbol_server) {
  int id = next_symbol_server_id_;
  next_symbol_server_id_++;

  id_to_symbol_server_[id] = symbol_server;
  symbol_server_to_id_[symbol_server] = id;

  if (active_symbol_server_id_ == 0) {
    active_symbol_server_id_ = id;
  }
}

void ConsoleContext::OnSymbolIndexingInformation(const std::string& msg) {
  Console* console = Console::get();
  console->Output(OutputBuffer(Syntax::kComment, msg));
}

void ConsoleContext::DidCreateProcess(Target* target, Process* process,
                                      bool autoattached_to_new_process) {
  TargetRecord* record = GetTargetRecord(target);
  if (!record) {
    FXL_NOTREACHED();
    return;
  }

  process->AddObserver(this);

  // Restart the thread ID counting when the process starts in case this
  // target was previously running (we want to restart numbering every time).
  record->next_thread_id = 1;

  OutputBuffer out;
  switch (process->start_type()) {
    case Process::StartType::kAttach:
      out.Append("Attached ");
      break;
    case Process::StartType::kComponent:
    case Process::StartType::kLaunch:
      out.Append("Launched ");
      break;
  }
  out.Append(FormatTarget(this, target));

  bool pause_on_attach =
      session()->system().settings().GetBool(ClientSettings::System::kPauseOnAttach);
  if (autoattached_to_new_process && pause_on_attach) {
    out.Append(Syntax::kComment,
               "\n  The process is currently in an initializing state. You can "
               "set pending\n  breakpoints (symbols haven't been loaded yet) "
               "and \"continue\".");
  }
  Console::get()->Output(out);
}

void ConsoleContext::WillDestroyProcess(Target* target, Process* process, DestroyReason reason,
                                        int exit_code) {
  TargetRecord* record = GetTargetRecord(target);
  if (!record) {
    FXL_NOTREACHED();
    return;
  }

  int process_index = IdForTarget(process->GetTarget());

  Console* console = Console::get();
  std::string msg;
  switch (reason) {
    case TargetObserver::DestroyReason::kExit:
      msg = fxl::StringPrintf("Process %d exited with code %d.", process_index, exit_code);
      break;
    case TargetObserver::DestroyReason::kDetach:
      msg = fxl::StringPrintf("Process %d detached.", process_index);
      break;
    case TargetObserver::DestroyReason::kKill:
      msg = fxl::StringPrintf("Process %d killed.", process_index);
      break;
  }

  console->Output(msg);
}

void ConsoleContext::DidCreateThread(Process* process, Thread* thread) {
  TargetRecord* record = GetTargetRecord(process->GetTarget());
  if (!record) {
    FXL_NOTREACHED();
    return;
  }

  thread->AddObserver(this);

  int thread_id = record->next_thread_id;
  record->next_thread_id++;

  record->id_to_thread[thread_id].thread = thread;
  record->thread_to_id[thread] = thread_id;

  // Only make a new thread the default if there is no current thread,
  // otherwise the context will be swapping out from under the user as the
  // program runs.
  if (record->active_thread_id == 0)
    record->active_thread_id = thread_id;
}

void ConsoleContext::WillDestroyThread(Process* process, Thread* thread) {
  TargetRecord* record = GetTargetRecord(process->GetTarget());
  if (!record) {
    FXL_NOTREACHED();
    return;
  }

  thread->RemoveObserver(this);

  auto found_thread_to_id = record->thread_to_id.find(thread);
  if (found_thread_to_id == record->thread_to_id.end()) {
    FXL_NOTREACHED();
    return;
  }
  int thread_id = found_thread_to_id->second;

  record->id_to_thread.erase(found_thread_to_id->second);
  record->thread_to_id.erase(found_thread_to_id);

  // Update the active thread if the currently active one is being deleted.
  if (thread_id == record->active_thread_id) {
    // Just pick the first thread to be the active one. It might be nice to
    // have an ordering of which one the user had selected previously in
    // case they're toggling between two.
    if (record->id_to_thread.empty()) {
      record->active_thread_id = 0;
    } else {
      record->active_thread_id = record->id_to_thread.begin()->first;
    }
  }
}

void ConsoleContext::OnSymbolLoadFailure(Process* process, const Err& err) {
  Console::get()->Output(err);
}

// For comparison, GDB's printout for a breakpoint hit is:
//
//   Breakpoint 1, main () at eraseme.c:4
//   4         printf("Hello\n");
//
// And LLDB's is:
//
//   * thread #1: tid = 33767, 0x000055555555463e a.out`main + 4 at
//   eraseme.c:4, name = 'a.out', stop reason = breakpoint 1.1
//       frame #0: 0x000055555555463e a.out`main + 4 at eraseme.c:4
//      1    #include <stdio.h>
//      2
//      3    int main() {
//   -> 4    printf("Hello\n");
//      5    return 1;
//      6  }
//
// When stepping, GDB prints out only the 2nd line with source info, and LLDB
// prints out the whole thing with "step over" for "stop reason".
void ConsoleContext::OnThreadStopped(Thread* thread, debug_ipc::NotifyException::Type type,
                                     const std::vector<fxl::WeakPtr<Breakpoint>>& hit_breakpoints) {
  // The stopped, process, thread, and frame should be active.
  Target* target = thread->GetProcess()->GetTarget();
  SetActiveTarget(target);
  SetActiveThreadForTarget(thread);
  SetActiveFrameIdForThread(thread, 0);

  // Show the location information.
  OutputThreadContext(thread, type, hit_breakpoints);
}

void ConsoleContext::OnThreadFramesInvalidated(Thread* thread) {
  ThreadRecord* record = GetThreadRecord(thread);
  if (!record) {
    FXL_NOTREACHED();
    return;
  }

  // Reset the active frame.
  record->active_frame_id = 0;
}

ConsoleContext::TargetRecord* ConsoleContext::GetTargetRecord(int target_id) {
  return const_cast<TargetRecord*>(
      const_cast<const ConsoleContext*>(this)->GetTargetRecord(target_id));
}

const ConsoleContext::TargetRecord* ConsoleContext::GetTargetRecord(int target_id) const {
  auto found_to_record = id_to_target_.find(target_id);
  if (found_to_record == id_to_target_.end()) {
    FXL_NOTREACHED();
    return nullptr;
  }
  return &found_to_record->second;
}

ConsoleContext::TargetRecord* ConsoleContext::GetTargetRecord(const Target* target) {
  return const_cast<TargetRecord*>(
      const_cast<const ConsoleContext*>(this)->GetTargetRecord(target));
}

const ConsoleContext::TargetRecord* ConsoleContext::GetTargetRecord(const Target* target) const {
  auto found_to_id = target_to_id_.find(target);
  if (found_to_id == target_to_id_.end()) {
    FXL_NOTREACHED();
    return nullptr;
  }
  return GetTargetRecord(found_to_id->second);
}

ConsoleContext::ThreadRecord* ConsoleContext::GetThreadRecord(const Thread* thread) {
  // Share implementation with the non-const version.
  return const_cast<ThreadRecord*>(
      const_cast<const ConsoleContext*>(this)->GetThreadRecord(thread));
}

const ConsoleContext::ThreadRecord* ConsoleContext::GetThreadRecord(const Thread* thread) const {
  const TargetRecord* target_record = GetTargetRecord(thread->GetProcess()->GetTarget());
  if (!target_record) {
    FXL_NOTREACHED();
    return nullptr;
  }

  auto found_thread_to_id = target_record->thread_to_id.find(thread);
  if (found_thread_to_id == target_record->thread_to_id.end()) {
    FXL_NOTREACHED();
    return nullptr;
  }
  int thread_id = found_thread_to_id->second;

  auto found_id_to_thread = target_record->id_to_thread.find(thread_id);
  if (found_thread_to_id == target_record->thread_to_id.end()) {
    FXL_NOTREACHED();
    return nullptr;
  }
  return &found_id_to_thread->second;
}

Err ConsoleContext::FillOutJobContext(Command* cmd) const {
  int job_context_id = cmd->GetNounIndex(Noun::kJob);
  if (job_context_id == Command::kNoIndex) {
    // No index: use the active one (may or may not exist).
    job_context_id = active_job_context_id_;
    auto found_job_context = id_to_job_context_.find(job_context_id);
    if (found_job_context == id_to_job_context_.end()) {
      // When there are no job contexts, the active ID should be 0.
      FXL_DCHECK(job_context_id == 0);
    } else {
      cmd->set_job_context(found_job_context->second.job_context);
    }
    return Err();
  }

  // Explicit index given, look it up.
  auto found_job_context = id_to_job_context_.find(job_context_id);
  if (found_job_context == id_to_job_context_.end()) {
    return Err(ErrType::kInput, fxl::StringPrintf("There is no job %d.", job_context_id));
  }
  cmd->set_job_context(found_job_context->second.job_context);
  return Err();
}

Err ConsoleContext::FillOutTarget(Command* cmd, TargetRecord const** out_target_record) const {
  int target_id = cmd->GetNounIndex(Noun::kProcess);
  if (target_id == Command::kNoIndex) {
    // No index: use the active one (which should always exist).
    target_id = active_target_id_;
    auto found_target = id_to_target_.find(target_id);
    FXL_DCHECK(found_target != id_to_target_.end());
    cmd->set_target(found_target->second.target);

    FXL_DCHECK(cmd->target());  // Default target should always exist.
    *out_target_record = GetTargetRecord(target_id);
    return Err();
  }

  // Explicit index given, look it up.
  auto found_target = id_to_target_.find(target_id);
  if (found_target == id_to_target_.end()) {
    return Err(ErrType::kInput, fxl::StringPrintf("There is no process %d.", target_id));
  }
  cmd->set_target(found_target->second.target);
  *out_target_record = GetTargetRecord(target_id);
  return Err();
}

Err ConsoleContext::FillOutThread(Command* cmd, const TargetRecord* target_record,
                                  ThreadRecord const** out_thread_record) const {
  int thread_id = cmd->GetNounIndex(Noun::kThread);
  const ThreadRecord* thread_record = nullptr;
  if (thread_id == Command::kNoIndex) {
    // No thread specified, use the default one.
    thread_id = target_record->active_thread_id;
    auto found_thread = target_record->id_to_thread.find(thread_id);
    if (found_thread == target_record->id_to_thread.end()) {
      // When there are no threads, the active thread ID will be 0 and that's
      // fine. But if it's nonzero, the thread should always be valid.
      FXL_DCHECK(thread_id == 0);
    } else {
      thread_record = &found_thread->second;
      cmd->set_thread(thread_record->thread);
    }
    *out_thread_record = thread_record;
    return Err();
  }

  // Explicit index given, look it up.
  auto found_thread = target_record->id_to_thread.find(thread_id);
  if (found_thread == target_record->id_to_thread.end()) {
    if (target_record->id_to_thread.empty()) {
      return Err(ErrType::kInput, "There are no threads in the process.");
    }
    return Err(ErrType::kInput,
               fxl::StringPrintf("There is no thread %d in the process.", thread_id));
  }

  thread_record = &found_thread->second;
  cmd->set_thread(thread_record->thread);
  *out_thread_record = thread_record;
  return Err();
}

Err ConsoleContext::FillOutFrame(Command* cmd, const ThreadRecord* thread_record) const {
  int frame_id = cmd->GetNounIndex(Noun::kFrame);
  if (frame_id == Command::kNoIndex) {
    // No index: use the active one (if any).
    if (thread_record) {
      auto& stack = thread_record->thread->GetStack();
      frame_id = thread_record->active_frame_id;
      if (frame_id >= 0 && frame_id < static_cast<int>(stack.size())) {
        cmd->set_frame(stack[frame_id]);
      } else if (!stack.empty()) {
        // Invalid frame index, default to 0th frame.
        frame_id = 0;
        cmd->set_frame(stack[0]);
      }
    }
    return Err();
  }

  // Frame index specified, use it.
  if (!thread_record)
    return Err(ErrType::kInput, "There is no thread to have frames.");

  Stack& stack = thread_record->thread->GetStack();
  if (frame_id >= 0 && frame_id < static_cast<int>(stack.size())) {
    // References a valid frame. Now check that the frame index references
    // the top physical frame (or one of its inline expansions above it) or
    // all frames are synced.
    bool top_physical_frame = true;
    for (int i = 0; i < frame_id; i++) {
      if (!stack[i]->IsInline()) {
        top_physical_frame = false;
        break;
      }
    }
    if (top_physical_frame || stack.has_all_frames()) {
      cmd->set_frame(stack[frame_id]);
      return Err();
    }
  }

  // Invalid frame specified. The full backtrace list is populated on
  // demand. It could be if the frames aren't synced for the thread we
  // could delay processing this command and get the frames, but we're not
  // set up to do that (this function is currently synchronous). Instead
  // if we detect the list isn't populated and the user requested one
  // that's out-of-range, request they manually sync the list.
  //
  // Check for the presence of any frames because the thread might not be
  // in a state to have frames (i.e. it's running).
  if (!stack.empty() && !thread_record->thread->GetStack().has_all_frames()) {
    return Err(ErrType::kInput,
               "The frames for this thread haven't been synced.\n"
               "Use \"frame\" to list the frames before selecting one to "
               "populate the frame list.");
  }
  return Err(ErrType::kInput,
             "Invalid frame index.\n"
             "Use \"frame\" to list available ones.");
}

Err ConsoleContext::FillOutBreakpoint(Command* cmd) const {
  int breakpoint_id = cmd->GetNounIndex(Noun::kBreakpoint);
  if (breakpoint_id == Command::kNoIndex) {
    // No index: use the active one (which may not exist).
    cmd->set_breakpoint(GetActiveBreakpoint());
    return Err();
  }

  // Explicit index given, look it up.
  auto found_breakpoint = id_to_breakpoint_.find(breakpoint_id);
  if (found_breakpoint == id_to_breakpoint_.end()) {
    return Err(ErrType::kInput, fxl::StringPrintf("There is no breakpoint %d.", breakpoint_id));
  }
  cmd->set_breakpoint(found_breakpoint->second);
  return Err();
}

Err ConsoleContext::FillOutFilter(Command* cmd) const {
  int filter_id = cmd->GetNounIndex(Noun::kFilter);
  if (filter_id == Command::kNoIndex) {
    // No index: use the active one (which may not exist).
    cmd->set_filter(GetActiveFilter());
    return Err();
  }

  // Explicit index given, look it up.
  auto found_filter = id_to_filter_.find(filter_id);
  if (found_filter == id_to_filter_.end()) {
    return Err(ErrType::kInput, fxl::StringPrintf("There is no filter %d.", filter_id));
  }
  cmd->set_filter(found_filter->second);
  return Err();
}

Err ConsoleContext::FillOutSymbolServer(Command* cmd) const {
  int symbol_server_id = cmd->GetNounIndex(Noun::kSymServer);
  if (symbol_server_id == Command::kNoIndex) {
    // No index: use the active one (which may not exist).
    cmd->set_sym_server(GetActiveSymbolServer());
    return Err();
  }

  // Explicit index given, look it up.
  auto found_symbol_server = id_to_symbol_server_.find(symbol_server_id);
  if (found_symbol_server == id_to_symbol_server_.end()) {
    return Err(ErrType::kInput,
               fxl::StringPrintf("There is no symbol server %d.", symbol_server_id));
  }
  cmd->set_sym_server(found_symbol_server->second);
  return Err();
}

std::string ConsoleContext::DescribeHitBreakpoints(
    const std::vector<fxl::WeakPtr<Breakpoint>>& hits) const {
  // Do two passes since some of the weak pointers may be gone.
  std::vector<int> ids;
  for (const auto& hit : hits) {
    if (hit)
      ids.push_back(IdForBreakpoint(hit.get()));
  }

  if (ids.empty())
    return std::string();

  std::string result("on bp");
  for (size_t i = 0; i < ids.size(); i++) {
    result += fxl::StringPrintf(" %d", ids[i]);
    if (i < ids.size() - 1)
      result.push_back(',');
  }
  result.push_back(' ');
  return result;
}

}  // namespace zxdb
