// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FILE_LINE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FILE_LINE_H_

#include <string>

namespace zxdb {

class FileLine {
 public:
  FileLine();
  FileLine(std::string file, int line);
  ~FileLine();

  bool is_valid() const { return !file_.empty() && line_ > 0; }

  const std::string& file() const { return file_; }
  int line() const { return line_; }

 private:
  std::string file_;
  int line_ = 0;
};

// Comparison function for use in set and map.
bool operator<(const FileLine& a, const FileLine& b);

bool operator==(const FileLine& a, const FileLine& b);
bool operator!=(const FileLine& a, const FileLine& b);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_FILE_LINE_H_
