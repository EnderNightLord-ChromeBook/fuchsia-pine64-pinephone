// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/thread.h"

#include "src/developer/debug/zxdb/client/setting_schema_definition.h"
#include "src/developer/debug/zxdb/client/system.h"
#include "src/developer/debug/zxdb/client/target.h"

namespace zxdb {

// Schema Definition -----------------------------------------------------------

namespace {

fxl::RefPtr<SettingSchema> CreateSchema() {
  auto schema = fxl::MakeRefCounted<SettingSchema>();
  return schema;
}

}  // namespace

// Thread Implementation -------------------------------------------------------

Thread::Thread(Session* session)
    : ClientObject(session),
      // Implementations can set up fallbacks if needed.
      settings_(GetSchema(), nullptr),
      weak_factory_(this) {}

Thread::~Thread() = default;

void Thread::AddObserver(ThreadObserver* observer) { observers_.AddObserver(observer); }

void Thread::RemoveObserver(ThreadObserver* observer) { observers_.RemoveObserver(observer); }

fxl::WeakPtr<Thread> Thread::GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

fxl::RefPtr<SettingSchema> Thread::GetSchema() {
  // Will only run initialization once.
  InitializeSchemas();
  static fxl::RefPtr<SettingSchema> schema = CreateSchema();
  return schema;
}

}  // namespace zxdb
