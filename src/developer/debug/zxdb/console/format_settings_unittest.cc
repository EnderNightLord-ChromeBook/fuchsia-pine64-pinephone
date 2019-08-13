// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/format_settings.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/client/setting_schema.h"
#include "src/developer/debug/zxdb/client/setting_store.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"

namespace zxdb {

fxl::RefPtr<SettingSchema> GetSchema() {
  auto schema = fxl::MakeRefCounted<SettingSchema>();

  schema->AddBool("setting-bool", "Setting bool description");
  schema->AddBool("setting-bool2", "Setting bool description", true);

  schema->AddInt("setting-int", "Setting int description");
  schema->AddInt("setting-int2", "Setting int description", 12334);

  schema->AddString("setting-string", "Setting string description");
  schema->AddString("setting-string2", R"(
  Setting string description,
  with many lines.)",
                    "Test string");

  schema->AddList("setting-list", "Setting list description");
  schema->AddList("setting-list2", R"(
  Some very long description about how this setting is very important to the
  company and all its customers.)",
                  {"first", "second", "third"});

  return schema;
}

TEST(FormatSetting, Setting) {
  SettingStore store(GetSchema(), nullptr);

  Setting setting = store.GetSetting("setting-string2");
  OutputBuffer out = FormatSetting(setting);
  EXPECT_EQ(
      "setting-string2\n"
      "\n"
      "  Setting string description,\n"
      "  with many lines.\n"
      "\n"
      "Type: string\n"
      "\n"
      "Value(s):\n"
      "Test string\n",
      out.AsString());
}

TEST(FormatSetting, List) {
  std::vector<std::string> options = {
      "/some/very/long/and/annoying/path/that/actually/leads/nowhere",
      "/another/some/very/long/and/annoying/path/that/actually/leads/nowhere",
      "/yet/another/some/very/long/and/annoying/path/that/actually/leads/"
      "nowhere"};

  SettingStore store(GetSchema(), nullptr);
  Err err = store.SetList("setting-list2", std::move(options));
  EXPECT_FALSE(err.has_error()) << err.msg();

  Setting setting = store.GetSetting("setting-list2");

  // clang-format makes this one very hard to read.
  // Leave this text easier.
  OutputBuffer out = FormatSetting(setting);
  EXPECT_EQ(
      "setting-list2\n"
      "\n"
      "  Some very long description about how this setting is very important "
      "to the\n"
      "  company and all its customers.\n"
      "\n"
      "Type: list\n"
      "\n"
      "Value(s):\n"
      "• /some/very/long/and/annoying/path/that/actually/leads/nowhere\n"
      "• "
      "/another/some/very/long/and/annoying/path/that/actually/leads/nowhere\n"
      "• "
      "/yet/another/some/very/long/and/annoying/path/that/actually/leads/"
      "nowhere\n"
      "\n"
      "See \"help set\" about using the set value for lists.\n"
      "To set, type: set setting-list2 "
      "/some/very/long/and/annoying/path/that/actually/leads/nowhere:/another/"
      "some/very/long/and/annoying/path/that/actually/leads/nowhere:/yet/"
      "another/some/very/long/and/annoying/path/that/actually/leads/nowhere\n",
      out.AsString());
}

}  // namespace zxdb
