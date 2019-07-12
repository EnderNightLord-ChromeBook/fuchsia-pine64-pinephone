// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_COMMON_FILE_UTIL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_COMMON_FILE_UTIL_H_

#include <string>
#include <string_view>

namespace zxdb {

// Extracts the substring into the given file path of the last path component
// (the stuff following the last slash). If the path ends in a slash, it will
// return an empty StringView. If the input has no slash, it will return the
// whole thing.
std::string_view ExtractLastFileComponent(std::string_view path);

// Returns true if the given file path is absolute (begins with a slash).
// The contents could still have relative components ("/foo/../bar" is still
// absolute).
bool IsPathAbsolute(const std::string& path);

// Concatenates the two path components with a slash in between them. "first"
// can end with a slash or not. The second component shouldn't begin with a
// slash.
std::string CatPathComponents(const std::string& first, const std::string& second);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_COMMON_FILE_UTIL_H_
