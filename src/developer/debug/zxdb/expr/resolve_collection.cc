// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/resolve_collection.h"

#include "src/developer/debug/zxdb/expr/eval_context.h"
#include "src/developer/debug/zxdb/expr/expr_value.h"
#include "src/developer/debug/zxdb/expr/find_name.h"
#include "src/developer/debug/zxdb/expr/resolve_ptr_ref.h"
#include "src/developer/debug/zxdb/symbols/arch.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/data_member.h"
#include "src/developer/debug/zxdb/symbols/function.h"
#include "src/developer/debug/zxdb/symbols/identifier.h"
#include "src/developer/debug/zxdb/symbols/inherited_from.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/symbol_data_provider.h"
#include "src/developer/debug/zxdb/symbols/variable.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

// A wrapper around FindMember that issues errors rather than returning an optional. The base can be
// null for the convenience of the caller. On error, the output FoundMember will be untouched.
Err FindMemberWithErr(const Collection* base, const ParsedIdentifier& identifier,
                      FoundMember* out) {
  if (!base) {
    return Err("Can't resolve '%s' on non-struct/class/union value.",
               identifier.GetFullName().c_str());
  }

  FindNameOptions options(FindNameOptions::kNoKinds);
  options.find_vars = true;

  std::vector<FoundName> found;
  FindMember(FindNameContext(), options, base, identifier, nullptr, &found);
  if (!found.empty()) {
    FXL_DCHECK(found[0].kind() == FoundName::kMemberVariable);
    *out = found[0].member();
    return Err();
  }

  return Err("No member '%s' in %s '%s'.", identifier.GetFullName().c_str(), base->GetKindString(),
             base->GetFullName().c_str());
}

Err GetErrorForInvalidMemberOf(const Collection* coll) {
  return Err("Invalid data member for %s '%s'.", coll->GetKindString(),
             coll->GetFullName().c_str());
}

// Tries to describe the type of the value as best as possible when a member access is invalid.
Err GetErrorForInvalidMemberOf(const ExprValue& value) {
  if (!value.type())
    return Err("No type information.");

  if (const Collection* coll = value.type()->AsCollection())
    return GetErrorForInvalidMemberOf(coll);

  // Something other than a collection is the base.
  return Err("Accessing a member of non-struct/class/union '%s'.",
             value.type()->GetFullName().c_str());
}

// Validates the input member (it will null check) and extracts the type for the member.
Err GetMemberType(const Collection* coll, const DataMember* member,
                  fxl::RefPtr<Type>* member_type) {
  if (!member)
    return GetErrorForInvalidMemberOf(coll);

  *member_type = RefPtrTo(member->type().Get()->AsType());
  if (!*member_type) {
    return Err("Bad type information for '%s.%s'.", coll->GetFullName().c_str(),
               member->GetAssignedName().c_str());
  }
  return Err();
}

void DoResolveMemberByPointer(fxl::RefPtr<EvalContext> context, const ExprValue& base_ptr,
                              const Collection* pointed_to_type, const FoundMember& member,
                              std::function<void(const Err&, ExprValue)> cb) {
  Err err = base_ptr.EnsureSizeIs(kTargetPointerSize);
  if (err.has_error()) {
    cb(err, ExprValue());
    return;
  }

  fxl::RefPtr<Type> member_type;
  err = GetMemberType(pointed_to_type, member.data_member(), &member_type);
  if (err.has_error()) {
    cb(err, ExprValue());
    return;
  }

  TargetPointer base_address = base_ptr.GetAs<TargetPointer>();
  ResolvePointer(context, base_address + member.data_member_offset(), std::move(member_type),
                 std::move(cb));
}

// Extracts an embedded type inside of a base. This can be used for finding collection data members
// and inherited classes, both of which consist of a type and an offset.
Err ExtractSubType(const ExprValue& base, fxl::RefPtr<Type> sub_type, uint32_t offset,
                   ExprValue* out) {
  uint32_t size = sub_type->byte_size();
  if (offset + size > base.data().size())
    return GetErrorForInvalidMemberOf(base);
  std::vector<uint8_t> member_data(base.data().begin() + offset,
                                   base.data().begin() + (offset + size));

  *out =
      ExprValue(std::move(sub_type), std::move(member_data), base.source().GetOffsetInto(offset));
  return Err();
}

// This variant takes a precomputed offset of the data member in the base class. This is to support
// the case where the data member is in a derived class (the derived class will have its own
// offset).
Err DoResolveMember(fxl::RefPtr<EvalContext> context, const ExprValue& base,
                    const FoundMember& member, ExprValue* out) {
  fxl::RefPtr<Type> concrete_type = base.GetConcreteType(context.get());
  const Collection* coll = nullptr;
  if (!base.type() || !(coll = concrete_type->AsCollection()))
    return Err("Can't resolve data member on non-struct/class value.");

  fxl::RefPtr<Type> member_type;
  Err err = GetMemberType(coll, member.data_member(), &member_type);
  if (err.has_error())
    return err;

  return ExtractSubType(base, std::move(member_type), member.data_member_offset(), out);
}

}  // namespace

Err ResolveMember(fxl::RefPtr<EvalContext> context, const ExprValue& base, const DataMember* member,
                  ExprValue* out) {
  if (!member)
    return GetErrorForInvalidMemberOf(base);
  return DoResolveMember(context, base, FoundMember(member, member->member_location()), out);
}

Err ResolveMember(fxl::RefPtr<EvalContext> context, const ExprValue& base,
                  const ParsedIdentifier& identifier, ExprValue* out) {
  fxl::RefPtr<Type> concrete_type = base.GetConcreteType(context.get());
  if (!concrete_type)
    return Err("No type information.");

  FoundMember found;
  Err err = FindMemberWithErr(concrete_type->AsCollection(), identifier, &found);
  if (err.has_error())
    return err;
  return DoResolveMember(context, base, found, out);
}

void ResolveMemberByPointer(fxl::RefPtr<EvalContext> context, const ExprValue& base_ptr,
                            const FoundMember& found_member,
                            std::function<void(const Err&, ExprValue)> cb) {
  fxl::RefPtr<Collection> pointed_to;
  Err err = GetConcretePointedToCollection(context, base_ptr.type(), &pointed_to);
  if (err.has_error())
    return cb(err, ExprValue());

  DoResolveMemberByPointer(context, base_ptr, pointed_to.get(), found_member, std::move(cb));
}

void ResolveMemberByPointer(
    fxl::RefPtr<EvalContext> context, const ExprValue& base_ptr, const ParsedIdentifier& identifier,
    std::function<void(const Err&, fxl::RefPtr<DataMember>, ExprValue)> cb) {
  fxl::RefPtr<Collection> coll;
  if (Err err = GetConcretePointedToCollection(context, base_ptr.type(), &coll); err.has_error())
    return cb(err, nullptr, ExprValue());

  FoundMember found_member;
  if (Err err = FindMemberWithErr(coll.get(), identifier, &found_member); err.has_error())
    return cb(err, nullptr, ExprValue());

  DoResolveMemberByPointer(
      context, base_ptr, coll.get(), found_member,
      [cb = std::move(cb),
       member_ref = RefPtrTo(found_member.data_member())](
          const Err& err, ExprValue value) { cb(err, std::move(member_ref), std::move(value)); });
}

Err ResolveInherited(const ExprValue& value, const InheritedFrom* from, ExprValue* out) {
  const Type* from_type = from->from().Get()->AsType();
  if (!from_type)
    return GetErrorForInvalidMemberOf(value);

  return ExtractSubType(value, RefPtrTo(from_type), from->offset(), out);
}

Err ResolveInherited(const ExprValue& value, fxl::RefPtr<Type> base_type, uint64_t offset,
                     ExprValue* out) {
  return ExtractSubType(value, std::move(base_type), offset, out);
}

Err GetConcretePointedToCollection(const fxl::RefPtr<EvalContext>& eval_context, const Type* input,
                                   fxl::RefPtr<Collection>* pointed_to) {
  fxl::RefPtr<Type> to_type;
  if (Err err = GetPointedToType(eval_context, input, &to_type); err.has_error())
    return err;
  to_type = eval_context->GetConcreteType(to_type.get());

  if (const Collection* collection = to_type->AsCollection()) {
    *pointed_to = fxl::RefPtr<Collection>(const_cast<Collection*>(collection));
    return Err();
  }

  return Err("Attempting to dereference a pointer to '%s' which is not a class, struct, or union.",
             to_type->GetFullName().c_str());
}

}  // namespace zxdb
