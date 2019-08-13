// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>

#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/sys/cpp/file_descriptor.h>
#include <lib/sys/cpp/testing/test_with_environment.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/lib/files/file.h"
#include "src/lib/files/scoped_temp_dir.h"

namespace intl_wisdom {

using fuchsia::sys::ComponentControllerPtr;
using fuchsia::sys::LaunchInfo;
using sys::testing::TestWithEnvironment;

constexpr char kIntlWisdomClientPackage[] =
    "fuchsia-pkg://fuchsia.com/intl_wisdom#meta/intl_wisdom_client.cmx";

// Integration test for IntlWisdomClient and IntlWisdomServer.
//
// Starts a client, which starts a server and asks it for wisdom. Compares the
// entire STDOUT output of the client (including the server's response) to an
// expected output file.
class IntlWisdomTest : public TestWithEnvironment {
 protected:
  void SetUp() override {
    TestWithEnvironment::SetUp();
    OpenNewOutFile();
  }

  void TearDown() override {
    CloseOutFile();
    TestWithEnvironment::TearDown();
  }

  void OpenNewOutFile() {
    ASSERT_TRUE(temp_dir_.NewTempFile(&out_file_path_));
    out_file_ = std::fopen(out_file_path_.c_str(), "w");
  }

  void CloseOutFile() {
    if (out_file_ != nullptr) {
      std::fclose(out_file_);
    }
  }

  // Read the contents of the file at |path| into |contents|.
  void ReadFile(const std::string& path, std::string& contents) {
    ASSERT_TRUE(files::ReadFileToString(path, &contents)) << "Could not read file " << path;
  }

  // Read the contents of the file at |out_file_path_| into |contents|.
  void ReadStdOutFile(std::string& contents) { ReadFile(out_file_path_, contents); }

  ComponentControllerPtr LaunchClientWithServer() {
    LaunchInfo launch_info{
        .url = kIntlWisdomClientPackage,
        .out = sys::CloneFileDescriptor(fileno(out_file_)),
        .err = sys::CloneFileDescriptor(STDERR_FILENO),
        .arguments =
            std::vector<std::string>({
                "--timestamp=2018-11-01T12:34:56Z",
                "--timezone=America/Los_Angeles",
            }),
    };

    ComponentControllerPtr controller;
    CreateComponentInCurrentEnvironment(std::move(launch_info), controller.NewRequest());
    return controller;
  }

 private:
  files::ScopedTempDir temp_dir_;
  std::string out_file_path_;
  FILE* out_file_ = nullptr;
};

TEST_F(IntlWisdomTest, RunWisdomClientAndServer) {
  std::string expected_output;
  ReadFile("/pkg/data/golden-output.txt", expected_output);

  ComponentControllerPtr controller = LaunchClientWithServer();
  ASSERT_TRUE(RunComponentUntilTerminated(std::move(controller), nullptr));
  std::string actual_output;
  ReadStdOutFile(actual_output);
  ASSERT_EQ(actual_output, expected_output);
}

}  // namespace intl_wisdom
