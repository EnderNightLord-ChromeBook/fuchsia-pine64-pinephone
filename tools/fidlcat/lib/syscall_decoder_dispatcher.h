// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDLCAT_LIB_SYSCALL_DECODER_DISPATCHER_H_
#define TOOLS_FIDLCAT_LIB_SYSCALL_DECODER_DISPATCHER_H_

#include <src/lib/fxl/logging.h>
#include <zircon/system/public/zircon/errors.h>
#include <zircon/system/public/zircon/types.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/developer/debug/zxdb/client/thread.h"
#include "tools/fidlcat/lib/message_decoder.h"
#include "tools/fidlcat/lib/syscall_decoder.h"

namespace fidlcat {

class DisplayTime {
 public:
  DisplayTime(const Colors& colors, zx_time_t time_ns) : colors_(colors), time_ns_(time_ns) {}
  const Colors& colors() const { return colors_; }
  zx_time_t time_ns() const { return time_ns_; }

 private:
  const Colors& colors_;
  const zx_time_t time_ns_;
};

inline std::ostream& operator<<(std::ostream& os, const DisplayTime& time) {
  if (time.time_ns() == ZX_TIME_INFINITE) {
    os << time.colors().blue << "ZX_TIME_INFINITE" << time.colors().reset;
  } else if (time.time_ns() == ZX_TIME_INFINITE_PAST) {
    os << time.colors().blue << "ZX_TIME_INFINITE_PAST" << time.colors().reset;
  } else {
    time_t value = time.time_ns() / 1'000'000'000L;
    struct tm tm;
    if (localtime_r(&value, &tm) == &tm) {
      char buffer[100];
      strftime(buffer, sizeof(buffer), "%c", &tm);
      os << time.colors().blue << buffer << " and ";
      snprintf(buffer, sizeof(buffer), "%09ld", time.time_ns() % 1000000000L);
      os << buffer << " ns" << time.colors().reset;
    } else {
      os << time.colors().red << "unknown time" << time.colors().reset;
    }
  }
  return os;
}

// Base class (not templated) for system call arguments.
class SyscallArgumentBase {
 public:
  SyscallArgumentBase(int index, SyscallType syscall_type)
      : index_(index), syscall_type_(syscall_type) {}
  virtual ~SyscallArgumentBase() = default;

  int index() const { return index_; }
  SyscallType syscall_type() const { return syscall_type_; }

 private:
  const int index_;
  const SyscallType syscall_type_;
};

template <typename Type>
class SyscallArgumentBaseTyped : public SyscallArgumentBase {
 public:
  SyscallArgumentBaseTyped(int index, SyscallType syscall_type)
      : SyscallArgumentBase(index, syscall_type) {}

  // Ensures that the argument data will be in memory.
  virtual void Load(SyscallDecoder* decoder) const {}

  // True if the argument data is available.
  virtual bool Loaded(SyscallDecoder* decoder) const { return false; }

  // True if the argument data is valid (not a null pointer).
  virtual bool ValueValid(SyscallDecoder* decoder) const { return false; }

  // The data for the argument.
  virtual Type Value(SyscallDecoder* decoder) const { return Type(); }

  // For buffers, ensures that the buffer will be in memory.
  virtual void LoadArray(SyscallDecoder* decoder, size_t size) const {}

  // For buffers, true if the buffer is available.
  virtual bool ArrayLoaded(SyscallDecoder* decoder, size_t size) const { return false; }

  // For buffers, get a pointer on the buffer data.
  virtual Type* Content(SyscallDecoder* decoder) const { return nullptr; }
};

// Defines an basic type argument for a system call.
// A basic type argument can be stored in a 64 bit register.
template <typename Type>
class SyscallArgument : public SyscallArgumentBaseTyped<Type> {
 public:
  SyscallArgument(int index, SyscallType syscall_type)
      : SyscallArgumentBaseTyped<Type>(index, syscall_type) {}

  // Redefine index within the class to avoid a compiler error.
  int index() const { return SyscallArgumentBase::index(); }

  bool Loaded(SyscallDecoder* decoder) const override { return true; }

  bool ValueValid(SyscallDecoder* decoder) const override { return true; }

  Type Value(SyscallDecoder* decoder) const override { return Type(decoder->Value(index())); }
};

// Defines a buffer argument for a system call.
// A buffer argument is defined by a pointer which can be stored in a 64 bit
// register. The data for the buffer stays in memory (referenced by the
// pointer).
template <typename Type>
class SyscallPointerArgument : public SyscallArgumentBaseTyped<Type> {
 public:
  SyscallPointerArgument(int index, SyscallType syscall_type)
      : SyscallArgumentBaseTyped<Type>(index, syscall_type) {}

  int index() const { return SyscallArgumentBase::index(); }

  void Load(SyscallDecoder* decoder) const override {
    decoder->LoadArgument(index(), sizeof(Type));
  }

  bool Loaded(SyscallDecoder* decoder) const override {
    return decoder->Loaded(index(), sizeof(Type));
  }

  bool ValueValid(SyscallDecoder* decoder) const override {
    return decoder->Content(index()) != nullptr;
  }

  Type Value(SyscallDecoder* decoder) const override {
    uint8_t* content = decoder->Content(index());
    if (content == nullptr) {
      return Type();
    }
    return *reinterpret_cast<Type*>(content);
  }

  void LoadArray(SyscallDecoder* decoder, size_t size) const override {
    decoder->LoadArgument(index(), size);
  }

  bool ArrayLoaded(SyscallDecoder* decoder, size_t size) const override {
    return decoder->Loaded(index(), size);
  }

  Type* Content(SyscallDecoder* decoder) const override {
    return reinterpret_cast<Type*>(decoder->Content(index()));
  }
};

// Use to access data for an input or an output.
template <typename Type>
class Access {
 public:
  Access() = default;
  virtual ~Access() = default;

  // Returns the real type of the data (because, for example, handles are
  // implemented as uint32_t).
  virtual SyscallType GetSyscallType() const = 0;

  // Ensures that the data will be in memory.
  virtual void Load(SyscallDecoder* decoder) const = 0;

  // True if the data is available.
  virtual bool Loaded(SyscallDecoder* decoder) const = 0;

  // True if the data is valid (not a null pointer).
  virtual bool ValueValid(SyscallDecoder* decoder) const = 0;

  // The data.
  virtual Type Value(SyscallDecoder* decoder) const = 0;

  // For buffers, ensures that the buffer will be in memory.
  virtual void LoadArray(SyscallDecoder* decoder, size_t size) = 0;

  // For buffers, true if the buffer is available.
  virtual bool ArrayLoaded(SyscallDecoder* decoder, size_t size) const = 0;

  // For buffers, get a pointer on the buffer data.
  virtual const Type* Content(SyscallDecoder* decoder) const = 0;

  // Display the data on a stream (with name and type).
  void Display(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder, std::string_view name,
               std::ostream& os) const;

  // Display a value on a stream
  void DisplayValue(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder, Type value,
                    std::ostream& os) const;
};

// Access to a system call argument. There is a direct access to the value
// given when the system call is called. For struct or buffer input arguments
// and for output arguments (all are pointers), we need to load the referenced
// data to be able to access the actual content. This is done within LoadInputs
// at the system call entry for input arguments. This is done within LoadOutputs
// after the system call returns for output arguments.
// All the basic types values and the pointer values are read at the system call
// entry.
template <typename Type>
class ArgumentAccess : public Access<Type> {
 public:
  explicit ArgumentAccess(const SyscallArgumentBaseTyped<Type>* argument) : argument_(argument) {}

  SyscallType GetSyscallType() const override { return argument_->syscall_type(); }

  void Load(SyscallDecoder* decoder) const override { argument_->Load(decoder); }

  bool Loaded(SyscallDecoder* decoder) const override { return argument_->Loaded(decoder); }

  bool ValueValid(SyscallDecoder* decoder) const override { return argument_->ValueValid(decoder); }

  Type Value(SyscallDecoder* decoder) const override { return argument_->Value(decoder); }

  void LoadArray(SyscallDecoder* decoder, size_t size) override {
    argument_->LoadArray(decoder, size);
  }

  bool ArrayLoaded(SyscallDecoder* decoder, size_t size) const override {
    return argument_->ArrayLoaded(decoder, size);
  }

  const Type* Content(SyscallDecoder* decoder) const override {
    return argument_->Content(decoder);
  }

 private:
  const SyscallArgumentBaseTyped<Type>* const argument_;
};

// Access to a field of a system call argument.
template <typename ClassType, typename Type>
class FieldAccess : public Access<Type> {
 public:
  explicit FieldAccess(const SyscallPointerArgument<ClassType>* argument,
                       Type (*get)(const ClassType* from), SyscallType syscall_type)
      : argument_(argument), get_(get), syscall_type_(syscall_type) {}

  SyscallType GetSyscallType() const override { return syscall_type_; }

  void Load(SyscallDecoder* decoder) const override {
    argument_->LoadArray(decoder, sizeof(ClassType));
  }

  bool Loaded(SyscallDecoder* decoder) const override {
    return argument_->ArrayLoaded(decoder, sizeof(ClassType));
  }

  bool ValueValid(SyscallDecoder* decoder) const override {
    return argument_->Content(decoder) != nullptr;
  }

  Type Value(SyscallDecoder* decoder) const override { return get_(argument_->Content(decoder)); }

  void LoadArray(SyscallDecoder* decoder, size_t size) override {}

  bool ArrayLoaded(SyscallDecoder* decoder, size_t size) const override { return false; }

  const Type* Content(SyscallDecoder* decoder) const override { return nullptr; }

 private:
  const SyscallPointerArgument<ClassType>* const argument_;
  Type (*get_)(const ClassType* from);
  const SyscallType syscall_type_;
};

// Access to a field of a system call argument.
template <typename ClassType, typename Type>
class PointerFieldAccess : public Access<Type> {
 public:
  explicit PointerFieldAccess(const SyscallPointerArgument<ClassType>* argument,
                              const Type* (*get)(const ClassType* from), SyscallType syscall_type)
      : argument_(argument), get_(get), syscall_type_(syscall_type) {}

  SyscallType GetSyscallType() const override { return syscall_type_; }

  void Load(SyscallDecoder* decoder) const override {}

  bool Loaded(SyscallDecoder* decoder) const override { return false; }

  bool ValueValid(SyscallDecoder* decoder) const override { return false; }

  Type Value(SyscallDecoder* decoder) const override { return {}; }

  void LoadArray(SyscallDecoder* decoder, size_t size) override {
    if (loading_) {
      return;
    }
    argument_->LoadArray(decoder, sizeof(ClassType));
    if (argument_->ArrayLoaded(decoder, sizeof(ClassType))) {
      ClassType* object = argument_->Content(decoder);
      if (object != nullptr) {
        loading_ = true;
        decoder->LoadMemory(reinterpret_cast<uint64_t>(get_(object)), size, &loaded_values_);
      }
    }
  }

  bool ArrayLoaded(SyscallDecoder* decoder, size_t size) const override {
    return loaded_values_.size() == size;
  }

  const Type* Content(SyscallDecoder* decoder) const override {
    return reinterpret_cast<const Type*>(loaded_values_.data());
  }

 private:
  const SyscallPointerArgument<ClassType>* const argument_;
  const Type* (*get_)(const ClassType* from);
  const SyscallType syscall_type_;
  std::vector<uint8_t> loaded_values_;
  bool loading_ = false;
};

// Base class for the inputs/outputs we want to display for a system call.
class SyscallInputOutputBase {
 public:
  explicit SyscallInputOutputBase(int64_t error_code, std::string_view name)
      : error_code_(error_code), name_(name) {}
  virtual ~SyscallInputOutputBase() = default;

  // For outputs, error code which must have been returned to be able to display
  // the ouput.
  int64_t error_code() const { return error_code_; }

  // Name of the input/output.
  const std::string& name() const { return name_; }

  // Ensures that all the data needed to display the input/output is available.
  virtual void Load(SyscallDecoder* decoder) const = 0;

  // Displays small inputs or outputs.
  virtual const char* DisplayInline(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder,
                                    const char* separator, std::ostream& os) const {
    return separator;
  }

  // Displays large (multi lines) inputs or outputs.
  virtual void DisplayOutline(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder,
                              std::string_view line_header, int tabs, std::ostream& os) const {}

 private:
  const int64_t error_code_;
  const std::string name_;
};

// An input/output which only displays an expression (for example, the value of
// an argument). This is always decoded inline.
template <typename Type>
class SyscallInputOutput : public SyscallInputOutputBase {
 public:
  SyscallInputOutput(int64_t error_code, std::string_view name,
                     std::unique_ptr<Access<Type>> access)
      : SyscallInputOutputBase(error_code, name), access_(std::move(access)) {}

  void Load(SyscallDecoder* decoder) const override { access_->Load(decoder); }

  const char* DisplayInline(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder,
                            const char* separator, std::ostream& os) const override {
    os << separator;
    access_->Display(dispatcher, decoder, name(), os);
    return ", ";
  }

 private:
  const std::unique_ptr<Access<Type>> access_;
};

// An input/output which is a FIDL message. This is always displayed outline.
class SyscallFidlMessage : public SyscallInputOutputBase {
 public:
  SyscallFidlMessage(int64_t error_code, std::string_view name, SyscallFidlType type,
                     std::unique_ptr<Access<zx_handle_t>> handle,
                     std::unique_ptr<Access<uint8_t>> bytes,
                     std::unique_ptr<Access<uint32_t>> num_bytes,
                     std::unique_ptr<Access<zx_handle_t>> handles,
                     std::unique_ptr<Access<uint32_t>> num_handles)
      : SyscallInputOutputBase(error_code, name),
        type_(type),
        handle_(std::move(handle)),
        bytes_(std::move(bytes)),
        num_bytes_(std::move(num_bytes)),
        handles_(std::move(handles)),
        num_handles_(std::move(num_handles)) {}

  void Load(SyscallDecoder* decoder) const override {
    handle_->Load(decoder);
    num_bytes_->Load(decoder);
    num_handles_->Load(decoder);

    if (num_bytes_->Loaded(decoder)) {
      uint32_t value = num_bytes_->Value(decoder);
      if (value > 0) {
        bytes_->LoadArray(decoder, value);
      }
    }

    if (num_handles_->Loaded(decoder)) {
      uint32_t value = num_handles_->Value(decoder);
      if (value > 0) {
        handles_->LoadArray(decoder, value * sizeof(zx_handle_t));
      }
    }
  }

  void DisplayOutline(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder,
                      std::string_view line_header, int tabs, std::ostream& os) const override;

 private:
  const SyscallFidlType type_;
  const std::unique_ptr<Access<zx_handle_t>> handle_;
  const std::unique_ptr<Access<uint8_t>> bytes_;
  const std::unique_ptr<Access<uint32_t>> num_bytes_;
  const std::unique_ptr<Access<zx_handle_t>> handles_;
  const std::unique_ptr<Access<uint32_t>> num_handles_;
};

// Defines a syscall we want to decode/display.
class Syscall {
 public:
  explicit Syscall(std::string_view name) : name_(name), breakpoint_name_(name_ + "@plt") {}

  // Name of the syscall.
  const std::string& name() const { return name_; }

  // Name of the breakpoint used to watch the syscall.
  const std::string& breakpoint_name() const { return breakpoint_name_; }

  // All arguments for the syscall.
  const std::vector<std::unique_ptr<SyscallArgumentBase>>& arguments() const { return arguments_; }

  // All the data we want to display at the syscall entry.
  const std::vector<std::unique_ptr<SyscallInputOutputBase>>& inputs() const { return inputs_; }

  // All the data we want to display at the syscall exit. These data are
  // conditionally displayed depending on the syscall error code.
  const std::vector<std::unique_ptr<SyscallInputOutputBase>>& outputs() const { return outputs_; }

  // Adds an argument definition to the syscall.
  template <typename Type>
  SyscallArgument<Type>* Argument(SyscallType syscall_type) {
    auto argument = std::make_unique<SyscallArgument<Type>>(arguments_.size(), syscall_type);
    auto result = argument.get();
    arguments_.push_back(std::move(argument));
    return result;
  }

  // Adds a pointer argument definition to the syscall (the actual type of the
  // argument is Type*).
  template <typename Type>
  SyscallPointerArgument<Type>* PointerArgument(SyscallType syscall_type) {
    auto argument = std::make_unique<SyscallPointerArgument<Type>>(arguments_.size(), syscall_type);
    auto result = argument.get();
    arguments_.push_back(std::move(argument));
    return result;
  }

  // Adds an inline input to display.
  template <typename Type>
  void Input(std::string_view name, std::unique_ptr<Access<Type>> access) {
    inputs_.push_back(std::make_unique<SyscallInputOutput<Type>>(0, name, std::move(access)));
  }

  // Adds an input FIDL message to display.
  void InputFidlMessage(std::string_view name, SyscallFidlType type,
                        std::unique_ptr<Access<zx_handle_t>> handle,
                        std::unique_ptr<Access<uint8_t>> bytes,
                        std::unique_ptr<Access<uint32_t>> num_bytes,
                        std::unique_ptr<Access<zx_handle_t>> handles,
                        std::unique_ptr<Access<uint32_t>> num_handles) {
    inputs_.push_back(std::make_unique<SyscallFidlMessage>(
        0, name, type, std::move(handle), std::move(bytes), std::move(num_bytes),
        std::move(handles), std::move(num_handles)));
  }

  // Adds an inline output to display.
  template <typename Type>
  void Output(int64_t error_code, std::string_view name, std::unique_ptr<Access<Type>> access) {
    outputs_.push_back(
        std::make_unique<SyscallInputOutput<Type>>(error_code, name, std::move(access)));
  }

  // Add an output FIDL message to display.
  void OutputFidlMessage(int64_t error_code, std::string_view name, SyscallFidlType type,
                         std::unique_ptr<Access<zx_handle_t>> handle,
                         std::unique_ptr<Access<uint8_t>> bytes,
                         std::unique_ptr<Access<uint32_t>> num_bytes,
                         std::unique_ptr<Access<zx_handle_t>> handles,
                         std::unique_ptr<Access<uint32_t>> num_handles) {
    outputs_.push_back(std::make_unique<SyscallFidlMessage>(
        error_code, name, type, std::move(handle), std::move(bytes), std::move(num_bytes),
        std::move(handles), std::move(num_handles)));
  }

 private:
  const std::string name_;
  const std::string breakpoint_name_;
  std::vector<std::unique_ptr<SyscallArgumentBase>> arguments_;
  std::vector<std::unique_ptr<SyscallInputOutputBase>> inputs_;
  std::vector<std::unique_ptr<SyscallInputOutputBase>> outputs_;
};

// Decoder for syscalls. This creates the breakpoints for all the syscalls we
// want to monitor. Then, each time a breakpoint is reached, it creates a
// SyscallDecoder object which will handle the decoding of one syscall.
class SyscallDecoderDispatcher {
 public:
  SyscallDecoderDispatcher() { Populate(); }
  virtual ~SyscallDecoderDispatcher() = default;

  const std::vector<std::unique_ptr<Syscall>>& syscalls() const { return syscalls_; }

  // Decode an intercepted system call.
  // Called when a thread reached a breakpoint on a system call.
  // This will only start the decoding. The display will be done when all the
  // needed information will be gathered.
  void DecodeSyscall(InterceptingThreadObserver* thread_observer, zxdb::Thread* thread,
                     Syscall* syscall);

  // Called when we are watching a process we launched.
  virtual void AddLaunchedProcess(uint64_t process_koid) {}

  // Create the object which will decode the syscall.
  virtual std::unique_ptr<SyscallDecoder> CreateDecoder(InterceptingThreadObserver* thread_observer,
                                                        zxdb::Thread* thread, uint64_t thread_id,
                                                        const Syscall* syscall) = 0;

  // Delete a decoder created by DecodeSyscall. Called when the syscall is
  // fully decoded and displayed or the syscalls had an error.
  virtual void DeleteDecoder(SyscallDecoder* decoder);

 private:
  // Feeds syscalls_ with all the syscalls we can decode.
  void Populate();

  // Add a syscall. Used by Populate.
  Syscall* Add(std::string_view name) {
    auto syscall = std::make_unique<Syscall>(name);
    auto result = syscall.get();
    syscalls_.push_back(std::move(syscall));
    return result;
  }

  // The definition of all the syscalls we can decode.
  std::vector<std::unique_ptr<Syscall>> syscalls_;

  // The intercepted syscalls we are currently decoding.
  std::map<uint64_t, std::unique_ptr<SyscallDecoder>> syscall_decoders_;
};

class SyscallDisplayDispatcher : public SyscallDecoderDispatcher {
 public:
  SyscallDisplayDispatcher(LibraryLoader* loader, const DisplayOptions& display_options,
                           std::ostream& os)
      : message_decoder_dispatcher_(loader, display_options), os_(os) {}

  MessageDecoderDispatcher& message_decoder_dispatcher() { return message_decoder_dispatcher_; }

  const Colors& colors() const { return message_decoder_dispatcher_.colors(); }

  bool with_process_info() const { return message_decoder_dispatcher_.with_process_info(); }

  const SyscallDisplay* last_displayed_syscall() const { return last_displayed_syscall_; }
  void set_last_displayed_syscall(const SyscallDisplay* last_displayed_syscall) {
    last_displayed_syscall_ = last_displayed_syscall;
  }

  void AddLaunchedProcess(uint64_t process_koid) override {
    message_decoder_dispatcher_.AddLaunchedProcess(process_koid);
  }

  std::unique_ptr<SyscallDecoder> CreateDecoder(InterceptingThreadObserver* thread_observer,
                                                zxdb::Thread* thread, uint64_t thread_id,
                                                const Syscall* syscall) override;

 private:
  // Class which can decode a FIDL message.
  MessageDecoderDispatcher message_decoder_dispatcher_;
  // The last syscall we displayed the inputs on the stream.
  const SyscallDisplay* last_displayed_syscall_ = nullptr;
  // The stream which will receive the syscall decodings.
  std::ostream& os_;
};

template <typename Type>
void Access<Type>::DisplayValue(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder,
                                Type value, std::ostream& os) const {
  os << "unimplemented generic value " << static_cast<uint32_t>(GetSyscallType());
}

template <>
inline void Access<uint32_t>::DisplayValue(SyscallDisplayDispatcher* dispatcher,
                                           SyscallDecoder* decoder, uint32_t value,
                                           std::ostream& os) const {
  const Colors& colors = dispatcher->colors();
  switch (GetSyscallType()) {
    case SyscallType::kUint32:
      os << colors.blue << value << colors.reset;
      break;
    case SyscallType::kHandle:
      os << colors.red << value << colors.reset;
      break;
    default:
      os << "unimplemented uint32_t value " << static_cast<uint32_t>(GetSyscallType());
      break;
  }
}

template <>
inline void Access<int64_t>::DisplayValue(SyscallDisplayDispatcher* dispatcher,
                                          SyscallDecoder* decoder, int64_t value,
                                          std::ostream& os) const {
  const Colors& colors = dispatcher->colors();
  switch (GetSyscallType()) {
    case SyscallType::kTime:
      os << DisplayTime(colors, value);
      break;
    default:
      os << "unimplemented int64_t value " << static_cast<uint32_t>(GetSyscallType());
      break;
  }
}
template <typename Type>
void Access<Type>::Display(SyscallDisplayDispatcher* dispatcher, SyscallDecoder* decoder,
                           std::string_view name, std::ostream& os) const {
  const Colors& colors = dispatcher->colors();
  switch (GetSyscallType()) {
    case SyscallType::kUint32:
      os << name << ":" << colors.green << "uint32" << colors.reset << ": ";
      break;
    case SyscallType::kHandle:
      os << name << ":" << colors.green << "handle" << colors.reset << ": ";
      break;
    case SyscallType::kTime:
      os << name << ":" << colors.green << "time" << colors.reset << ": ";
      break;
    default:
      os << name << ":" << colors.green << "unimplemented type "
         << static_cast<uint32_t>(GetSyscallType()) << colors.reset;
      return;
  }
  if (ValueValid(decoder)) {
    DisplayValue(dispatcher, decoder, Value(decoder), os);
  } else {
    os << colors.red << "(nullptr)" << colors.reset;
  }
}

}  // namespace fidlcat

#endif  // TOOLS_FIDLCAT_LIB_SYSCALL_DECODER_DISPATCHER_H_
