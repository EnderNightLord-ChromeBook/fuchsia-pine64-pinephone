// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/debugged_thread.h"

#include "gtest/gtest.h"
#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/debugged_process.h"
#include "src/developer/debug/debug_agent/object_util.h"

namespace debug_agent {

using namespace debug_ipc;

namespace {

// TODO(donosoc): These helpers are replicated throughout many tests.
//                Collapse them into one place.
std::vector<uint8_t> CreateData(size_t length) {
  std::vector<uint8_t> data;
  data.reserve(length);
  // So that we get the number backwards (0x0102...).
  uint8_t base = length;
  for (size_t i = 0; i < length; i++) {
    data.emplace_back(base - i);
  }
  return data;
}

debug_ipc::Register CreateRegister(RegisterID id, size_t length) {
  debug_ipc::Register reg;
  reg.id = id;
  reg.data = CreateData(length);
  return reg;
}

bool FindRegister(const std::vector<Register>& regs, RegisterID id) {
  for (const auto& reg : regs) {
    if (reg.id == id)
      return true;
  }
  return false;
}

class FakeArchProvider : public arch::ArchProvider {
 public:
  zx_status_t ReadRegisters(const debug_ipc::RegisterCategory::Type& type,
                            const zx::thread&,
                            std::vector<debug_ipc::Register>* out) override {
    auto it = categories_.find(type);
    if (it == categories_.end())
      return ZX_ERR_INVALID_ARGS;

    *out = it->second.registers;
    return ZX_OK;
  }

  zx_status_t WriteRegisters(const debug_ipc::RegisterCategory& cat,
                             zx::thread*) override {
    auto& written_cat = regs_written_[cat.type];
    for (const Register& reg : cat.registers) {
      written_cat.push_back(reg);
    }

    return ZX_OK;
  }

  void AddCategory(RegisterCategory::Type type, size_t reg_count) {
    auto& cat = categories_[type];
    cat.type = type;
    cat.registers.reserve(reg_count);
    for (uint32_t i = 0; i < reg_count; i++) {
      auto& reg = cat.registers.emplace_back();
      reg.id = static_cast<debug_ipc::RegisterID>(i);
      // No data for now.
    }
  }

  const std::map<RegisterCategory::Type, std::vector<Register>>& regs_written()
      const {
    return regs_written_;
  }

 private:
  std::map<RegisterCategory::Type, RegisterCategory> categories_;
  std::map<RegisterCategory::Type, std::vector<Register>> regs_written_;
};

class ScopedFakeArchProvider {
 public:
  ScopedFakeArchProvider() {
    auto fake_arch = std::make_unique<FakeArchProvider>();
    fake_arch_ = fake_arch.get();
    arch::ArchProvider::Set(std::move(fake_arch));
  }

  ~ScopedFakeArchProvider() { arch::ArchProvider::Set(nullptr); }

  FakeArchProvider* get() const { return fake_arch_; }

 private:
  FakeArchProvider* fake_arch_;
};

class FakeProcess : public DebuggedProcess {
 public:
  FakeProcess(zx_koid_t koid)
      : DebuggedProcess(nullptr, {koid, zx::process()}) {}
  ~FakeProcess() = default;

  DebuggedThread* CreateThread(zx_koid_t tid) {
    if (!thread_) {
      thread_ = std::make_unique<DebuggedThread>(
          this, zx::thread(), tid, zx::exception(),
          ThreadCreationOption::kSuspendedKeepSuspended);
    }
    return thread_.get();
  }

 private:
  std::unique_ptr<DebuggedThread> thread_;
};

TEST(DebuggedThread, ReadRegisters) {
  ScopedFakeArchProvider scoped_arch_provider;
  FakeArchProvider* arch = scoped_arch_provider.get();

  constexpr size_t kGeneralCount = 12u;
  arch->AddCategory(RegisterCategory::Type::kGeneral, kGeneralCount);

  FakeProcess fake_process(1u);
  DebuggedThread* thread = fake_process.CreateThread(1u);

  std::vector<RegisterCategory::Type> cats_to_get = {
      RegisterCategory::Type::kGeneral};

  std::vector<RegisterCategory> categories;
  thread->ReadRegisters(cats_to_get, &categories);

  ASSERT_EQ(categories.size(), 1u);
  auto& cat = categories.front();
  EXPECT_EQ(cat.type, RegisterCategory::Type::kGeneral);
  EXPECT_EQ(cat.registers.size(), kGeneralCount);
}

TEST(DebuggedThread, ReadRegistersGettingErrorShouldStillReturnTheRest) {
  ScopedFakeArchProvider scoped_arch_provider;
  FakeArchProvider* arch = scoped_arch_provider.get();

  FakeProcess fake_process(1u);
  DebuggedThread* thread = fake_process.CreateThread(1u);

  constexpr size_t kGeneralCount = 12u;
  constexpr size_t kDebugCount = 33u;
  arch->AddCategory(RegisterCategory::Type::kGeneral, kGeneralCount);
  arch->AddCategory(RegisterCategory::Type::kDebug, kDebugCount);

  std::vector<RegisterCategory::Type> cats_to_get = {
      RegisterCategory::Type::kGeneral, RegisterCategory::Type::kVector,
      RegisterCategory::Type::kDebug};

  std::vector<RegisterCategory> categories;
  thread->ReadRegisters(cats_to_get, &categories);

  ASSERT_EQ(categories.size(), 2u);
  EXPECT_EQ(categories[0].type, RegisterCategory::Type::kGeneral);
  EXPECT_EQ(categories[0].registers.size(), kGeneralCount);
  EXPECT_EQ(categories[1].type, RegisterCategory::Type::kDebug);
  EXPECT_EQ(categories[1].registers.size(), kDebugCount);
}

TEST(DebuggedThread, WriteRegisters) {
  ScopedFakeArchProvider scoped_arch_provider;
  FakeArchProvider* arch = scoped_arch_provider.get();

  FakeProcess fake_process(1u);
  DebuggedThread* thread = fake_process.CreateThread(1u);

  std::vector<Register> regs_to_write;
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_rax, 16));
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_rip, 16));
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_rsp, 16));

  regs_to_write.push_back(CreateRegister(RegisterID::kX64_fcw, 16));
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_st0, 16));

  regs_to_write.push_back(CreateRegister(RegisterID::kX64_mxcsr, 16));
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_ymm1, 16));
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_ymm2, 16));
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_ymm3, 16));

  regs_to_write.push_back(CreateRegister(RegisterID::kX64_dr1, 16));
  regs_to_write.push_back(CreateRegister(RegisterID::kX64_dr7, 16));

  thread->WriteRegisters(regs_to_write);

  const auto& regs_written = arch->regs_written();
  ASSERT_EQ(regs_written.size(), 4u);
  EXPECT_EQ(regs_written.count(RegisterCategory::Type::kNone), 0u);

  auto it = regs_written.find(RegisterCategory::Type::kGeneral);
  ASSERT_NE(it, regs_written.end());
  ASSERT_EQ(it->second.size(), 3u);
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_rax));
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_rip));
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_rsp));

  it = regs_written.find(RegisterCategory::Type::kFP);
  ASSERT_NE(it, regs_written.end());
  ASSERT_EQ(it->second.size(), 2u);
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_fcw));
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_st0));

  it = regs_written.find(RegisterCategory::Type::kVector);
  ASSERT_NE(it, regs_written.end());
  ASSERT_EQ(it->second.size(), 4u);
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_mxcsr));
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_ymm1));
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_ymm2));
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_ymm3));

  it = regs_written.find(RegisterCategory::Type::kDebug);
  ASSERT_NE(it, regs_written.end());
  ASSERT_EQ(it->second.size(), 2u);
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_dr1));
  EXPECT_TRUE(FindRegister(it->second, RegisterID::kX64_dr7));
}

TEST(DebuggedThread, FillThreadRecord) {
  constexpr zx_koid_t kProcessKoid = 0x8723456;
  FakeProcess fake_process(kProcessKoid);

  zx::thread current_thread;
  zx::thread::self()->duplicate(ZX_RIGHT_SAME_RIGHTS, &current_thread);

  zx_koid_t current_thread_koid = KoidForObject(current_thread);

  // Set the name of the current thread so we can find it.
  const std::string thread_name("ProcessInfo test thread name");
  std::string old_name = NameForObject(current_thread);
  current_thread.set_property(ZX_PROP_NAME, thread_name.c_str(),
                              thread_name.size());
  EXPECT_EQ(thread_name, NameForObject(current_thread));

  auto thread = std::make_unique<DebuggedThread>(
      &fake_process, std::move(current_thread), current_thread_koid,
      zx::exception(), ThreadCreationOption::kRunningKeepRunning);

  // Request no stack since this thread should be running.
  debug_ipc::ThreadRecord record;
  thread->FillThreadRecord(debug_ipc::ThreadRecord::StackAmount::kNone, nullptr,
                           &record);

  // Put back the old thread name for hygiene.
  zx::thread::self()->set_property(ZX_PROP_NAME, old_name.c_str(),
                                   old_name.size());

  // Validate the results.
  EXPECT_EQ(kProcessKoid, record.process_koid);
  EXPECT_EQ(current_thread_koid, record.thread_koid);
  EXPECT_EQ(thread_name, record.name);
  EXPECT_EQ(debug_ipc::ThreadRecord::State::kRunning, record.state);
  EXPECT_EQ(debug_ipc::ThreadRecord::StackAmount::kNone, record.stack_amount);
  EXPECT_TRUE(record.frames.empty());
}

}  // namespace

}  // namespace debug_agent
