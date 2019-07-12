// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/format_frame.h"

#include "gtest/gtest.h"
#include "src/developer/debug/shared/platform_message_loop.h"
#include "src/developer/debug/zxdb/client/mock_frame.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/console/format_value.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/location.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"

namespace zxdb {

namespace {

// Synchronous wrapper around asynchronous long formatting.
std::string SyncFormatFrameLong(const Frame* frame, const FormatExprValueOptions& options) {
  debug_ipc::PlatformMessageLoop loop;
  loop.Init();

  auto helper = fxl::MakeRefCounted<FormatValue>();
  FormatFrameLong(frame, false, helper.get(), FormatExprValueOptions());

  std::string out_string;
  bool complete = false;
  helper->Complete([&loop, &out_string, &complete](OutputBuffer out) {
    complete = true;
    out_string = out.AsString();
    loop.QuitNow();
  });

  if (!complete) {
    // Did not complete synchronously, run the loop.
    loop.Run();
  }

  EXPECT_TRUE(complete);

  loop.Cleanup();
  return out_string;
}

}  // namespace

TEST(FormatFrame, Unsymbolized) {
  MockFrame frame(nullptr, nullptr, Location(Location::State::kSymbolized, 0x12345678), 0x567890, 0,
                  std::vector<Register>(), 0xdeadbeef);

  OutputBuffer out;

  // Short format just prints the address.
  FormatFrame(&frame, false, &out);
  EXPECT_EQ("0x12345678", out.AsString());

  // Long version should do the same (not duplicate it).
  EXPECT_EQ("\n      IP = 0x12345678, SP = 0x567890, base = 0xdeadbeef",
            SyncFormatFrameLong(&frame, FormatExprValueOptions()));

  // With index.
  out = OutputBuffer();
  FormatFrame(&frame, false, &out, 3);
  EXPECT_EQ("Frame 3 0x12345678", out.AsString());
}

TEST(FormatFrame, Inline) {
  // This is to have some place for the inline frame to refer to as the
  // underlying physical frame. The values are ignored.
  MockFrame physical_frame(nullptr, nullptr, Location(Location::State::kSymbolized, 0x12345678),
                           0x567890);

  SymbolContext symbol_context = SymbolContext::ForRelativeAddresses();

  auto function = fxl::MakeRefCounted<Function>(DwarfTag::kInlinedSubroutine);
  function->set_assigned_name("Function");

  MockFrame inline_frame(
      nullptr, nullptr,
      Location(0x12345678, FileLine("file.cc", 22), 0, symbol_context, LazySymbol(function)),
      0x567890, 0, std::vector<Register>(), 0xdeadbeef, &physical_frame);

  EXPECT_EQ(
      "Function() • file.cc:22 (inline)\n"
      "      IP = 0x12345678, SP = 0x567890, base = 0xdeadbeef",
      SyncFormatFrameLong(&inline_frame, FormatExprValueOptions()));
}

}  // namespace zxdb
