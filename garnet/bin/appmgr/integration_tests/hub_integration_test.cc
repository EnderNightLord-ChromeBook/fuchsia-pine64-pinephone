// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async/default.h>
#include <lib/async_promise/executor.h>
#include <lib/fdio/directory.h>
#include <lib/inspect_deprecated/query/discover.h>
#include <lib/inspect_deprecated/query/read.h>
#include <lib/inspect_deprecated/testing/inspect.h>
#include <lib/sys/cpp/file_descriptor.h>
#include <lib/sys/cpp/testing/test_with_environment.h>
#include <src/lib/fxl/strings/concatenate.h>
#include <src/lib/fxl/strings/join_strings.h>
#include <src/lib/fxl/strings/string_printf.h>

#include "garnet/bin/sysmgr/config.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lib/inspect_deprecated/query/location.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/file.h"
#include "src/lib/files/glob.h"

using namespace inspect_deprecated::testing;

namespace component {
namespace {

// This test fixture will provide a way to run components in provided launchers
// and check for errors.
class HubTest : public sys::testing::TestWithEnvironment {
 protected:
  // This would launch component and check that it returns correct
  // |expected_return_code|.
  void RunComponent(const fuchsia::sys::LauncherPtr& launcher, const std::string& component_url,
                    const std::vector<std::string>& args, int64_t expected_return_code) {
    std::FILE* outf = std::tmpfile();
    int out_fd = fileno(outf);
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = component_url;
    launch_info.arguments = args;

    launch_info.out = sys::CloneFileDescriptor(out_fd);

    fuchsia::sys::ComponentControllerPtr controller;
    launcher->CreateComponent(std::move(launch_info), controller.NewRequest());

    int64_t return_code = INT64_MIN;
    controller.events().OnTerminated = [&return_code](int64_t code,
                                                      fuchsia::sys::TerminationReason reason) {
      return_code = code;
    };
    RunLoopUntil([&return_code] { return return_code != INT64_MIN; });
    std::string output;
    ASSERT_TRUE(files::ReadFileDescriptorToString(out_fd, &output));
    EXPECT_EQ(expected_return_code, return_code)
        << "failed for: " << fxl::JoinStrings(args, ", ") << "\noutput: " << output;
  }
};

TEST(ProbeHub, Component) {
  constexpr char kGlob[] = "/hub/c/*/*/out/debug";
  files::Glob glob(kGlob);
  EXPECT_GE(glob.size(), 1u) << kGlob << " expected to match at least once.";
}

TEST(ProbeHub, Realm) {
  constexpr char kGlob[] = "/hub/c/";
  files::Glob glob(kGlob);
  EXPECT_EQ(glob.size(), 1u) << kGlob << " expected to match once.";
}

TEST(ProbeHub, RealmSvc) {
  constexpr char kGlob[] = "/hub/svc/fuchsia.sys.Environment";
  files::Glob glob(kGlob);
  EXPECT_EQ(glob.size(), 1u);
}

TEST_F(HubTest, Services) {
  // Services for sys.
  {
    constexpr char kGlob[] = "/hub/svc";
    files::Glob glob(kGlob);
    EXPECT_EQ(glob.size(), 1u) << kGlob << " expected to match once.";
    const std::string path = *glob.begin();

    // Expected files are built-in services plus sysmgr services.
    std::vector<std::string> expected_files = {".",
                                               "fuchsia.boot.FactoryItems",
                                               "fuchsia.boot.RootJob",
                                               "fuchsia.boot.RootResource",
                                               "fuchsia.device.NameProvider",
                                               "fuchsia.device.manager.Administrator",
                                               "fuchsia.device.manager.DebugDumper",
                                               "fuchsia.paver.Paver",
                                               "fuchsia.kernel.Counter",
                                               "fuchsia.kernel.DebugBroker",
                                               "fuchsia.process.Launcher",
                                               "fuchsia.process.Resolver",
                                               "fuchsia.scheduler.ProfileProvider",
                                               "fuchsia.sys.Environment",
                                               "fuchsia.sys.Launcher",
                                               "fuchsia.sys.Loader",
                                               "fuchsia.sys.test.CacheControl",
                                               "fuchsia.virtualconsole.SessionManager"};
    sysmgr::Config config;
    ASSERT_TRUE(config.ParseFromDirectory("/system/data/sysmgr"));
    // The following path is deprecated, and because config-data is component
    // name isolated, it will be impossible to continue to do this in future:
    ASSERT_TRUE(config.ParseFromDirectory("/pkgfs/packages/config-data/0/data/sysmgr"));
    const auto service_map = config.TakeServices();
    for (const auto& e : service_map) {
      expected_files.push_back(e.first);
    }

    // readdir should list all services.
    std::vector<std::string> files;
    ASSERT_TRUE(files::ReadDirContents(path, &files));
    EXPECT_THAT(files, ::testing::UnorderedElementsAreArray(expected_files));

    // Try looking up an individual service.
    const std::string service_path = fxl::Concatenate({path, "/", service_map.begin()->first});
    EXPECT_TRUE(files::IsFile(service_path)) << service_path;
    const std::string bogus_path = fxl::Concatenate({path, "/does_not_exist"});
    EXPECT_FALSE(files::IsFile(bogus_path)) << bogus_path;
  }
}

TEST_F(HubTest, ScopePolicy) {
  constexpr char kGlobUrl[] = "fuchsia-pkg://fuchsia.com/glob#meta/glob.cmx";
  // create nested environment
  // test that we can see nested env
  auto nested_env = CreateNewEnclosingEnvironment("hubscopepolicytest", CreateServices());
  WaitForEnclosingEnvToStart(nested_env.get());
  RunComponent(launcher_ptr(), kGlobUrl, {"/hub/r/hubscopepolicytest/"}, 0);

  // test that we cannot see nested env using its own launcher
  RunComponent(nested_env->launcher_ptr(), kGlobUrl, {"/hub/r/hubscopepolicytest"}, 1);

  // test that we can see check_hub_path
  RunComponent(nested_env->launcher_ptr(), kGlobUrl, {"/hub/c/glob.cmx"}, 0);
}

TEST_F(HubTest, SystemObjects) {
  std::string glob_url = "fuchsia-pkg://fuchsia.com/glob#meta/glob.cmx";

  auto nested_env = CreateNewEnclosingEnvironment("hubscopepolicytest", CreateServices());
  WaitForEnclosingEnvToStart(nested_env.get());
  RunComponent(launcher_ptr(), glob_url, {"/hub/r/hubscopepolicytest/"}, 0);

  // test that we can see system objects
  RunComponent(nested_env->launcher_ptr(), glob_url, {"/hub/c/glob.cmx/*/system_objects"}, 0);
}

TEST_F(HubTest, SystemObjectsThreads) {
  std::string url =
      "fuchsia-pkg://fuchsia.com/appmgr_integration_tests#meta/"
      "appmgr_integration_tests_inspect_test_app.cmx";

  auto env_name = fxl::StringPrintf("test-%lu", time(NULL));

  auto nested_env = CreateNewEnclosingEnvironment(env_name, CreateServices());
  WaitForEnclosingEnvToStart(nested_env.get());

  fuchsia::sys::ComponentControllerPtr controller = nested_env->CreateComponentFromUrl(url);
  bool ready = false;
  controller.events().OnDirectoryReady = [&] { ready = true; };
  RunLoopUntil([&] { return ready; });

  auto paths = inspect_deprecated::SyncSearchGlobs(
      {fxl::StringPrintf("/hub/r/%s/*/c/appmgr_integration_tests_inspect_test_app.cmx/*/"
                         "system_objects/*",
                         env_name.c_str())});

  ASSERT_EQ(1U, paths.size());

  auto read = inspect_deprecated::ReadLocation(paths[0]);
  async::Executor executor_(dispatcher());

  fit::result<inspect_deprecated::Source, std::string> result;
  executor_.schedule_task(read.then(
      [&](fit::result<inspect_deprecated::Source, std::string>& res) { result = std::move(res); }));

  RunLoopUntil([&] { return !!result; });

  ASSERT_TRUE(result.is_ok()) << result.take_error();

  auto* stacks = result.value().GetHierarchy().GetByPath({"threads", "all_thread_stacks"});
  ASSERT_NE(nullptr, stacks);
  EXPECT_THAT(*stacks, NodeMatches(PropertyList(ElementsAre(StringPropertyIs(
                           "stacks", "\nERROR (CF-812): Full thread dump disabled")))));
}

TEST_F(HubTest, SystemObjectsThreadsInUseWhileFreed) {
  std::string url =
      "fuchsia-pkg://fuchsia.com/appmgr_integration_tests#meta/"
      "appmgr_integration_tests_inspect_test_app.cmx";

  auto env_name = fxl::StringPrintf("test-%lu", time(NULL));

  auto nested_env = CreateNewEnclosingEnvironment(env_name, CreateServices());
  WaitForEnclosingEnvToStart(nested_env.get());

  fuchsia::sys::ComponentControllerPtr controller = nested_env->CreateComponentFromUrl(url);
  bool ready = false;
  controller.events().OnDirectoryReady = [&] { ready = true; };
  RunLoopUntil([&] { return ready; });

  auto paths = inspect_deprecated::SyncSearchGlobs(
      {fxl::StringPrintf("/hub/r/%s/*/c/appmgr_integration_tests_inspect_test_app.cmx/*/"
                         "system_objects/*",
                         env_name.c_str())});

  ASSERT_EQ(1U, paths.size());

  async::Executor executor_(dispatcher());

  fuchsia::inspect::InspectPtr inspect;
  auto endpoint = paths[0].AbsoluteFilePath();
  zx_status_t status =
      fdio_service_connect(endpoint.c_str(), inspect.NewRequest().TakeChannel().get());
  ASSERT_EQ(ZX_OK, status);
  auto reader = std::make_unique<inspect_deprecated::ObjectReader>(std::move(inspect));

  bool reader_open = false;
  executor_.schedule_task(
      reader->Read().and_then([&](fuchsia::inspect::Object& unused) { reader_open = true; }));
  RunLoopUntil([&] { return reader_open; });

  auto open_child = reader->OpenChild("threads");

  bool terminated = false;
  controller.events().OnTerminated = [&](uint64_t status, fuchsia::sys::TerminationReason reason) {
    terminated = true;
  };
  controller->Kill();
  RunLoopUntil([&] { return terminated; });
  controller.Unbind();

  std::unique_ptr<inspect_deprecated::ObjectReader> all_stack_reader;
  executor_.schedule_task(
      open_child
          .and_then([&](inspect_deprecated::ObjectReader& next) {
            return next.OpenChild("all_thread_stacks");
          })
          .and_then([&](inspect_deprecated::ObjectReader& next) {
            all_stack_reader = std::make_unique<inspect_deprecated::ObjectReader>(std::move(next));
          }));

  RunLoopUntil([&] { return !!all_stack_reader; });
  reader.reset();

  // At this point in time we have an open FIDL connection to a node in the
  // SystemObjectsDirectory. Accessing that node should not cause a crash and
  // will give no visible error.
  fit::result<fuchsia::inspect::Object> result;
  executor_.schedule_task(all_stack_reader->Read().then(
      [&](fit::result<fuchsia::inspect::Object>& res) { result = std::move(res); }));

  RunLoopUntil([&] { return !!result; });
  EXPECT_TRUE(result.is_ok());
}

}  // namespace
}  // namespace component
