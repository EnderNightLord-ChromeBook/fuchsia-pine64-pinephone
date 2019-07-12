// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/callback/waiter.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/time.h>
#include <trace/event.h>

#include <iostream>
#include <memory>

#include "garnet/public/lib/callback/waiter.h"
#include "peridot/lib/rng/test_random.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/filesystem/get_directory_content_size.h"
#include "src/ledger/bin/testing/get_ledger.h"
#include "src/ledger/bin/testing/get_page_ensure_initialized.h"
#include "src/ledger/bin/testing/page_data_generator.h"
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
    "fuchsia-pkg://fuchsia.com/ledger_benchmarks#meta/disk_space.cmx";
constexpr fxl::StringView kStoragePath = "/data/benchmark/ledger/disk_space";
constexpr fxl::StringView kPageCountFlag = "page-count";
constexpr fxl::StringView kUniqueKeyCountFlag = "unique-key-count";
constexpr fxl::StringView kCommitCountFlag = "commit-count";
constexpr fxl::StringView kKeySizeFlag = "key-size";
constexpr fxl::StringView kValueSizeFlag = "value-size";

void PrintUsage() {
  std::cout << "Usage: trace record "
            << kBinaryPath
            // Comment to make clang format not break formatting.
            << " --" << kPageCountFlag << "=<int>"
            << " --" << kUniqueKeyCountFlag << "=<int>"
            << " --" << kCommitCountFlag << "=<int>"
            << " --" << kKeySizeFlag << "=<int>"
            << " --" << kValueSizeFlag << "=<int>" << std::endl;
}

// Disk space "general usage" benchmark.
// This benchmark is used to capture Ledger disk usage over the set of common
// operations, such as getting a new page, adding several entries to the page,
// modifying the same entry several times.
//
// The emulated scenario is as follows:
// First, |page_count| pages is requested from ledger. Then each page is
// populated with |unique_key_count| unique entries, making |commit_count|
// commits in the process (so if |commit_count| is bigger than
// |unique_key_count|, some entries get overwritten in subsequent commits,
// whereas if |commit_count| is smaller than |unique_key_count|, insertion
// operations get grouped together into the requested number of commits). Each
// entry has a key size of |key_size| and a value size of |value_size|. After
// that, the connection to the ledger is closed and the size of the directory
// used by it is measured and reported using a trace counter event.
//
// Parameters:
//   --page-count=<int> number of pages to be requested.
//   --unique-key-count=<int> number of unique keys contained in each page
//   after population.
//   --commit-count=<int> number of commits made to each page.
//   If this number is smaller than unique-key-count, changes will be bundled
//   into transactions. If it is bigger, some or all of the changes will use the
//   same keys, modifying the value.
//   --key-size=<int> size of a key for each entry.
//   --value-size=<int> size of a value for each entry.
class DiskSpaceBenchmark {
 public:
  DiskSpaceBenchmark(async::Loop* loop, std::unique_ptr<sys::ComponentContext> component_context,
                     size_t page_count, size_t unique_key_count, size_t commit_count,
                     size_t key_size, size_t value_size);

  void Run();

 private:
  void Populate();
  void ShutDownAndRecord();
  fit::closure QuitLoopClosure();

  async::Loop* const loop_;
  rng::TestRandom random_;
  files::ScopedTempDir tmp_dir_;
  DataGenerator generator_;
  PageDataGenerator page_data_generator_;
  std::unique_ptr<sys::ComponentContext> component_context_;
  const size_t page_count_;
  const size_t unique_key_count_;
  const size_t commit_count_;
  const size_t key_size_;
  const size_t value_size_;
  fuchsia::sys::ComponentControllerPtr component_controller_;
  LedgerPtr ledger_;
  std::vector<PagePtr> pages_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DiskSpaceBenchmark);
};

DiskSpaceBenchmark::DiskSpaceBenchmark(async::Loop* loop,
                                       std::unique_ptr<sys::ComponentContext> component_context,
                                       size_t page_count, size_t unique_key_count,
                                       size_t commit_count, size_t key_size, size_t value_size)
    : loop_(loop),
      random_(0),
      tmp_dir_(kStoragePath),
      generator_(&random_),
      page_data_generator_(&random_),
      component_context_(std::move(component_context)),
      page_count_(page_count),
      unique_key_count_(unique_key_count),
      commit_count_(commit_count),
      key_size_(key_size),
      value_size_(value_size) {
  FXL_DCHECK(loop_);
  FXL_DCHECK(page_count_ >= 0);
  FXL_DCHECK(unique_key_count_ >= 0);
  FXL_DCHECK(commit_count_ >= 0);
  FXL_DCHECK(key_size_ > 0);
  FXL_DCHECK(value_size_ > 0);
}

void DiskSpaceBenchmark::Run() {
  Status status =
      GetLedger(component_context_.get(), component_controller_.NewRequest(), nullptr, "",
                "disk_space", DetachedPath(tmp_dir_.path()), QuitLoopClosure(), &ledger_);
  if (QuitOnError(QuitLoopClosure(), status, "GetLedger")) {
    return;
  }

  auto waiter = fxl::MakeRefCounted<callback::Waiter<Status, PagePtr>>(Status::OK);

  for (size_t page_number = 0; page_number < page_count_; page_number++) {
    GetPageEnsureInitialized(
        &ledger_, nullptr, DelayCallback::YES, QuitLoopClosure(),
        [callback = waiter->NewCallback()](Status status, PagePtr page, PageId id) {
          callback(status, std::move(page));
        });
  }

  waiter->Finalize([this](Status status, std::vector<PagePtr> pages) {
    if (QuitOnError(QuitLoopClosure(), status, "GetPageEnsureInitialized")) {
      return;
    }
    pages_ = std::move(pages);
    if (commit_count_ == 0) {
      ShutDownAndRecord();
      return;
    }
    Populate();
  });
}

void DiskSpaceBenchmark::Populate() {
  int transaction_size =
      static_cast<int>(ceil(static_cast<double>(unique_key_count_) / commit_count_));
  int insertions = std::max(unique_key_count_, commit_count_);
  FXL_LOG(INFO) << "Transaction size: " << transaction_size << ", insertions: " << insertions
                << ".";
  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  for (auto& page : pages_) {
    auto keys = generator_.MakeKeys(insertions, key_size_, unique_key_count_);
    page_data_generator_.Populate(&page, std::move(keys), value_size_, transaction_size,
                                  PageDataGenerator::ReferenceStrategy::REFERENCE, Priority::EAGER,
                                  waiter->NewCallback());
  }
  waiter->Finalize([this](Status status) {
    if (QuitOnError(QuitLoopClosure(), status, "PageGenerator::Populate")) {
      return;
    }
    ShutDownAndRecord();
  });
}

void DiskSpaceBenchmark::ShutDownAndRecord() {
  KillLedgerProcess(&component_controller_);
  loop_->Quit();

  uint64_t tmp_dir_size = 0;
  FXL_CHECK(GetDirectoryContentSize(DetachedPath(tmp_dir_.path()), &tmp_dir_size));
  TRACE_COUNTER("benchmark", "ledger_directory_size", 0, "directory_size", TA_UINT64(tmp_dir_size));
}

fit::closure DiskSpaceBenchmark::QuitLoopClosure() {
  return [this] { loop_->Quit(); };
}

int Main(int argc, const char** argv) {
  fxl::CommandLine command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto component_context = sys::ComponentContext::Create();

  std::string page_count_str;
  size_t page_count;
  std::string unique_key_count_str;
  size_t unique_key_count;
  std::string commit_count_str;
  size_t commit_count;
  std::string key_size_str;
  size_t key_size;
  std::string value_size_str;
  size_t value_size;
  if (!command_line.GetOptionValue(kPageCountFlag.ToString(), &page_count_str) ||
      !fxl::StringToNumberWithError(page_count_str, &page_count) ||
      !command_line.GetOptionValue(kUniqueKeyCountFlag.ToString(), &unique_key_count_str) ||
      !fxl::StringToNumberWithError(unique_key_count_str, &unique_key_count) ||
      !command_line.GetOptionValue(kCommitCountFlag.ToString(), &commit_count_str) ||
      !fxl::StringToNumberWithError(commit_count_str, &commit_count) ||
      !command_line.GetOptionValue(kKeySizeFlag.ToString(), &key_size_str) ||
      !fxl::StringToNumberWithError(key_size_str, &key_size) || key_size == 0 ||
      !command_line.GetOptionValue(kValueSizeFlag.ToString(), &value_size_str) ||
      !fxl::StringToNumberWithError(value_size_str, &value_size) || value_size == 0) {
    PrintUsage();
    return -1;
  }

  DiskSpaceBenchmark app(&loop, std::move(component_context), page_count, unique_key_count,
                         commit_count, key_size, value_size);

  return RunWithTracing(&loop, [&app] { app.Run(); });
}

}  // namespace
}  // namespace ledger

int main(int argc, const char** argv) { return ledger::Main(argc, argv); }
