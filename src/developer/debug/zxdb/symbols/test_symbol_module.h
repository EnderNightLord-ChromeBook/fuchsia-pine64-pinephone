// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_TEST_SYMBOL_MODULE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_TEST_SYMBOL_MODULE_H_

#include <memory>
#include <string>
#include <string_view>

#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/ObjectFile.h"
#include "src/developer/debug/zxdb/symbols/identifier.h"
#include "src/lib/fxl/macros.h"

namespace llvm {

class DWARFCompileUnit;
class DWARFContext;
class MemoryBuffer;

namespace object {
class Binary;
}  // namespace object

}  // namespace llvm

namespace zxdb {

// This class loads the unstripped zxdb_symbol_test module with the
// required LLDB classes for writing symbol testing.
class TestSymbolModule {
 public:
  // These constants identify locations in the symbol test files.
  static const char kMyNamespaceName[];
  static const char kMyFunctionName[];
  static const int kMyFunctionLine;
  static const char kNamespaceFunctionName[];
  static const char kMyClassName[];
  static const char kMyInnerClassName[];
  static const char kMyMemberOneName[];
  static const char kFunctionInTest2Name[];
  static const char kMyMemberTwoName[];
  static const char kAnonNSFunctionName[];
  static const char kGlobalName[];
  static const char kClassStaticName[];
  static const char kPltFunctionName[];
  static const uint64_t kPltFunctionOffset;

  TestSymbolModule();
  ~TestSymbolModule();

  // Returns the name of the .so file used by this class for doing tests with
  // it that involve different types of setup.
  static std::string GetTestFileName();

  // Returns the checked in .so used for line testing. As the mapping changes
  // between architectures, the file is compiled offline and remains the same.
  static std::string GetCheckedInTestFileName();

  // Returns the Build ID for the checked in .so returned by GetCheckedInTestFileName.
  static std::string GetCheckedInTestFileBuildID();

  // Returns a stripped version of the file returned by
  // GetCheckedInTestFileName().
  static std::string GetStrippedCheckedInTestFileName();

  // Loads the test file. On failure, returns false and sets the given error
  // message to be something helpful.
  bool Load(std::string* err_msg);

  // Loads a file at the given path. See Load().
  bool LoadSpecific(const std::string& path, std::string* err_msg);

  llvm::object::ObjectFile* object_file() {
    return static_cast<llvm::object::ObjectFile*>(binary_.get());
  }
  llvm::DWARFContext* context() { return context_.get(); }
  llvm::DWARFUnitVector& compile_units() { return compile_units_; }

  // Helper to convert symbol names to vectors of components without using the
  // "expr" library. This just splits on "::" which handles most cases but
  // not elaborate templates.
  static Identifier SplitName(std::string_view input);

 private:
  std::unique_ptr<llvm::MemoryBuffer> binary_buffer_;  // Backing for binary_.
  std::unique_ptr<llvm::object::Binary> binary_;
  std::unique_ptr<llvm::DWARFContext> context_;

  llvm::DWARFUnitVector compile_units_;

  FXL_DISALLOW_COPY_AND_ASSIGN(TestSymbolModule);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_TEST_SYMBOL_MODULE_H_
