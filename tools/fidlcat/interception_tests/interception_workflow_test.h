// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDLCAT_INTERCEPTION_TESTS_INTERCEPTION_WORKFLOW_TEST_H_
#define TOOLS_FIDLCAT_INTERCEPTION_TESTS_INTERCEPTION_WORKFLOW_TEST_H_

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "tools/fidlcat/lib/interception_workflow.h"
#undef __TA_REQUIRES
#include <zircon/fidl.h>

#include "src/developer/debug/ipc/protocol.h"
#include "src/developer/debug/zxdb/client/mock_remote_api.h"
#include "src/developer/debug/zxdb/client/remote_api_test.h"
#include "src/developer/debug/zxdb/client/target_impl.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/mock_module_symbols.h"

namespace fidlcat {

class ProcessController;
class SyscallDecoderDispatcherTest;

constexpr uint64_t kFirstPid = 3141;
constexpr uint64_t kSecondPid = 2718;

constexpr uint64_t kFirstThreadKoid = 8764;
constexpr uint64_t kSecondThreadKoid = 8765;

constexpr uint32_t kHandle = 0xcefa1db0;

class SystemCallTest {
 public:
  SystemCallTest(const char* name, int64_t result, std::string_view result_name)
      : name_(name), result_(result), result_name_(result_name) {}

  const std::string& name() const { return name_; }
  int64_t result() const { return result_; }
  const std::string& result_name() const { return result_name_; }
  const std::vector<uint64_t>& inputs() const { return inputs_; }

  void AddInput(uint64_t input) { inputs_.push_back(input); }

 private:
  const std::string name_;
  const int64_t result_;
  const std::string result_name_;
  std::vector<uint64_t> inputs_;
};

// Data for syscall tests.
class DataForSyscallTest {
 public:
  DataForSyscallTest(debug_ipc::Arch arch);

  const SystemCallTest* syscall() const { return syscall_.get(); }

  void set_syscall(std::unique_ptr<SystemCallTest> syscall) { syscall_ = std::move(syscall); }

  bool use_alternate_data() const { return use_alternate_data_; }

  void set_use_alternate_data() { use_alternate_data_ = true; }

  void load_syscall_data() {
    size_t argument_count = syscall_->inputs().size();
    if (argument_count > param_regs_count_) {
      argument_count -= param_regs_count_;
      for (auto input = syscall_->inputs().crbegin();
           (input != syscall_->inputs().crend()) && (argument_count > 0);
           ++input, --argument_count) {
        *(--sp_) = *input;
      }
    }
    if (arch_ == debug_ipc::Arch::kX64) {
      *(--sp_) = kReturnAddress;
    }
    stepped_processes_.clear();
  }

  uint64_t* sp() const { return sp_; }

  void set_check_bytes() { check_bytes_ = true; }
  void set_check_handles() { check_handles_ = true; }

  uint8_t* bytes() { return reinterpret_cast<uint8_t*>(&header_); }

  size_t num_bytes() const { return sizeof(header_); }

  zx_handle_t* handles() { return handles_; }

  size_t num_handles() const { return sizeof(handles_) / sizeof(handles_[0]); }

  zx_handle_info_t* handle_infos() { return handle_infos_; }

  size_t num_handle_infos() const { return sizeof(handle_infos_) / sizeof(handle_infos_[0]); }

  uint8_t* bytes2() { return reinterpret_cast<uint8_t*>(&header2_); }

  size_t num_bytes2() const { return sizeof(header2_); }

  zx_handle_t* handles2() { return handles2_; }

  size_t num_handles2() const { return sizeof(handles2_) / sizeof(handles2_[0]); }

  void PopulateModules(std::vector<debug_ipc::Module>& modules) {
    const uint64_t kModuleBase = 0x1000000;
    debug_ipc::Module load;
    load.name = "test";
    load.base = kModuleBase;
    load.build_id = kElfSymbolBuildID;
    modules.push_back(load);
  }

  void PopulateMemoryBlockForAddress(uint64_t address, uint64_t size,
                                     debug_ipc::MemoryBlock& block) {
    block.address = address;
    block.size = size;
    block.valid = true;
    std::copy(reinterpret_cast<uint8_t*>(address), reinterpret_cast<uint8_t*>(address + size),
              std::back_inserter(block.data));
    FXL_DCHECK(size == block.data.size())
        << "expected size: " << size << " and actual size: " << block.data.size();
  }

  void PopulateRegister(debug_ipc::RegisterID register_id, uint64_t value,
                        std::vector<debug_ipc::Register>* registers) {
    debug_ipc::Register& reg = registers->emplace_back();
    reg.id = register_id;
    for (int i = 0; i < 64; i += 8) {
      reg.data.push_back((value >> i) & 0xff);
    }
  }

  void PopulateRegisters(uint64_t process_koid, std::vector<debug_ipc::Register>* registers) {
    if (stepped_processes_.find(process_koid) == stepped_processes_.end()) {
      size_t count = std::min(param_regs_count_, syscall_->inputs().size());
      for (size_t i = 0; i < count; ++i) {
        PopulateRegister(param_regs_[i], syscall_->inputs()[i], registers);
      }
    } else {
      if (arch_ == debug_ipc::Arch::kArm64) {
        PopulateRegister(debug_ipc::RegisterID::kARMv8_x0, syscall_->result(), registers);
      } else {
        PopulateRegister(debug_ipc::RegisterID::kX64_rax, syscall_->result(), registers);
      }
    }

    if (arch_ == debug_ipc::Arch::kArm64) {
      // stack pointer
      PopulateRegister(debug_ipc::RegisterID::kARMv8_sp, reinterpret_cast<uint64_t>(sp_),
                       registers);
      // link register
      PopulateRegister(debug_ipc::RegisterID::kARMv8_lr, kReturnAddress, registers);
    } else if (arch_ == debug_ipc::Arch::kX64) {
      // stack pointer
      PopulateRegister(debug_ipc::RegisterID::kX64_rsp, reinterpret_cast<uint64_t>(sp_), registers);
    }
  }

  void PopulateRegisters(uint64_t process_koid, debug_ipc::RegisterCategory& category) {
    category.type = debug_ipc::RegisterCategory::Type::kGeneral;
    PopulateRegisters(process_koid, &category.registers);
  }

  void Step(uint64_t process_koid) {
    // Increment the stack pointer to make it look as if we've stepped out of
    // the zx_channel function.
    sp_ = stack_ + kMaxStackSizeInWords;
    stepped_processes_.insert(process_koid);
  }

  template <typename T>
  void AppendElements(std::string& result, size_t num, const T* a, const T* b) {
    std::ostringstream os;
    os << "actual      expected\n";
    for (size_t i = 0; i < num; i++) {
      os << std::left << std::setw(11) << static_cast<uint32_t>(a[i]);
      os << " ";
      os << std::left << std::setw(11) << static_cast<uint32_t>(b[i]);
      os << std::endl;
    }
    result.append(os.str());
  }

  static constexpr uint64_t kReturnAddress = 0x123456798;
  static constexpr uint64_t kMaxStackSizeInWords = 0x100;
  static constexpr zx_txid_t kTxId = 0xaaaaaaaa;
  static constexpr zx_txid_t kTxId2 = 0x88888888;
  static constexpr uint32_t kReserved = 0x0;
  static constexpr uint64_t kOrdinal = 0x77e4cceb00000000lu;
  static constexpr uint64_t kOrdinal2 = 1234567890123456789lu;
  static constexpr char kElfSymbolBuildID[] = "123412341234";

 private:
  debug_ipc::RegisterID* param_regs_;
  size_t param_regs_count_;
  std::unique_ptr<SystemCallTest> syscall_;
  bool use_alternate_data_ = false;
  uint64_t stack_[kMaxStackSizeInWords];
  uint64_t* sp_;
  bool check_bytes_ = false;
  bool check_handles_ = false;
  fidl_message_header_t header_;
  fidl_message_header_t header2_;
  zx_handle_t handles_[2] = {0x01234567, 0x89abcdef};
  zx_handle_info_t handle_infos_[2] = {
      {0x01234567, ZX_OBJ_TYPE_CHANNEL,
       ZX_RIGHT_TRANSFER | ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_SIGNAL | ZX_RIGHT_SIGNAL_PEER |
           ZX_RIGHT_WAIT | ZX_RIGHT_INSPECT,
       0},
      {0x89abcdef, ZX_OBJ_TYPE_LOG,
       ZX_RIGHT_DUPLICATE | ZX_RIGHT_TRANSFER | ZX_RIGHT_WRITE | ZX_RIGHT_SIGNAL | ZX_RIGHT_WAIT |
           ZX_RIGHT_INSPECT,
       0}};
  zx_handle_t handles2_[2] = {0x76543210, 0xfedcba98};
  debug_ipc::Arch arch_;
  std::set<uint64_t> stepped_processes_;
};

// Provides the infrastructure needed to provide the data above.
class InterceptionRemoteAPI : public zxdb::MockRemoteAPI {
 public:
  explicit InterceptionRemoteAPI(DataForSyscallTest& data) : data_(data) {}

  void AddOrChangeBreakpoint(
      const debug_ipc::AddOrChangeBreakpointRequest& request,
      fit::callback<void(const zxdb::Err&, debug_ipc::AddOrChangeBreakpointReply)> cb) override {
    breakpoints_[request.breakpoint.id] = request.breakpoint;
    MockRemoteAPI::AddOrChangeBreakpoint(request, std::move(cb));
  }

  void Attach(const debug_ipc::AttachRequest& request,
              fit::callback<void(const zxdb::Err&, debug_ipc::AttachReply)> cb) override {
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb)]() mutable { cb(zxdb::Err(), debug_ipc::AttachReply()); });
  }

  void Modules(const debug_ipc::ModulesRequest& request,
               fit::callback<void(const zxdb::Err&, debug_ipc::ModulesReply)> cb) override {
    debug_ipc::ModulesReply reply;
    data_.PopulateModules(reply.modules);
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb), reply]() mutable { cb(zxdb::Err(), reply); });
  }

  void ReadMemory(const debug_ipc::ReadMemoryRequest& request,
                  fit::callback<void(const zxdb::Err&, debug_ipc::ReadMemoryReply)> cb) override {
    debug_ipc::ReadMemoryReply reply;
    data_.PopulateMemoryBlockForAddress(request.address, request.size, reply.blocks.emplace_back());
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb), reply]() mutable { cb(zxdb::Err(), reply); });
  }

  void ReadRegisters(
      const debug_ipc::ReadRegistersRequest& request,
      fit::callback<void(const zxdb::Err&, debug_ipc::ReadRegistersReply)> cb) override {
    // TODO: Parameterize this so we can have more than one test.
    debug_ipc::ReadRegistersReply reply;
    data_.PopulateRegisters(request.process_koid, reply.categories.emplace_back());
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb), reply]() mutable { cb(zxdb::Err(), reply); });
  }

  void Resume(const debug_ipc::ResumeRequest& request,
              fit::callback<void(const zxdb::Err&, debug_ipc::ResumeReply)> cb) override {
    debug_ipc::ResumeReply reply;
    data_.Step(request.process_koid);
    debug_ipc::MessageLoop::Current()->PostTask(FROM_HERE, [cb = std::move(cb), reply]() mutable {
      cb(zxdb::Err(), reply);
      // This is so that the test can inject the next exception.
      debug_ipc::MessageLoop::Current()->QuitNow();
    });
  }

  void PopulateBreakpointIds(uint64_t address, debug_ipc::NotifyException& notification) {
    for (auto& breakpoint : breakpoints_) {
      if (address == breakpoint.second.locations[0].address) {
        notification.hit_breakpoints.emplace_back();
        notification.hit_breakpoints.back().id = breakpoint.first;
      }
    }
  }

 private:
  std::map<uint32_t, debug_ipc::BreakpointSettings> breakpoints_;
  DataForSyscallTest& data_;
};

class InterceptionWorkflowTest : public zxdb::RemoteAPITest {
 public:
  explicit InterceptionWorkflowTest(debug_ipc::Arch arch) : data_(arch) {
    display_options_.pretty_print = true;
    display_options_.columns = 132;
    display_options_.needs_colors = true;
  }
  ~InterceptionWorkflowTest() override = default;

  InterceptionRemoteAPI& mock_remote_api() { return *mock_remote_api_; }

  std::unique_ptr<zxdb::RemoteAPI> GetRemoteAPIImpl() override {
    auto remote_api = std::make_unique<InterceptionRemoteAPI>(data_);
    mock_remote_api_ = remote_api.get();
    return std::move(remote_api);
  }

  DataForSyscallTest& data() { return data_; }

  void set_with_process_info() { display_options_.with_process_info = true; }

  void PerformCheckTest(const char* syscall_name, std::unique_ptr<SystemCallTest> syscall1,
                        std::unique_ptr<SystemCallTest> syscall2);

  void PerformDisplayTest(const char* syscall_name, std::unique_ptr<SystemCallTest> syscall,
                          const char* expected);

  void PerformInterleavedDisplayTest(const char* syscall_name,
                                     std::unique_ptr<SystemCallTest> syscall, const char* expected);

  void PerformTest(const char* syscall_name, std::unique_ptr<SystemCallTest> syscall1,
                   std::unique_ptr<SystemCallTest> syscall2, ProcessController* controller,
                   std::unique_ptr<SyscallDecoderDispatcher> dispatcher, bool interleaved_test);

  void SimulateSyscall(std::unique_ptr<SystemCallTest> syscall, ProcessController* controller,
                       bool interleaved_test);
  void TriggerSyscallBreakpoint(uint64_t process_koid, uint64_t thread_koid);
  void TriggerCallerBreakpoint(uint64_t process_koid, uint64_t thread_koid);

 protected:
  DataForSyscallTest data_;
  InterceptionRemoteAPI* mock_remote_api_;  // Owned by the session.
  DecodeOptions decode_options_;
  DisplayOptions display_options_;
  std::stringstream result_;
};

class InterceptionWorkflowTestX64 : public InterceptionWorkflowTest {
 public:
  InterceptionWorkflowTestX64() : InterceptionWorkflowTest(GetArch()) {}
  ~InterceptionWorkflowTestX64() override = default;

  virtual debug_ipc::Arch GetArch() const override { return debug_ipc::Arch::kX64; }
};

class InterceptionWorkflowTestArm : public InterceptionWorkflowTest {
 public:
  InterceptionWorkflowTestArm() : InterceptionWorkflowTest(GetArch()) {}
  ~InterceptionWorkflowTestArm() override = default;

  virtual debug_ipc::Arch GetArch() const override { return debug_ipc::Arch::kArm64; }
};

// This does process setup for the test.  It creates fake processes, injects
// modules with the appropriate symbols, attaches to the processes, etc.
class ProcessController {
 public:
  ProcessController(InterceptionWorkflowTest* remote_api, zxdb::Session& session,
                    debug_ipc::PlatformMessageLoop& loop);
  ~ProcessController();

  InterceptionWorkflowTest* remote_api() const { return remote_api_; }
  InterceptionWorkflow& workflow() { return workflow_; }
  const std::vector<uint64_t>& process_koids() { return process_koids_; }
  uint64_t thread_koid(uint64_t process_koid) { return thread_koids_[process_koid]; }

  void InjectProcesses(zxdb::Session& session);

  void Initialize(zxdb::Session& session, std::unique_ptr<SyscallDecoderDispatcher> dispatcher,
                  const char* syscall_name);
  void Detach();

 private:
  InterceptionWorkflowTest* remote_api_;
  std::vector<uint64_t> process_koids_;
  std::map<uint64_t, uint64_t> thread_koids_;
  InterceptionWorkflow workflow_;

  std::vector<zxdb::Process*> processes_;
  std::vector<zxdb::Target*> targets_;
};

class AlwaysQuit {
 public:
  AlwaysQuit(ProcessController* controller) : controller_(controller) {}
  ~AlwaysQuit() { controller_->Detach(); }

 private:
  ProcessController* controller_;
};

template <typename T>
void AppendElements(std::string& result, const T* a, const T* b, size_t num) {
  std::ostringstream os;
  os << "actual      expected\n";
  for (size_t i = 0; i < num; i++) {
    os << std::left << std::setw(11) << static_cast<uint32_t>(a[i]);
    os << " ";
    os << std::left << std::setw(11) << static_cast<uint32_t>(b[i]);
    os << std::endl;
  }
  result.append(os.str());
}

class SyscallCheck : public SyscallUse {
 public:
  explicit SyscallCheck(ProcessController* controller) : controller_(controller) {}

  void SyscallOutputsDecoded(SyscallDecoder* decoder) override {
    if (decoder->syscall()->name() == "zx_channel_write") {
      DataForSyscallTest& data = controller_->remote_api()->data();
      FXL_DCHECK(decoder->ArgumentValue(0) == kHandle);  // handle
      FXL_DCHECK(decoder->ArgumentValue(1) == 0);        // options
      FXL_DCHECK(decoder->ArgumentLoaded(2, data.num_bytes()));
      uint8_t* bytes = decoder->ArgumentContent(2);
      if (memcmp(bytes, data.bytes(), data.num_bytes()) != 0) {
        std::string result = "bytes not equivalent\n";
        AppendElements(result, bytes, data.bytes(), data.num_bytes());
        decoder->Destroy();
        FAIL() << result;
      }
      FXL_DCHECK(decoder->ArgumentValue(3) == data.num_bytes());  // num_bytes
      FXL_DCHECK(decoder->ArgumentLoaded(4, data.num_handles() * sizeof(zx_handle_t)));
      zx_handle_t* handles = reinterpret_cast<zx_handle_t*>(decoder->ArgumentContent(4));
      if (memcmp(handles, data.handles(), data.num_handles()) != 0) {
        std::string result = "handles not equivalent";
        AppendElements(result, handles, data.handles(), data.num_handles());
        decoder->Destroy();
        FAIL() << result;
      }
      FXL_DCHECK(decoder->ArgumentValue(5) == data.num_handles());  // num_handles
      decoder->Destroy();
    } else if (decoder->syscall()->name() == "zx_channel_call") {
      DataForSyscallTest& data = controller_->remote_api()->data();
      FXL_DCHECK(decoder->ArgumentValue(0) == kHandle);           // handle
      FXL_DCHECK(decoder->ArgumentValue(1) == 0);                 // options
      FXL_DCHECK(decoder->ArgumentValue(2) == ZX_TIME_INFINITE);  // deadline
      FXL_DCHECK(decoder->ArgumentLoaded(3, sizeof(zx_channel_call_args_t)));
      const zx_channel_call_args_t* args =
          reinterpret_cast<const zx_channel_call_args_t*>(decoder->ArgumentContent(3));
      uint8_t* ref_bytes;
      uint32_t ref_num_bytes;
      if (data.use_alternate_data()) {
        ref_bytes = data.bytes2();
        ref_num_bytes = data.num_bytes2();
      } else {
        ref_bytes = data.bytes();
        ref_num_bytes = data.num_bytes();
      }
      FXL_DCHECK(args->wr_num_bytes == ref_num_bytes);
      FXL_DCHECK(decoder->BufferLoaded(uint64_t(args->wr_bytes), args->wr_num_bytes));
      uint8_t* bytes = decoder->BufferContent(uint64_t(args->wr_bytes));
      if (memcmp(bytes, ref_bytes, ref_num_bytes) != 0) {
        std::string result = "bytes not equivalent\n";
        AppendElements(result, bytes, ref_bytes, ref_num_bytes);
        decoder->Destroy();
        FAIL() << result;
      }
      decoder->Destroy();
    } else {
      FAIL() << "can't check " << decoder->syscall()->name();
    }
  }

  void SyscallDecodingError(const SyscallDecoderError& error, SyscallDecoder* decoder) override {
    SyscallUse::SyscallDecodingError(error, decoder);
    FAIL();
  }

 private:
  ProcessController* controller_;
};

class SyscallDecoderDispatcherTest : public SyscallDecoderDispatcher {
 public:
  SyscallDecoderDispatcherTest(const DecodeOptions& decode_options, ProcessController* controller)
      : SyscallDecoderDispatcher(decode_options), controller_(controller) {}

  std::unique_ptr<SyscallDecoder> CreateDecoder(InterceptingThreadObserver* thread_observer,
                                                zxdb::Thread* thread, uint64_t thread_id,
                                                const Syscall* syscall) override {
    return std::make_unique<SyscallDecoder>(this, thread_observer, thread, thread_id, syscall,
                                            std::make_unique<SyscallCheck>(controller_));
  }

  void DeleteDecoder(SyscallDecoder* decoder) override {
    SyscallDecoderDispatcher::DeleteDecoder(decoder);
    AlwaysQuit aq(controller_);
  }

 private:
  ProcessController* controller_;
};

class SyscallDisplayDispatcherTest : public SyscallDisplayDispatcher {
 public:
  SyscallDisplayDispatcherTest(LibraryLoader* loader, const DecodeOptions& decode_options,
                               const DisplayOptions& display_options, std::ostream& os,
                               ProcessController* controller)
      : SyscallDisplayDispatcher(loader, decode_options, display_options, os),
        controller_(controller) {}

  ProcessController* controller() const { return controller_; }

  void DeleteDecoder(SyscallDecoder* decoder) override {
    SyscallDisplayDispatcher::DeleteDecoder(decoder);
    AlwaysQuit aq(controller_);
  }

 private:
  ProcessController* controller_;
};

}  // namespace fidlcat

#endif  // TOOLS_FIDLCAT_INTERCEPTION_TESTS_INTERCEPTION_WORKFLOW_TEST_H_
