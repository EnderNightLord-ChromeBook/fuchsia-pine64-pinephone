// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/build_id_index.h"

#include <filesystem>

#include "gtest/gtest.h"
#include "src/developer/debug/zxdb/common/host_util.h"

namespace zxdb {

namespace {

const char kSmallTestBuildID[] = "763feb38b0e37a89964c330c5cf7f7af2ce79e54";

std::filesystem::path GetTestDataDir() {
  std::filesystem::path path(GetSelfPath());
  path.remove_filename();
  path.append("test_data/zxdb/");
  return path;
}

std::filesystem::path GetSmallTestFile() { return GetTestDataDir() / "small_test_file.elf"; }

}  // namespace

// Index one individual file.
TEST(BuildIDIndex, IndexFile) {
  BuildIDIndex index;
  std::string test_file = GetSmallTestFile();
  index.AddSymbolSource(test_file);

  // The known file should be found. We have no debug symbols for this binary,
  // so it shouldn't show as debug info.
  EXPECT_EQ("", index.FileForBuildID(kSmallTestBuildID, DebugSymbolFileType::kDebugInfo));
  EXPECT_EQ(test_file, index.FileForBuildID(kSmallTestBuildID, DebugSymbolFileType::kBinary));

  // Test some random build ID fails.
  EXPECT_EQ("", index.FileForBuildID("random build id", DebugSymbolFileType::kDebugInfo));
}

// Index all files in a directory.
TEST(BuildIDIndex, IndexDir) {
  BuildIDIndex index;
  index.AddSymbolSource(GetTestDataDir());

  // It should have found the small test file and indexed it.
  EXPECT_EQ(GetSmallTestFile(),
            index.FileForBuildID(kSmallTestBuildID, DebugSymbolFileType::kBinary));
}

TEST(BuildIDIndex, ParseIDFile) {
  // Malformed line (no space) and empty line should be ignored. First one also
  // has two spaces separating which should be handled.
  const char test_data[] =
      R"(ff344c5304043feb  /home/me/fuchsia/out/x64/exe.unstripped/false
ff3a9a920026380f8990a27333ed7634b3db89b9 /home/me/fuchsia/out/build-zircon/build-x64/system/dev/display/imx8m-display/libimx8m-display.so
asdf

ffc2990b78544c1cee5092c3bf040b53f2af10cf /home/me/fuchsia/out/build-zircon/build-x64/system/uapp/channel-perf/channel-perf.elf
deadb33fbadf00dbaddadbabb relative/path/dummy.elf
)";

  BuildIDIndex::IDMap map;
  BuildIDIndex::ParseIDs(test_data, GetTestDataDir(), &map);

  EXPECT_EQ(4u, map.size());
  EXPECT_EQ("/home/me/fuchsia/out/x64/exe.unstripped/false", map["ff344c5304043feb"].debug_info);
  EXPECT_EQ(
      "/home/me/fuchsia/out/build-zircon/build-x64/system/dev/display/"
      "imx8m-display/libimx8m-display.so",
      map["ff3a9a920026380f8990a27333ed7634b3db89b9"].debug_info);
  EXPECT_EQ(
      "/home/me/fuchsia/out/build-zircon/build-x64/system/uapp/channel-perf/"
      "channel-perf.elf",
      map["ffc2990b78544c1cee5092c3bf040b53f2af10cf"].debug_info);
  EXPECT_EQ(GetTestDataDir() / "relative/path/dummy.elf",
            map["deadb33fbadf00dbaddadbabb"].debug_info);
}

}  // namespace zxdb
