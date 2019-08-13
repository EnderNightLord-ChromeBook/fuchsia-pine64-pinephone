// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/symbol.h"

#include "src/developer/debug/zxdb/symbols/compile_unit.h"
#include "src/developer/debug/zxdb/symbols/symbol_utils.h"
#include "src/developer/debug/zxdb/symbols/type.h"

namespace zxdb {

Symbol::Symbol() = default;
Symbol::Symbol(DwarfTag tag) : tag_(tag) {}
Symbol::~Symbol() = default;

const std::string& Symbol::GetAssignedName() const {
  const static std::string empty;
  return empty;
}

const std::string& Symbol::GetFullName() const {
  if (!full_name_)
    full_name_ = ComputeFullName();
  return *full_name_;
}

const Identifier& Symbol::GetIdentifier() const {
  if (!identifier_)
    identifier_ = ComputeIdentifier();
  return *identifier_;
}

const CompileUnit* Symbol::GetCompileUnit() const {
  // Currently we don't use compile units very often. This implementation walks up the symbol
  // hierarchy until we find one. This has the disadvantage that it decodes the tree of DIEs up to
  // here which is potentially slow, and if anything fails the path will get lost (even when we can
  // get at the unit via other means).
  //
  // The compile unit is known at the time of decode and we could just stash a pointer on each
  // symbol. This would make them larger, however, and we should take steps to ensure that the unit
  // objects are re-used so we don't get them created all over.
  //
  // Each LazySymbol also has an offset of the compile unit. But symbols don't have a LazySymbol for
  // their *own* symbol. Perhaps they should? In that case we would add a new function to the symbol
  // factory to get the unit for a LazySymbol.
  const Symbol* cur = this;
  for (;;) {
    if (const CompileUnit* unit = cur->AsCompileUnit())
      return unit;
    if (!cur->parent())
      return nullptr;
    cur = cur->parent().Get();
  }
}

DwarfLang Symbol::GetLanguage() const {
  if (const CompileUnit* unit = GetCompileUnit())
    return unit->language();
  return DwarfLang::kNone;
}

const ArrayType* Symbol::AsArrayType() const { return nullptr; }
const BaseType* Symbol::AsBaseType() const { return nullptr; }
const CodeBlock* Symbol::AsCodeBlock() const { return nullptr; }
const CompileUnit* Symbol::AsCompileUnit() const { return nullptr; }
const Collection* Symbol::AsCollection() const { return nullptr; }
const DataMember* Symbol::AsDataMember() const { return nullptr; }
const Enumeration* Symbol::AsEnumeration() const { return nullptr; }
const Function* Symbol::AsFunction() const { return nullptr; }
const FunctionType* Symbol::AsFunctionType() const { return nullptr; }
const InheritedFrom* Symbol::AsInheritedFrom() const { return nullptr; }
const MemberPtr* Symbol::AsMemberPtr() const { return nullptr; }
const ModifiedType* Symbol::AsModifiedType() const { return nullptr; }
const Namespace* Symbol::AsNamespace() const { return nullptr; }
const TemplateParameter* Symbol::AsTemplateParameter() const { return nullptr; }
const Type* Symbol::AsType() const { return nullptr; }
const Value* Symbol::AsValue() const { return nullptr; }
const Variable* Symbol::AsVariable() const { return nullptr; }
const Variant* Symbol::AsVariant() const { return nullptr; }
const VariantPart* Symbol::AsVariantPart() const { return nullptr; }

std::string Symbol::ComputeFullName() const { return GetIdentifier().GetFullName(); }

Identifier Symbol::ComputeIdentifier() const {
  const std::string& assigned_name = GetAssignedName();
  if (assigned_name.empty()) {
    // When a thing doesn't have a name, don't try to qualify it, since returning "foo::" for the
    // name of something like a lexical block is actively confusing.
    return Identifier();
  }

  // This base type class just uses the qualified name for the full name.  Derived classes will
  // override this function to apply modifiers.
  Identifier result = GetSymbolScopePrefix(this);
  result.AppendComponent(IdentifierComponent(assigned_name));
  return result;
}

}  // namespace zxdb
