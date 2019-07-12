// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_EXPR_EVAL_CONTEXT_IMPL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_EXPR_EVAL_CONTEXT_IMPL_H_

#include <optional>
#include <vector>

#include "src/developer/debug/zxdb/expr/eval_context.h"
#include "src/developer/debug/zxdb/expr/found_name.h"
#include "src/developer/debug/zxdb/symbols/symbol.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class CodeBlock;
class DwarfExprEval;
class LazySymbol;
class Location;
class ProcessSymbols;
class SymbolDataProvider;
class Variable;

// An implementation of EvalContext that integrates with the DWARF symbol
// system. It will provide the values of variables currently in scope.
//
// This object is reference counted since it requires asynchronous operations
// in some cases. This means it can outlive the scope in which it was invoked
// (say if the thread was resumed or the process was killed).
//
// Generally the creator of this context will be something representing that
// context in the running program like a stack frame. This frame should call
// DisownContext() when it is destroyed to ensure that evaluation does not use
// any invalid context.
class EvalContextImpl : public EvalContext {
 public:
  // All of the input pointers can be null:
  //
  //  - The ProcessSymbols can be a null weak pointer in which case globals will not be resolved.
  //    This can make testing easier and supports evaluating math without a loaded program.
  //
  //  - The SymbolDataProvider can be null in which case anything that requires memory from the
  //    target will fail. Some operations like pure math don't require this.
  //
  //  - The code block can be null in which case nothing using the current scope will work. This
  //    includes local variables, variables on "this", and things relative to the current namespace.
  //
  // The variant that takes a location will extract the code block from the location if possible.
  EvalContextImpl(fxl::WeakPtr<const ProcessSymbols> process_symbols,
                  const SymbolContext& symbol_context,
                  fxl::RefPtr<SymbolDataProvider> data_provider,
                  fxl::RefPtr<CodeBlock> code_block = fxl::RefPtr<CodeBlock>());
  EvalContextImpl(fxl::WeakPtr<const ProcessSymbols> process_symbols,
                  fxl::RefPtr<SymbolDataProvider> data_provider, const Location& location);
  ~EvalContextImpl() override;

  // EvalContext implementation.
  ExprLanguage GetLanguage() const override;
  void GetNamedValue(const ParsedIdentifier& name, ValueCallback cb) const override;
  void GetVariableValue(fxl::RefPtr<Variable> variable, ValueCallback cb) const override;
  fxl::RefPtr<Type> ResolveForwardDefinition(const Type* type) const override;
  fxl::RefPtr<Type> GetConcreteType(const Type* type) const override;
  fxl::RefPtr<SymbolDataProvider> GetDataProvider() override;
  NameLookupCallback GetSymbolNameLookupCallback() override;
  Location GetLocationForAddress(uint64_t address) const override;

 private:
  struct ResolutionState;

  // Computes the value of the given variable and issues the callback (possibly
  // asynchronously, possibly not).
  void DoResolve(FoundName found, ValueCallback cb) const;

  // Callback for when the dwarf_eval_ has completed evaluation.
  void OnDwarfEvalComplete(const Err& err, fxl::RefPtr<ResolutionState> state) const;

  // Implements type name lookup on the target's symbol index.
  FoundName DoTargetSymbolsNameLookup(const ParsedIdentifier& ident);

  FindNameContext GetFindNameContext() const;

  fxl::WeakPtr<const ProcessSymbols> process_symbols_;  // Possibly null.
  SymbolContext symbol_context_;
  fxl::RefPtr<SymbolDataProvider> data_provider_;  // Possibly null.

  // Innermost block of the current context. May be null if there is none
  // (this means you won't get any local variable lookups).
  fxl::RefPtr<const CodeBlock> block_;

  // Language extracted from the code block.
  ExprLanguage language_ = ExprLanguage::kC;

  mutable fxl::WeakPtrFactory<EvalContextImpl> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_EXPR_EVAL_CONTEXT_IMPL_H_
