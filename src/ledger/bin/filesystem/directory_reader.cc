// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/filesystem/directory_reader.h"

#include <dirent.h>
#include <fcntl.h>
#include <lib/fit/function.h>

#include <functional>
#include <memory>

#include "src/lib/fxl/strings/string_view.h"

namespace ledger {
namespace {

void SafeCloseDir(DIR* dir) {
  if (dir)
    closedir(dir);
}

}  // namespace

bool GetDirectoryEntries(const DetachedPath& directory,
                         fit::function<bool(fxl::StringView)> callback) {
  int dir_fd = openat(directory.root_fd(), directory.path().c_str(), O_RDONLY);
  if (dir_fd == -1) {
    return false;
  }
  std::unique_ptr<DIR, decltype(&SafeCloseDir)> dir(fdopendir(dir_fd), SafeCloseDir);
  if (!dir)
    return false;
  for (struct dirent* entry = readdir(dir.get()); entry != nullptr; entry = readdir(dir.get())) {
    char* name = entry->d_name;
    if (name[0]) {
      if (name[0] == '.') {
        if (!name[1] || (name[1] == '.' && !name[2])) {
          // . or ..
          continue;
        }
      }
      if (!callback(fxl::StringView(name))) {
        break;
      }
    }
  }
  return true;
}

}  // namespace ledger
