// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>

#include <fuchsia/ledger/internal/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/callback/capture.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/synchronous_interface_ptr.h>
#include <lib/fit/function.h>
#include <lib/fsl/io/fd.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/gtest/real_loop_fixture.h>
#include <lib/svc/cpp/services.h>
#include <lib/sys/cpp/component_context.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "peridot/lib/base64url/base64url.h"
#include "peridot/lib/convert/convert.h"
#include "peridot/lib/scoped_tmpfs/scoped_tmpfs.h"
#include "src/ledger/bin/app/serialization_version.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/filesystem/detached_path.h"
#include "src/ledger/bin/filesystem/directory_reader.h"
#include "src/ledger/bin/public/status.h"
#include "src/ledger/bin/testing/ledger_matcher.h"
#include "src/ledger/cloud_provider_in_memory/lib/fake_cloud_provider.h"
#include "src/ledger/cloud_provider_in_memory/lib/types.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/file.h"

namespace test {
namespace e2e_local {
namespace {

using ::testing::ElementsAre;

// Recursively searches for a directory with name |target_dir| and returns
// whether it was found. If found, |path_to_dir| is updated with the path from
// the source path.
bool FindPathToDir(const ledger::DetachedPath& root_path, fxl::StringView target_dir,
                   ledger::DetachedPath* path_to_dir) {
  bool dir_found = false;
  auto on_next_directory_entry = [&](fxl::StringView entry) {
    ledger::DetachedPath current_path = root_path.SubPath(entry);
    if (files::IsDirectoryAt(current_path.root_fd(), current_path.path())) {
      if (entry == target_dir) {
        dir_found = true;
        *path_to_dir = std::move(current_path);
        return false;
      }
      dir_found = FindPathToDir(current_path, target_dir, path_to_dir);
      // If the page path was found, stop the iteration by returning false.
      return !dir_found;
    }
    return true;
  };
  ledger::GetDirectoryEntries(root_path, on_next_directory_entry);
  return dir_found;
}

template <class A>
bool Equals(const fidl::VectorPtr<uint8_t>& a1, const A& a2) {
  if (a1->size() != a2.size())
    return false;
  return memcmp(a1->data(), a2.data(), a1->size()) == 0;
}

std::vector<uint8_t> TestArray() {
  std::string value = "value";
  std::vector<uint8_t> result(value.size());
  memcpy(&result.at(0), &value[0], value.size());
  return result;
}

class LedgerEndToEndTest : public gtest::RealLoopFixture {
 public:
  LedgerEndToEndTest() : component_context_(sys::ComponentContext::Create()) {
    component_context()->svc()->Connect(launcher_.NewRequest());
  }
  ~LedgerEndToEndTest() override {}

 protected:
  void Init(std::vector<std::string> additional_args) {
    component::Services child_services;
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = "fuchsia-pkg://fuchsia.com/ledger#meta/ledger.cmx";
    launch_info.directory_request = child_services.NewRequest();
    launch_info.arguments.emplace({"--disable_reporting"});
    for (auto& additional_arg : additional_args) {
      launch_info.arguments->push_back(additional_arg);
    }
    launcher_->CreateComponent(std::move(launch_info), ledger_controller_.NewRequest());

    ledger_controller_.set_error_handler([this](zx_status_t status) {
      for (const auto& callback : ledger_shutdown_callbacks_) {
        callback();
      }
    });

    ledger_repository_factory_.set_error_handler([](zx_status_t status) {
      if (status != ZX_ERR_PEER_CLOSED) {
        ADD_FAILURE() << "Ledger repository error: " << status;
      }
    });
    child_services.ConnectToService(ledger_repository_factory_.NewRequest());
    child_services.ConnectToService(controller_.NewRequest());
  }

  void RegisterShutdownCallback(fit::function<void()> callback) {
    ledger_shutdown_callbacks_.push_back(std::move(callback));
  }

  sys::ComponentContext* component_context() { return component_context_.get(); }

 private:
  fuchsia::sys::ComponentControllerPtr ledger_controller_;
  std::vector<fit::function<void()>> ledger_shutdown_callbacks_;
  std::unique_ptr<sys::ComponentContext> component_context_;
  fuchsia::sys::LauncherPtr launcher_;

 protected:
  ledger_internal::LedgerRepositoryFactoryPtr ledger_repository_factory_;
  fidl::SynchronousInterfacePtr<ledger::Ledger> ledger_;
  fidl::SynchronousInterfacePtr<ledger_internal::LedgerController> controller_;
};

TEST_F(LedgerEndToEndTest, PutAndGet) {
  Init({});
  fidl::SynchronousInterfacePtr<ledger_internal::LedgerRepository> ledger_repository;
  scoped_tmpfs::ScopedTmpFS tmpfs;
  ledger_repository_factory_->GetRepository(fsl::CloneChannelFromFileDescriptor(tmpfs.root_fd()),
                                            nullptr, "", ledger_repository.NewRequest());

  ledger_repository->GetLedger(TestArray(), ledger_.NewRequest());
  ASSERT_EQ(ledger_repository->Sync(), ZX_OK);

  fidl::SynchronousInterfacePtr<ledger::Page> page;
  ledger_->GetRootPage(page.NewRequest());
  page->Put(TestArray(), TestArray());
  fidl::SynchronousInterfacePtr<ledger::PageSnapshot> snapshot;
  page->GetSnapshot(snapshot.NewRequest(), {}, nullptr);
  fuchsia::ledger::PageSnapshot_Get_Result result;
  EXPECT_EQ(snapshot->Get(TestArray(), &result), ZX_OK);
  EXPECT_THAT(result, ledger::MatchesString(convert::ToString(TestArray())));
}

TEST_F(LedgerEndToEndTest, Terminate) {
  Init({});
  bool called = false;
  RegisterShutdownCallback([this, &called] {
    called = true;
    QuitLoop();
  });
  controller_->Terminate();
  RunLoop();
  EXPECT_TRUE(called);
}

TEST_F(LedgerEndToEndTest, ClearPage) {
  Init({});
  fidl::SynchronousInterfacePtr<ledger_internal::LedgerRepository> ledger_repository;
  scoped_tmpfs::ScopedTmpFS tmpfs;
  ledger_repository_factory_->GetRepository(fsl::CloneChannelFromFileDescriptor(tmpfs.root_fd()),
                                            nullptr, "", ledger_repository.NewRequest());

  ledger_repository->GetLedger(TestArray(), ledger_.NewRequest());
  ASSERT_EQ(ledger_repository->Sync(), ZX_OK);

  int page_count = 5;

  std::vector<ledger::DetachedPath> page_paths;
  page_paths.reserve(page_count);

  // Create 5 pages, add contents and clear them.
  for (int i = 0; i < page_count; ++i) {
    fidl::SynchronousInterfacePtr<ledger::Page> page;
    ledger_->GetPage(nullptr, page.NewRequest());
    ASSERT_EQ(ZX_OK, ledger_->Sync());

    // Check that the directory has been created.
    ledger::PageId page_id;
    page->GetId(&page_id);

    // The following is assuming that the page's folder is using the name
    // <base64(page_id)>.
    std::string page_dir_name = base64url::Base64UrlEncode(convert::ExtendedStringView(page_id.id));
    ledger::DetachedPath page_path;
    ASSERT_TRUE(FindPathToDir(ledger::DetachedPath(tmpfs.root_fd()), page_dir_name, &page_path))
        << "Failed to find page's directory. Expected to find directory named "
           "`base64(page_id)`: "
        << page_dir_name;
    page_paths.push_back(std::move(page_path));

    // Insert an entry.
    page->Put(TestArray(), TestArray());

    // Clear the page and close it.
    page->Clear();
    page.Unbind();
  }

  // Make sure all directories have been deleted.
  for (const ledger::DetachedPath& path : page_paths) {
    RunLoopUntil([&] { return !files::IsDirectoryAt(path.root_fd(), path.path()); });
    EXPECT_FALSE(files::IsDirectoryAt(tmpfs.root_fd(), path.path()));
  }
}

// Verifies the cloud erase recovery in case of a cloud that was erased before
// startup.
//
// Expected behavior: Ledger disconnects the clients and the local state is
// cleared.
TEST_F(LedgerEndToEndTest, CloudEraseRecoveryOnInitialCheck) {
  Init({});
  bool ledger_shut_down = false;
  RegisterShutdownCallback([&ledger_shut_down] { ledger_shut_down = true; });

  ledger_internal::LedgerRepositoryPtr ledger_repository;
  scoped_tmpfs::ScopedTmpFS tmpfs;
  const std::string content_path = ledger::kSerializationVersion.ToString();
  const std::string deletion_sentinel_path = content_path + "/sentinel";
  ASSERT_TRUE(files::CreateDirectoryAt(tmpfs.root_fd(), content_path));
  ASSERT_TRUE(files::WriteFileAt(tmpfs.root_fd(), deletion_sentinel_path, "", 0));
  ASSERT_TRUE(files::IsFileAt(tmpfs.root_fd(), deletion_sentinel_path));

  // Write a fingerprint file, so that Ledger will check if it is still in the
  // cloud device set.
  const std::string fingerprint_path = content_path + "/fingerprint";
  const std::string fingerprint = "bazinga";
  ASSERT_TRUE(files::WriteFileAt(tmpfs.root_fd(), fingerprint_path, fingerprint.c_str(),
                                 fingerprint.size()));

  // Create a cloud provider configured to trigger the cloude erase recovery on
  // initial check.
  auto cloud_provider = ledger::FakeCloudProvider::Builder()
                            .SetCloudEraseOnCheck(ledger::CloudEraseOnCheck::YES)
                            .Build();
  cloud_provider::CloudProviderPtr cloud_provider_ptr;
  fidl::Binding<cloud_provider::CloudProvider> cloud_provider_binding(
      cloud_provider.get(), cloud_provider_ptr.NewRequest());

  ledger_repository_factory_->GetRepository(fsl::CloneChannelFromFileDescriptor(tmpfs.root_fd()),
                                            std::move(cloud_provider_ptr), "user_id",
                                            ledger_repository.NewRequest());

  bool repo_disconnected = false;
  ledger_repository.set_error_handler(
      [&repo_disconnected](zx_status_t status) { repo_disconnected = true; });

  // Run the message loop until Ledger clears the repo directory and disconnects
  // the client.
  RunLoopUntil([&] {
    return !files::IsFileAt(tmpfs.root_fd(), deletion_sentinel_path) && repo_disconnected;
  });
  EXPECT_FALSE(files::IsFileAt(tmpfs.root_fd(), deletion_sentinel_path));
  EXPECT_TRUE(repo_disconnected);

  // Make sure all the contents are deleted. Only the staging directory should
  // be present.
  std::vector<std::string> directory_entries;
  auto on_next_directory_entry = [&](fxl::StringView entry) {
    directory_entries.push_back(entry.ToString());
    return true;
  };
  EXPECT_TRUE(
      ledger::GetDirectoryEntries(ledger::DetachedPath(tmpfs.root_fd()), on_next_directory_entry));
  EXPECT_THAT(directory_entries, ElementsAre("staging"));

  // Verify that the Ledger app didn't crash.
  EXPECT_FALSE(ledger_shut_down);
}

// Verifies the cloud erase recovery in case of a cloud that is erased while
// Ledger is connected to it.
//
// Expected behavior: Ledger disconnects the clients and the local state is
// cleared.
TEST_F(LedgerEndToEndTest, CloudEraseRecoveryFromTheWatcher) {
  Init({});
  bool ledger_shut_down = false;
  RegisterShutdownCallback([&ledger_shut_down] { ledger_shut_down = true; });

  ledger_internal::LedgerRepositoryPtr ledger_repository;
  scoped_tmpfs::ScopedTmpFS tmpfs;
  const std::string content_path = ledger::kSerializationVersion.ToString();
  const std::string deletion_sentinel_path = content_path + "/sentinel";
  ASSERT_TRUE(files::CreateDirectoryAt(tmpfs.root_fd(), content_path));
  ASSERT_TRUE(files::WriteFileAt(tmpfs.root_fd(), deletion_sentinel_path, "", 0));
  ASSERT_TRUE(files::IsFileAt(tmpfs.root_fd(), deletion_sentinel_path));

  // Create a cloud provider configured to trigger the cloud erase recovery
  // while Ledger is connected.
  auto cloud_provider = ledger::FakeCloudProvider::Builder()
                            .SetCloudEraseFromWatcher(ledger::CloudEraseFromWatcher::YES)
                            .Build();
  cloud_provider::CloudProviderPtr cloud_provider_ptr;
  fidl::Binding<cloud_provider::CloudProvider> cloud_provider_binding(
      cloud_provider.get(), cloud_provider_ptr.NewRequest());

  ledger_repository_factory_->GetRepository(fsl::CloneChannelFromFileDescriptor(tmpfs.root_fd()),
                                            std::move(cloud_provider_ptr), "user_id",
                                            ledger_repository.NewRequest());

  bool repo_disconnected = false;
  ledger_repository.set_error_handler(
      [&repo_disconnected](zx_status_t status) { repo_disconnected = true; });

  // Run the message loop until Ledger clears the repo directory and disconnects
  // the client.
  RunLoopUntil([&] {
    return !files::IsFileAt(tmpfs.root_fd(), deletion_sentinel_path) && repo_disconnected;
  });
  EXPECT_FALSE(files::IsFileAt(tmpfs.root_fd(), deletion_sentinel_path));
  EXPECT_TRUE(repo_disconnected);

  // Verify that the Ledger app didn't crash.
  EXPECT_FALSE(ledger_shut_down);
}

// Verifies that Ledger instance continues to work even if the cloud provider
// goes away (for example, because it crashes).
//
// In the future, we need to also be able to reconnect/request a new cloud
// provider, see LE-567.
TEST_F(LedgerEndToEndTest, HandleCloudProviderDisconnectBeforePageInit) {
  Init({});
  bool ledger_app_shut_down = false;
  RegisterShutdownCallback([&ledger_app_shut_down] { ledger_app_shut_down = true; });
  scoped_tmpfs::ScopedTmpFS tmpfs;

  cloud_provider::CloudProviderPtr cloud_provider_ptr;
  fidl::SynchronousInterfacePtr<ledger_internal::LedgerRepository> ledger_repository;
  ledger::FakeCloudProvider cloud_provider;
  fidl::Binding<cloud_provider::CloudProvider> cloud_provider_binding(
      &cloud_provider, cloud_provider_ptr.NewRequest());
  ledger_repository_factory_->GetRepository(fsl::CloneChannelFromFileDescriptor(tmpfs.root_fd()),
                                            std::move(cloud_provider_ptr), "user_id",
                                            ledger_repository.NewRequest());

  ledger_repository->GetLedger(TestArray(), ledger_.NewRequest());
  ASSERT_EQ(ledger_repository->Sync(), ZX_OK);

  // Close the cloud provider channel.
  cloud_provider_binding.Unbind();

  // Write and read some data to verify that Ledger still works.
  fidl::SynchronousInterfacePtr<ledger::Page> page;
  ledger_->GetPage(nullptr, page.NewRequest());
  page->Put(TestArray(), TestArray());
  fidl::SynchronousInterfacePtr<ledger::PageSnapshot> snapshot;
  page->GetSnapshot(snapshot.NewRequest(), {}, nullptr);
  fuchsia::mem::BufferPtr value;
  fuchsia::ledger::PageSnapshot_Get_Result result;
  EXPECT_EQ(snapshot->Get(TestArray(), &result), ZX_OK);
  EXPECT_THAT(result, ledger::MatchesString(convert::ToString(TestArray())));

  // Verify that the Ledger app didn't crash or shut down.
  EXPECT_TRUE(ledger_repository);
  EXPECT_FALSE(ledger_app_shut_down);
}

TEST_F(LedgerEndToEndTest, HandleCloudProviderDisconnectBetweenReadAndWrite) {
  Init({});
  bool ledger_app_shut_down = false;
  RegisterShutdownCallback([&ledger_app_shut_down] { ledger_app_shut_down = true; });
  ledger::Status status;
  scoped_tmpfs::ScopedTmpFS tmpfs;

  cloud_provider::CloudProviderPtr cloud_provider_ptr;
  fidl::SynchronousInterfacePtr<ledger_internal::LedgerRepository> ledger_repository;
  ledger::FakeCloudProvider cloud_provider;
  fidl::Binding<cloud_provider::CloudProvider> cloud_provider_binding(
      &cloud_provider, cloud_provider_ptr.NewRequest());
  ledger_repository_factory_->GetRepository(fsl::CloneChannelFromFileDescriptor(tmpfs.root_fd()),
                                            std::move(cloud_provider_ptr), "user_id",
                                            ledger_repository.NewRequest());

  ledger_repository->GetLedger(TestArray(), ledger_.NewRequest());
  ASSERT_EQ(ledger_repository->Sync(), ZX_OK);

  // Write some data.
  fidl::SynchronousInterfacePtr<ledger::Page> page;
  ledger_->GetPage(nullptr, page.NewRequest());
  status = ledger::Status::INTERNAL_ERROR;
  page->Put(TestArray(), TestArray());

  // Close the cloud provider channel.
  cloud_provider_binding.Unbind();

  // Read the data back.
  fidl::SynchronousInterfacePtr<ledger::PageSnapshot> snapshot;
  status = ledger::Status::INTERNAL_ERROR;
  page->GetSnapshot(snapshot.NewRequest(), {}, nullptr);
  fuchsia::mem::BufferPtr value;
  fuchsia::ledger::PageSnapshot_Get_Result result;
  EXPECT_EQ(snapshot->Get(TestArray(), &result), ZX_OK);
  EXPECT_THAT(result, ledger::MatchesString(convert::ToString(TestArray())));

  // Verify that the Ledger app didn't crash or shut down.
  EXPECT_TRUE(ledger_repository);
  EXPECT_FALSE(ledger_app_shut_down);
}

}  // namespace
}  // namespace e2e_local
}  // namespace test
