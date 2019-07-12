// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_MODULE_SYMBOLS_IMPL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_MODULE_SYMBOLS_IMPL_H_

#include "gtest/gtest_prod.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/index.h"
#include "src/developer/debug/zxdb/symbols/location.h"
#include "src/developer/debug/zxdb/symbols/module_symbols.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace llvm {

class DWARFCompileUnit;
class DWARFContext;
class MemoryBuffer;

namespace object {
class Binary;
}  // namespace object

}  // namespace llvm

namespace zxdb {

class DwarfSymbolFactory;
class Variable;

// Represents the symbols for a module (executable or shared library).
//
// All addresses in and out of the API of this class are module-relative. This way, the symbol
// information can be shared between multiple processes that have mapped the same .so file (often at
// different addresses). This means that callers have to offset addresses when calling into this
// class, and offset them in the opposite way when they get the results.
class ModuleSymbolsImpl : public ModuleSymbols {
 public:
  // You must call Load before using this class.
  explicit ModuleSymbolsImpl(const std::string& name, const std::string& binary_name,
                             const std::string& build_id);
  ~ModuleSymbolsImpl();

  llvm::DWARFContext* context() { return context_.get(); }
  llvm::DWARFUnitVector& compile_units() { return compile_units_; }
  DwarfSymbolFactory* symbol_factory() { return symbol_factory_.get(); }

  Err Load();

  fxl::WeakPtr<ModuleSymbolsImpl> GetWeakPtr();

  // ModuleSymbols implementation.
  ModuleSymbolStatus GetStatus() const override;
  std::vector<Location> ResolveInputLocation(
      const SymbolContext& symbol_context, const InputLocation& input_location,
      const ResolveOptions& options = ResolveOptions()) const override;
  LineDetails LineDetailsForAddress(const SymbolContext& symbol_context,
                                    uint64_t absolute_address) const override;
  std::vector<std::string> FindFileMatches(std::string_view name) const override;
  std::vector<fxl::RefPtr<Function>> GetMainFunctions() const override;
  const Index& GetIndex() const override;
  LazySymbol IndexDieRefToSymbol(const IndexNode::DieRef&) const override;
  bool HasBinary() const override;

 private:
  FRIEND_TEST(ModuleSymbols, ResolveMainFunction);

  llvm::DWARFUnit* CompileUnitForRelativeAddress(uint64_t relative_address) const;

  // Helpers for ResolveInputLocation() for the different types of inputs.
  std::vector<Location> ResolveLineInputLocation(const SymbolContext& symbol_context,
                                                 const InputLocation& input_location,
                                                 const ResolveOptions& options) const;
  std::vector<Location> ResolveSymbolInputLocation(const SymbolContext& symbol_context,
                                                   const InputLocation& input_location,
                                                   const ResolveOptions& options) const;
  std::vector<Location> ResolveAddressInputLocation(const SymbolContext& symbol_context,
                                                    const InputLocation& input_location,
                                                    const ResolveOptions& options) const;

  // Symbolizes the given address if possible.
  Location LocationForAddress(const SymbolContext& symbol_context, uint64_t absolute_address) const;

  // Converts the given global or static variable to a Location. This doesn't work for local
  // variables which are dynamic and based on the current CPU state and stack.
  Location LocationForVariable(const SymbolContext& symbol_context,
                               fxl::RefPtr<Variable> variable) const;

  // Converts a Function object to a found location according to the options and adds it to the
  // list. May append nothing if there is no code for the function.
  void AppendLocationForFunction(const SymbolContext& symbol_context, const ResolveOptions& options,
                                 const Function* func, std::vector<Location>* result) const;

  // Resolves the line number information for the given file, which must be an exact match. This is
  // a helper function for ResolveLineInputLocation().
  //
  // This appends to the given output.
  void ResolveLineInputLocationForFile(const SymbolContext& symbol_context,
                                       const std::string& canonical_file, int line_number,
                                       const ResolveOptions& options,
                                       std::vector<Location>* output) const;

  const std::string name_;
  const std::string binary_name_;
  const std::string build_id_;

  std::unique_ptr<llvm::MemoryBuffer> binary_buffer_;  // Backing for binary_.
  std::unique_ptr<llvm::object::Binary> binary_;
  std::unique_ptr<llvm::DWARFContext> context_;

  llvm::DWARFUnitVector compile_units_;

  Index index_;

  std::map<std::string, uint64_t> plt_locations_;

  fxl::RefPtr<DwarfSymbolFactory> symbol_factory_;

  fxl::WeakPtrFactory<ModuleSymbolsImpl> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ModuleSymbolsImpl);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_MODULE_SYMBOLS_IMPL_H_
