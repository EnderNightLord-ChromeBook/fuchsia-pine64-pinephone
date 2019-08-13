// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/time.h>
#include <trace/event.h>

#include <iostream>
#include <memory>

#include "peridot/lib/convert/convert.h"
#include "peridot/lib/rng/test_random.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/testing/data_generator.h"
#include "src/ledger/bin/testing/get_ledger.h"
#include "src/ledger/bin/testing/get_page_ensure_initialized.h"
#include "src/ledger/bin/testing/page_data_generator.h"
#include "src/ledger/bin/testing/quit_on_error.h"
#include "src/ledger/bin/testing/run_with_tracing.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_number_conversions.h"

namespace ledger {
namespace {

constexpr fxl::StringView kBinaryPath =
    "fuchsia-pkg://fuchsia.com/ledger_benchmarks#meta/get_entry.cmx";
constexpr fxl::StringView kStoragePath = "/data/benchmark/ledger/get_entry";
constexpr fxl::StringView kEntryCountFlag = "entry-count";
constexpr fxl::StringView kKeySizeFlag = "key-size";
constexpr fxl::StringView kValueSizeFlag = "value-size";
constexpr fxl::StringView kInlineFlag = "inline";

void PrintUsage() {
  std::cout << "Usage: trace record "
            << kBinaryPath
            // Comment to make clang format not break formatting.
            << " --" << kEntryCountFlag << "=<int>"
            << " --" << kKeySizeFlag << "=<int>"
            << " --" << kValueSizeFlag << "=<int>"
            << " [--" << kInlineFlag << "]" << std::endl;
}

// Benchmark that measures the time taken to read an entry from a page.
//
// Parameters:
//   --entry-count=<int> the number of entries to be put and retrieved
//   --key-size=<int> size of the keys for the entries
//   --value-size=<int> the size of a single value in bytes
//   --inline whether Get or GetInline method will be used (the latter retrieves
//   the entry directly as String).
class GetEntryBenchmark {
 public:
  GetEntryBenchmark(async::Loop* loop, std::unique_ptr<sys::ComponentContext> component_context,
                    size_t entry_count, size_t key_size, size_t value_size, bool get_inline);

  void Run();

 private:
  void Populate();
  void GetSnapshot();
  void GetKeys(std::unique_ptr<Token> token);
  void GetNextEntry(size_t i);
  void GetNextEntryInline(size_t i);
  void ShutDown();
  fit::closure QuitLoopClosure();

  async::Loop* const loop_;
  rng::TestRandom random_;
  files::ScopedTempDir tmp_dir_;
  DataGenerator generator_;
  PageDataGenerator page_data_generator_;
  std::unique_ptr<sys::ComponentContext> component_context_;
  const size_t entry_count_;
  const size_t key_size_;
  const size_t value_size_;
  const bool get_inline_;
  fuchsia::sys::ComponentControllerPtr component_controller_;
  LedgerPtr ledger_;
  PagePtr page_;
  PageSnapshotPtr snapshot_;
  std::vector<std::vector<uint8_t>> keys_;

  FXL_DISALLOW_COPY_AND_ASSIGN(GetEntryBenchmark);
};

GetEntryBenchmark::GetEntryBenchmark(async::Loop* loop,
                                     std::unique_ptr<sys::ComponentContext> component_context,
                                     size_t entry_count, size_t key_size, size_t value_size,
                                     bool get_inline)
    : loop_(loop),
      random_(0),
      tmp_dir_(kStoragePath),
      generator_(&random_),
      page_data_generator_(&random_),
      component_context_(std::move(component_context)),
      entry_count_(entry_count),
      key_size_(key_size),
      value_size_(value_size),
      get_inline_(get_inline) {
  FXL_DCHECK(loop_);
  FXL_DCHECK(entry_count_ > 0);
  FXL_DCHECK(key_size_ > 0);
  FXL_DCHECK(value_size_ > 0);
}

void GetEntryBenchmark::Run() {
  Status status =
      GetLedger(component_context_.get(), component_controller_.NewRequest(), nullptr, "",
                "get_entry", DetachedPath(tmp_dir_.path()), QuitLoopClosure(), &ledger_);
  if (QuitOnError(QuitLoopClosure(), status, "GetLedger")) {
    return;
  }

  GetPageEnsureInitialized(&ledger_, nullptr, DelayCallback::YES, QuitLoopClosure(),
                           [this](Status status, PagePtr page, PageId id) {
                             if (QuitOnError(QuitLoopClosure(), status, "Page initialization")) {
                               return;
                             }
                             page_ = std::move(page);
                             Populate();
                           });
}

void GetEntryBenchmark::Populate() {
  auto keys = generator_.MakeKeys(entry_count_, key_size_, entry_count_);

  page_data_generator_.Populate(
      &page_, std::move(keys), value_size_, entry_count_,
      PageDataGenerator::ReferenceStrategy::REFERENCE, Priority::EAGER, [this](Status status) {
        if (QuitOnError(QuitLoopClosure(), status, "PageGenerator::Populate")) {
          return;
        }
        GetSnapshot();
      });
}

void GetEntryBenchmark::GetSnapshot() {
  TRACE_ASYNC_BEGIN("benchmark", "get_snapshot", 0);
  page_->GetSnapshot(snapshot_.NewRequest(), {}, nullptr);
  page_->Sync([this] {
    TRACE_ASYNC_END("benchmark", "get_snapshot", 0);
    TRACE_ASYNC_BEGIN("benchmark", "get_keys", 0);
    GetKeys(nullptr);
  });
}

void GetEntryBenchmark::GetKeys(std::unique_ptr<Token> token) {
  snapshot_->GetKeys({}, std::move(token),
                     [this](auto keys, auto next_token) {
                       if (!next_token) {
                         TRACE_ASYNC_END("benchmark", "get_keys", 0);
                       }
                       for (size_t i = 0; i < keys.size(); i++) {
                         keys_.push_back(std::move(keys[i]));
                       }
                       if (next_token) {
                         GetKeys(std::move(next_token));
                         return;
                       }
                       if (get_inline_) {
                         GetNextEntryInline(0);
                       } else {
                         GetNextEntry(0);
                       }
                     });
}

void GetEntryBenchmark::GetNextEntry(size_t i) {
  if (i == entry_count_) {
    ShutDown();
    return;
  }

  TRACE_ASYNC_BEGIN("benchmark", "get_entry", i);
  snapshot_->Get(std::move(keys_[i]), [this, i](fuchsia::ledger::PageSnapshot_Get_Result result) {
    if (QuitOnError(QuitLoopClosure(), result, "PageShapshot::Get")) {
      return;
    }
    TRACE_ASYNC_END("benchmark", "get_entry", i);
    GetNextEntry(i + 1);
  });
}

void GetEntryBenchmark::GetNextEntryInline(size_t i) {
  if (i == entry_count_) {
    ShutDown();
    return;
  }

  TRACE_ASYNC_BEGIN("benchmark", "get_entry_inline", i);
  snapshot_->GetInline(std::move(keys_[i]),
                       [this, i](fuchsia::ledger::PageSnapshot_GetInline_Result result) {
                         if (QuitOnError(QuitLoopClosure(), result, "PageShapshot::GetInline")) {
                           return;
                         }
                         TRACE_ASYNC_END("benchmark", "get_entry_inline", i);
                         GetNextEntryInline(i + 1);
                       });
}

void GetEntryBenchmark::ShutDown() {
  KillLedgerProcess(&component_controller_);
  loop_->Quit();
}

fit::closure GetEntryBenchmark::QuitLoopClosure() {
  return [this] { loop_->Quit(); };
}

int Main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto component_context = sys::ComponentContext::Create();

  std::string entry_count_str;
  size_t entry_count;
  std::string key_size_str;
  size_t key_size;
  std::string value_size_str;
  size_t value_size;
  bool get_inline = command_line.HasOption(kInlineFlag.ToString());
  if (!command_line.GetOptionValue(kEntryCountFlag.ToString(), &entry_count_str) ||
      !fxl::StringToNumberWithError(entry_count_str, &entry_count) || entry_count == 0 ||
      !command_line.GetOptionValue(kKeySizeFlag.ToString(), &key_size_str) ||
      !fxl::StringToNumberWithError(key_size_str, &key_size) || key_size == 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(), &value_size_str) ||
      !fxl::StringToNumberWithError(value_size_str, &value_size) || value_size == 0) {
    PrintUsage();
    return -1;
  }

  GetEntryBenchmark app(&loop, std::move(component_context), entry_count, key_size, value_size,
                        get_inline);

  return RunWithTracing(&loop, [&app] { app.Run(); });
}

}  // namespace
}  // namespace ledger

int main(int argc, const char** argv) { return ledger::Main(argc, argv); }
