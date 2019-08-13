// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/eval_context_impl.h"

#include "src/developer/debug/shared/message_loop.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/expr/builtin_types.h"
#include "src/developer/debug/zxdb/expr/expr_value.h"
#include "src/developer/debug/zxdb/expr/find_name.h"
#include "src/developer/debug/zxdb/expr/resolve_collection.h"
#include "src/developer/debug/zxdb/expr/resolve_ptr_ref.h"
#include "src/developer/debug/zxdb/symbols/code_block.h"
#include "src/developer/debug/zxdb/symbols/compile_unit.h"
#include "src/developer/debug/zxdb/symbols/data_member.h"
#include "src/developer/debug/zxdb/symbols/dwarf_expr_eval.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/identifier.h"
#include "src/developer/debug/zxdb/symbols/input_location.h"
#include "src/developer/debug/zxdb/symbols/location.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"
#include "src/developer/debug/zxdb/symbols/symbol_data_provider.h"
#include "src/developer/debug/zxdb/symbols/variable.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {
namespace {

debug_ipc::RegisterID GetRegister(const ParsedIdentifier& ident) {
  auto str = GetSingleComponentIdentifierName(ident);
  if (!str)
    return debug_ipc::RegisterID::kUnknown;
  return debug_ipc::StringToRegisterID(*str);
}

}  // namespace

// The data associated with one in-progress variable resolution. This must be
// heap allocated for each resolution operation since multiple operations can
// be pending.
struct EvalContextImpl::ResolutionState : public fxl::RefCountedThreadSafe<ResolutionState> {
  DwarfExprEval dwarf_eval;
  ValueCallback callback;

  // Not necessarily a concrete type, this is the type of the result the user
  // will see.
  fxl::RefPtr<Type> type;

  // The Variable or DataMember that generated the value. Used to execute the
  // callback.
  fxl::RefPtr<Symbol> symbol;

  // This private stuff prevents refcounted mistakes.
 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(ResolutionState);
  FRIEND_MAKE_REF_COUNTED(ResolutionState);

  explicit ResolutionState(ValueCallback cb, fxl::RefPtr<Type> t, fxl::RefPtr<Symbol> s)
      : callback(std::move(cb)), type(std::move(t)), symbol(std::move(s)) {}
  ~ResolutionState() = default;
};

EvalContextImpl::EvalContextImpl(fxl::WeakPtr<const ProcessSymbols> process_symbols,
                                 const SymbolContext& symbol_context,
                                 fxl::RefPtr<SymbolDataProvider> data_provider,
                                 fxl::RefPtr<CodeBlock> code_block)
    : process_symbols_(std::move(process_symbols)),
      symbol_context_(symbol_context),
      data_provider_(data_provider),
      block_(std::move(code_block)),
      weak_factory_(this) {}

EvalContextImpl::EvalContextImpl(fxl::WeakPtr<const ProcessSymbols> process_symbols,
                                 fxl::RefPtr<SymbolDataProvider> data_provider,
                                 const Location& location)
    : process_symbols_(std::move(process_symbols)),
      symbol_context_(location.symbol_context()),
      data_provider_(data_provider),
      weak_factory_(this) {
  if (!location.symbol())
    return;
  const CodeBlock* function = location.symbol().Get()->AsCodeBlock();
  if (function) {
    block_ =
        RefPtrTo(function->GetMostSpecificChild(location.symbol_context(), location.address()));

    // Extract the language for the code if possible.
    if (const CompileUnit* unit = function->GetCompileUnit())
      language_ = DwarfLangToExprLanguage(unit->language());
  }
}

EvalContextImpl::~EvalContextImpl() = default;

ExprLanguage EvalContextImpl::GetLanguage() const { return language_; }

void EvalContextImpl::GetNamedValue(const ParsedIdentifier& identifier, ValueCallback cb) const {
  if (FoundName found =
          FindName(GetFindNameContext(), FindNameOptions(FindNameOptions::kAllKinds), identifier)) {
    switch (found.kind()) {
      case FoundName::kVariable:
      case FoundName::kMemberVariable:
        DoResolve(std::move(found), std::move(cb));
        return;
      case FoundName::kNamespace:
        cb(Err("Can not evaluate a namespace."), nullptr, ExprValue());
        return;
      case FoundName::kTemplate:
        cb(Err("Can not evaluate a template with no parameters."), nullptr, ExprValue());
        return;
      case FoundName::kType:
        cb(Err("Can not evaluate a type."), nullptr, ExprValue());
        return;
      case FoundName::kFunction:
        break;  // Function pointers not supported yet.
      case FoundName::kNone:
        break;  // Fall through to checking other stuff.
    }
  }

  auto reg = GetRegister(identifier);

  if (reg == debug_ipc::RegisterID::kUnknown ||
      GetArchForRegisterID(reg) != data_provider_->GetArch()) {
    cb(Err("No variable '%s' found.", identifier.GetFullName().c_str()), nullptr, ExprValue());
    return;
  }

  // Fall back to matching registers when no symbol is found.
  data_provider_->GetRegisterAsync(reg,
                                   [cb = std::move(cb)](const Err& err, uint64_t value) mutable {
                                     cb(err, fxl::RefPtr<zxdb::Symbol>(), ExprValue(value));
                                   });
}

void EvalContextImpl::GetVariableValue(fxl::RefPtr<Variable> var, ValueCallback cb) const {
  // Need to explicitly take a reference to the type.
  fxl::RefPtr<Type> type = RefPtrTo(var->type().Get()->AsType());
  if (!type) {
    cb(Err("Missing type information."), var, ExprValue());
    return;
  }

  std::optional<uint64_t> ip;
  data_provider_->GetRegister(debug_ipc::GetSpecialRegisterID(data_provider_->GetArch(),
                                                              debug_ipc::SpecialRegisterType::kIP),
                              &ip);
  if (!ip) {
    // The IP should never require an async call.
    cb(Err("No location available."), var, ExprValue());
    return;
  }

  const VariableLocation::Entry* loc_entry = var->location().EntryForIP(symbol_context_, *ip);
  if (!loc_entry) {
    // No DWARF location applies to the current instruction pointer.
    const char* err_str;
    if (var->location().is_null()) {
      // With no locations, this variable has been completely optimized out.
      err_str = "Optimized out.";
    } else {
      // There are locations but none of them match the current IP.
      err_str = "Unavailable";
    }
    cb(Err(ErrType::kOptimizedOut, err_str), var, ExprValue());
    return;
  }

  // Schedule the expression to be evaluated.
  auto state = fxl::MakeRefCounted<ResolutionState>(std::move(cb), std::move(type), std::move(var));
  state->dwarf_eval.Eval(data_provider_, symbol_context_, loc_entry->expression,
                         [state = std::move(state), weak_this = weak_factory_.GetWeakPtr()](
                             DwarfExprEval*, const Err& err) {
                           if (weak_this)
                             weak_this->OnDwarfEvalComplete(err, std::move(state));

                           // Prevent the DwarfExprEval from getting reentrantly deleted from
                           // within its own callback by posting a reference back to the message
                           // loop.
                           debug_ipc::MessageLoop::Current()->PostTask(
                               FROM_HERE, [state = std::move(state)]() {});
                         });
}

fxl::RefPtr<Type> EvalContextImpl::ResolveForwardDefinition(const Type* type) const {
  Identifier ident = type->GetIdentifier();
  if (ident.empty()) {
    // Some things like modified types don't have real identifier names.
    return RefPtrTo(type);
  }
  ParsedIdentifier parsed_ident = ToParsedIdentifier(ident);

  // Search for the first match of a type.
  FindNameOptions opts(FindNameOptions::kNoKinds);
  opts.find_types = true;
  opts.max_results = 1;

  // The type names will always be fully qualified. Mark the identifier as
  // such and only search the global context by clearing the code location.
  parsed_ident.set_qualification(IdentifierQualification::kGlobal);
  auto context = GetFindNameContext();
  context.block = nullptr;

  if (FoundName result = FindName(context, opts, parsed_ident)) {
    FXL_DCHECK(result.type());
    return result.type();
  }

  // Nothing found in the index.
  return RefPtrTo(type);
}

fxl::RefPtr<Type> EvalContextImpl::GetConcreteType(const Type* type) const {
  if (!type)
    return fxl::RefPtr<Type>();

  // Iteratively strip C-V qualifications, follow typedefs, and follow forward
  // declarations.
  fxl::RefPtr<Type> cur = RefPtrTo(type);
  do {
    // Follow forward declarations.
    if (cur->is_declaration()) {
      cur = ResolveForwardDefinition(cur.get());
      if (cur->is_declaration())
        break;  // Declaration can't be resolved, give up.
    }

    // Strip C-V qualifiers and follow typedefs.
    cur = RefPtrTo(cur->StripCVT());
  } while (cur && cur->is_declaration());
  return cur;
}

fxl::RefPtr<SymbolDataProvider> EvalContextImpl::GetDataProvider() { return data_provider_; }

NameLookupCallback EvalContextImpl::GetSymbolNameLookupCallback() {
  // The contract for this function is that the callback must not be stored
  // so the callback can reference |this|.
  return [this](const ParsedIdentifier& ident, const FindNameOptions& opts) -> FoundName {
    // Look up the symbols in the symbol table if possible.
    FoundName result = FindName(GetFindNameContext(), opts, ident);

    // Fall back on builtin types.
    if (result.kind() == FoundName::kNone && opts.find_types) {
      if (auto type = GetBuiltinType(language_, ident.GetFullName()))
        return FoundName(std::move(type));
    }
    return result;
  };
}

Location EvalContextImpl::GetLocationForAddress(uint64_t address) const {
  if (!process_symbols_)
    return Location(Location::State::kAddress, address);  // Can't symbolize.

  auto locations = process_symbols_->ResolveInputLocation(InputLocation(address));

  // Given an exact address, ResolveInputLocation() should only return one result.
  FXL_DCHECK(locations.size() == 1u);
  return locations[0];
}

void EvalContextImpl::DoResolve(FoundName found, ValueCallback cb) const {
  if (found.kind() == FoundName::kVariable) {
    // Simple variable resolution.
    GetVariableValue(found.variable_ref(), std::move(cb));
    return;
  }

  // Object variable resolution: Get the value of of the |this| variable.
  FXL_DCHECK(found.kind() == FoundName::kMemberVariable);
  GetVariableValue(found.object_ptr_ref(),
                   [weak_this = weak_factory_.GetWeakPtr(), found, cb = std::move(cb)](
                       const Err& err, fxl::RefPtr<Symbol> symbol, ExprValue value) mutable {
                     if (!weak_this)
                       return;  // Don't issue callbacks if we've been destroyed.

                     if (err.has_error()) {
                       // |this| not available, probably optimized out.
                       cb(err, symbol, ExprValue());
                       return;
                     }

                     // Got |this|, resolve |this-><DataMember>|.
                     ResolveMemberByPointer(
                         fxl::RefPtr<EvalContextImpl>(weak_this.get()), value, found.member(),
                         [weak_this, found, cb = std::move(cb)](ErrOrValue value) mutable {
                           if (weak_this) {
                             // Only issue callbacks if we're still alive.
                             cb(value.err_or_empty(), found.member().data_member_ref(),
                                std::move(value.take_value_or_empty()));
                           }
                         });
                   });
}

void EvalContextImpl::OnDwarfEvalComplete(const Err& err,
                                          fxl::RefPtr<ResolutionState> state) const {
  if (err.has_error()) {
    // Error decoding.
    state->callback(err, state->symbol, ExprValue());
    return;
  }

  uint64_t result_int = state->dwarf_eval.GetResult();

  // The DWARF expression will produce either the address of the value or the
  // value itself.
  if (state->dwarf_eval.GetResultType() == DwarfExprEval::ResultType::kValue) {
    // Get the concrete type since we need the byte size. But don't use this
    // to actually construct the variable since it will strip "const" and
    // stuff that the user will expect to see.
    fxl::RefPtr<Type> concrete_type = GetConcreteType(state->type.get());

    // The DWARF expression produced the exact value (it's not in memory).
    uint32_t type_size = concrete_type->byte_size();
    if (type_size > sizeof(uint64_t)) {
      state->callback(Err(fxl::StringPrintf("Result size insufficient for type of size %u. "
                                            "Please file a bug with a repro case.",
                                            type_size)),
                      state->symbol, ExprValue());
      return;
    }
    std::vector<uint8_t> data;
    data.resize(type_size);
    memcpy(&data[0], &result_int, type_size);
    state->callback(Err(), state->symbol, ExprValue(state->type, std::move(data)));
  } else {
    // The DWARF result is a pointer to the value.
    ResolvePointer(RefPtrTo(this), result_int, state->type,
                   [state, weak_this = weak_factory_.GetWeakPtr()](ErrOrValue value) {
                     if (weak_this)
                       state->callback(value.err_or_empty(), state->symbol,
                                       value.take_value_or_empty());
                   });
  }
}

FoundName EvalContextImpl::DoTargetSymbolsNameLookup(const ParsedIdentifier& ident) {
  return FindName(GetFindNameContext(), FindNameOptions(FindNameOptions::kAllKinds), ident);
}

FindNameContext EvalContextImpl::GetFindNameContext() const {
  return FindNameContext(process_symbols_.get(), symbol_context_, block_.get());
}

}  // namespace zxdb
