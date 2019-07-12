// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/clone.h>
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
#include "src/ledger/bin/testing/quit_on_error.h"
#include "src/ledger/bin/testing/run_with_tracing.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/strings/string_number_conversions.h"

namespace ledger {
namespace {

constexpr fxl::StringView kBinaryPath =
    "fuchsia-pkg://fuchsia.com/ledger_benchmarks#meta/update_entry.cmx";
constexpr fxl::StringView kStoragePath = "/data/benchmark/ledger/update_entry";
constexpr fxl::StringView kEntryCountFlag = "entry-count";
constexpr fxl::StringView kValueSizeFlag = "value-size";
constexpr fxl::StringView kTransactionSizeFlag = "transaction-size";

const int kKeySize = 100;

void PrintUsage() {
  std::cout << "Usage: trace record "
            << kBinaryPath
            // Comment to make clang format not break formatting.
            << " --" << kEntryCountFlag << "=<int>"
            << " --" << kValueSizeFlag << "=<int>"
            << " --" << kTransactionSizeFlag << "=<int>" << std::endl;
}

// Benchmark that measures a performance of Put() operation under the condition
// that it modifies the same entry.
//
// Parameters:
//   --entry-count=<int> the number of entries to be put
//   --value-size=<int> the size of the value for each entry
//   --transaction-size=<int> the size of a single transaction in number of put
//     operations. If equal to 0, every put operation will be executed
//     individually (implicit transaction).
class UpdateEntryBenchmark {
 public:
  UpdateEntryBenchmark(async::Loop* loop, std::unique_ptr<sys::ComponentContext> component_context,
                       int entry_count, int value_size, int transaction_size);

  void Run();

 private:
  void RunSingle(int i, std::vector<uint8_t> key);
  void CommitAndRunNext(int i, std::vector<uint8_t> key);

  void ShutDown();
  fit::closure QuitLoopClosure();

  async::Loop* const loop_;
  rng::TestRandom random_;
  DataGenerator generator_;

  files::ScopedTempDir tmp_dir_;
  std::unique_ptr<sys::ComponentContext> component_context_;
  const int entry_count_;
  const int transaction_size_;
  const int key_size_;
  const int value_size_;

  fuchsia::sys::ComponentControllerPtr component_controller_;
  LedgerPtr ledger_;
  PagePtr page_;

  FXL_DISALLOW_COPY_AND_ASSIGN(UpdateEntryBenchmark);
};

UpdateEntryBenchmark::UpdateEntryBenchmark(async::Loop* loop,
                                           std::unique_ptr<sys::ComponentContext> component_context,
                                           int entry_count, int value_size, int transaction_size)
    : loop_(loop),
      random_(0),
      generator_(&random_),
      tmp_dir_(kStoragePath),
      component_context_(std::move(component_context)),
      entry_count_(entry_count),
      transaction_size_(transaction_size),
      key_size_(kKeySize),
      value_size_(value_size) {
  FXL_DCHECK(loop_);
  FXL_DCHECK(entry_count_ > 0);
  FXL_DCHECK(value_size_ > 0);
  FXL_DCHECK(key_size_ > 0);
  FXL_DCHECK(transaction_size_ >= 0);
}

void UpdateEntryBenchmark::Run() {
  FXL_LOG(INFO) << "--entry-count=" << entry_count_ << " --transaction-size=" << transaction_size_;
  Status status =
      GetLedger(component_context_.get(), component_controller_.NewRequest(), nullptr, "",
                "update_entry", DetachedPath(tmp_dir_.path()), QuitLoopClosure(), &ledger_);
  if (QuitOnError(QuitLoopClosure(), status, "GetLedger")) {
    return;
  }
  GetPageEnsureInitialized(
      &ledger_, nullptr, DelayCallback::YES, QuitLoopClosure(),
      [this](Status status, PagePtr page, PageId id) {
        if (QuitOnError(QuitLoopClosure(), status, "GetPageEnsureInitialized")) {
          return;
        }
        page_ = std::move(page);
        std::vector<uint8_t> key = generator_.MakeKey(0, key_size_);
        if (transaction_size_ > 0) {
          page_->StartTransaction();
          page_->Sync([this, key = std::move(key)]() mutable {
            TRACE_ASYNC_BEGIN("benchmark", "transaction", 0);
            RunSingle(0, std::move(key));
          });
        } else {
          RunSingle(0, std::move(key));
        }
      });
}

void UpdateEntryBenchmark::RunSingle(int i, std::vector<uint8_t> key) {
  if (i == entry_count_) {
    ShutDown();
    return;
  }

  std::vector<uint8_t> value = generator_.MakeValue(value_size_);
  TRACE_ASYNC_BEGIN("benchmark", "put", i);
  page_->Put(key, std::move(value));
  page_->Sync([this, i, key = std::move(key)]() mutable {
    TRACE_ASYNC_END("benchmark", "put", i);
    if (transaction_size_ > 0 &&
        (i % transaction_size_ == transaction_size_ - 1 || i + 1 == entry_count_)) {
      CommitAndRunNext(i, std::move(key));
    } else {
      RunSingle(i + 1, std::move(key));
    }
  });
}

void UpdateEntryBenchmark::CommitAndRunNext(int i, std::vector<uint8_t> key) {
  TRACE_ASYNC_BEGIN("benchmark", "commit", i / transaction_size_);
  page_->Commit();
  page_->Sync([this, i, key = std::move(key)]() mutable {
    TRACE_ASYNC_END("benchmark", "commit", i / transaction_size_);
    TRACE_ASYNC_END("benchmark", "transaction", i / transaction_size_);

    if (i == entry_count_ - 1) {
      RunSingle(i + 1, std::move(key));
      return;
    }
    page_->StartTransaction();
    page_->Sync([this, i = i + 1, key = std::move(key)]() mutable {
      TRACE_ASYNC_BEGIN("benchmark", "transaction", i / transaction_size_);
      RunSingle(i, std::move(key));
    });
  });
}

void UpdateEntryBenchmark::ShutDown() {
  // Shut down the Ledger process first as it relies on |tmp_dir_| storage.
  KillLedgerProcess(&component_controller_);
  loop_->Quit();
}

fit::closure UpdateEntryBenchmark::QuitLoopClosure() {
  return [this] { loop_->Quit(); };
}

int Main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto component_context = sys::ComponentContext::Create();

  std::string entry_count_str;
  size_t entry_count;
  std::string value_size_str;
  int value_size;
  std::string transaction_size_str;
  int transaction_size;
  if (!command_line.GetOptionValue(kEntryCountFlag.ToString(), &entry_count_str) ||
      !fxl::StringToNumberWithError(entry_count_str, &entry_count) || entry_count <= 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(), &value_size_str) ||
      !fxl::StringToNumberWithError(value_size_str, &value_size) || value_size <= 0 ||
      !command_line.GetOptionValue(kTransactionSizeFlag.ToString(), &transaction_size_str) ||
      !fxl::StringToNumberWithError(transaction_size_str, &transaction_size) ||
      transaction_size < 0) {
    PrintUsage();
    return -1;
  }

  UpdateEntryBenchmark app(&loop, std::move(component_context), entry_count, value_size,
                           transaction_size);
  return RunWithTracing(&loop, [&app] { app.Run(); });
}

}  // namespace
}  // namespace ledger

int main(int argc, const char** argv) { return ledger::Main(argc, argv); }
