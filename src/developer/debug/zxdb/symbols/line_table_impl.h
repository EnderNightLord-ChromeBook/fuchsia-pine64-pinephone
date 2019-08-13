// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_IMPL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_IMPL_H_

#include "src/developer/debug/zxdb/symbols/line_table.h"

namespace zxdb {

// Implementation of LineTable backed by LLVM's DWARFDebugLine.
class LineTableImpl : public LineTable {
 public:
  // The passed-in pointer must outlive this class.
  explicit LineTableImpl(llvm::DWARFContext* context, llvm::DWARFUnit* unit);
  ~LineTableImpl() override;

  // LineTable implementation.
  size_t GetNumFileNames() const override;
  const std::vector<llvm::DWARFDebugLine::Row>& GetRows() const override;
  std::optional<std::string> GetFileNameByIndex(uint64_t file_id) const override;
  llvm::DWARFDie GetSubroutineForRow(const llvm::DWARFDebugLine::Row& row) const override;

 private:
  llvm::DWARFUnit* unit_;

  const llvm::DWARFDebugLine::LineTable* line_table_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_LINE_TABLE_IMPL_H_
