// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_THREAD_H
#define PLATFORM_THREAD_H

#include <cstdint>
#include <string>

#include "platform_handle.h"

namespace magma {

// Use std::thread except for ids.
class PlatformThreadId {
 public:
  PlatformThreadId() { SetToCurrent(); }

  uint64_t id() { return id_; }

  void SetToCurrent() { id_ = GetCurrentThreadId(); }

  bool IsCurrent() { return id_ == GetCurrentThreadId(); }

 private:
  static uint64_t GetCurrentThreadId();

  uint64_t id_ = 0;
};

class PlatformThreadHelper {
 public:
  static void SetCurrentThreadName(const std::string& name);
  static std::string GetCurrentThreadName();

  static bool SetProfile(PlatformHandle* profile);
};

class PlatformProcessHelper {
 public:
  static std::string GetCurrentProcessName();
  static uint64_t GetCurrentProcessId();
};

}  // namespace magma

#endif  // PLATFORM_THREAD_H
