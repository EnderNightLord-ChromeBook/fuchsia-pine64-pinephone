// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/inspect_deprecated/query/read.h"

#include <fuchsia/io/cpp/fidl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <lib/fdio/namespace.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/defer.h>
#include <lib/inspect_deprecated/inspect.h>
#include <lib/inspect_deprecated/query/source.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/vfs/cpp/service.h>
#include <lib/vfs/cpp/vmo_file.h>
#include <src/lib/fxl/strings/join_strings.h>

#include "fixture.h"
#include "fuchsia/inspect/cpp/fidl.h"
#include "lib/inspect_deprecated/hierarchy.h"
#include "lib/inspect_deprecated/query/location.h"
#include "lib/inspect_deprecated/reader.h"
#include "lib/inspect_deprecated/testing/inspect.h"

using namespace inspect_deprecated::testing;

namespace {

class TestDataWrapper {
 public:
  explicit TestDataWrapper(inspect_deprecated::Node object) : object_(std::move(object)) {
    version_ = object_.CreateStringProperty("version", "1.0");
    child_test_ = object_.CreateChild("test");
    count_ = child_test_.CreateIntMetric("count", 2);
  }

 private:
  inspect_deprecated::Node object_;
  inspect_deprecated::Node child_test_;
  inspect_deprecated::StringProperty version_;
  inspect_deprecated::IntMetric count_;
};

class ReadTest : public TestFixture {
 public:
  ReadTest()
      : tree_(inspector_.CreateTree("root")),
        fidl_dir_(component::ObjectDir::Make("root")),
        fidl_test_data_(inspect_deprecated::Node(fidl_dir_)),
        vmo_test_data_(std::move(tree_.GetRoot())) {
    // Host a FIDL and VMO inspect interface under /test in the global
    // namespace.
    root_dir_.AddEntry(
        fuchsia::inspect::Inspect::Name_,
        std::make_unique<vfs::Service>(bindings_.GetHandler(fidl_dir_.object().get())));

    root_dir_.AddEntry("root.inspect",
                       std::make_unique<vfs::VmoFile>(zx::unowned_vmo(tree_.GetVmo()), 0, 4096));

    fuchsia::io::DirectoryPtr ptr;
    root_dir_.Serve(fuchsia::io::OPEN_RIGHT_READABLE | fuchsia::io::OPEN_RIGHT_WRITABLE,
                    ptr.NewRequest().TakeChannel());
    ZX_ASSERT(fdio_ns_get_installed(&ns_) == ZX_OK);
    ZX_ASSERT(fdio_ns_bind(ns_, "/test", ptr.Unbind().TakeChannel().release()) == ZX_OK);
    ns_cleanup_ = [this] { ZX_ASSERT(fdio_ns_unbind(ns_, "/test") == ZX_OK); };
  }

 protected:
  inspect_deprecated::Inspector inspector_;
  inspect_deprecated::Tree tree_;
  component::ObjectDir fidl_dir_;
  TestDataWrapper fidl_test_data_, vmo_test_data_;
  fidl::BindingSet<fuchsia::inspect::Inspect> bindings_;
  fdio_ns_t* ns_;
  // Ensure ns is cleaned up after the root_dir is destroyed and no longer
  // depending on it.
  fit::deferred_action<fit::closure> ns_cleanup_;
  vfs::PseudoDir root_dir_;
};

TEST_F(ReadTest, ReadLocations) {
  // TODO(FLK-297): Reenable this test.
  GTEST_SKIP();
  const std::vector<std::string> paths = {"/test/root.inspect", "/test"};

  for (const auto& path : paths) {
    fit::result<inspect_deprecated::Source, std::string> result;

    SchedulePromise(
        inspect_deprecated::ReadLocation(inspect_deprecated::Location::Parse(path).take_value())
            .then([&](fit::result<inspect_deprecated::Source, std::string>& res) {
              result = std::move(res);
            }));

    RunLoopUntil([&] { return !!result; });

    ASSERT_TRUE(result.is_ok()) << "for " << path << " error " << result.error().c_str();
    EXPECT_THAT(result.take_value().GetHierarchy(),
                ::testing::AllOf(
                    NodeMatches(::testing::AllOf(
                        NameMatches("root"),
                        PropertyList(::testing::ElementsAre(StringPropertyIs("version", "1.0"))))),
                    ChildrenMatch(::testing::ElementsAre(NodeMatches(::testing::AllOf(
                        NameMatches("test"),
                        MetricList(::testing::ElementsAre(IntMetricIs("count", 2)))))))));
  }
}

TEST_F(ReadTest, ReadLocationsChild) {
  // TODO(FLK-297): Reenable this test.
  GTEST_SKIP();
  const std::vector<std::string> paths = {"/test/root.inspect#test", "/test#test"};

  for (const auto& path : paths) {
    fit::result<inspect_deprecated::Source, std::string> result;

    SchedulePromise(
        inspect_deprecated::ReadLocation(inspect_deprecated::Location::Parse(path).take_value())
            .then([&](fit::result<inspect_deprecated::Source, std::string>& res) {
              result = std::move(res);
            }));

    RunLoopUntil([&] { return !!result; });

    ASSERT_TRUE(result.is_ok()) << "for " << path << " error " << result.error().c_str();
    EXPECT_THAT(
        result.take_value().GetHierarchy(),
        NodeMatches(::testing::AllOf(NameMatches("test"),
                                     MetricList(::testing::ElementsAre(IntMetricIs("count", 2))))));
  }
}

TEST_F(ReadTest, ReadLocationsError) {
  const std::vector<std::string> paths = {
      "/test/root.inspect#missing", "/test#missing", "/", "/test/missing.inspect", "/test/missing",
  };

  for (const auto& path : paths) {
    fit::result<inspect_deprecated::Source, std::string> result;

    SchedulePromise(
        inspect_deprecated::ReadLocation(inspect_deprecated::Location::Parse(path).take_value())
            .then([&](fit::result<inspect_deprecated::Source, std::string>& res) {
              result = std::move(res);
            }));

    RunLoopUntil([&] { return !!result; });

    ASSERT_TRUE(result.is_error());
  }
}

}  // namespace
