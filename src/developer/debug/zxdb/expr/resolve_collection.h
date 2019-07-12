// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_EXPR_RESOLVE_COLLECTION_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_EXPR_RESOLVE_COLLECTION_H_

#include <functional>
#include <optional>
#include <string>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/expr/found_member.h"
#include "src/developer/debug/zxdb/expr/found_name.h"
#include "src/developer/debug/zxdb/expr/parsed_identifier.h"
#include "src/lib/fxl/memory/ref_ptr.h"

namespace zxdb {

class CodeBlock;
class DataMember;
class EvalContext;
class ExprValue;
class InheritedFrom;

// Resolves a DataMember given a collection (class/struct/union) and a record for a variable within
// that collection. The data member must be on the class itself, not on a base class.
//
// Returns an error on failure, or puts the result in |out| on success.
//
// The DataMember may be null. If so, returns an error (this is so callers don't have to type check
// the inputs).
Err ResolveMember(fxl::RefPtr<EvalContext> context, const ExprValue& base, const DataMember* member,
                  ExprValue* out);

// Resolves a DataMember by name. This variant searches base classes for name matches.
//
// Returns an error if the name isn't found.
Err ResolveMember(fxl::RefPtr<EvalContext> context, const ExprValue& base,
                  const ParsedIdentifier& identifier, ExprValue* out);

// The variant takes an ExprValue which is a pointer to the base/struct or class. Because it fetches
// memory it is asynchronous.
void ResolveMemberByPointer(fxl::RefPtr<EvalContext> context, const ExprValue& base_ptr,
                            const FoundMember& found_member,
                            std::function<void(const Err&, ExprValue)> cb);

// Same as previous version but takes the name of the member to find. The callback also provides the
// DataMember corresponding to what the name matched.
void ResolveMemberByPointer(fxl::RefPtr<EvalContext> context, const ExprValue& base_ptr,
                            const ParsedIdentifier& identifier,
                            std::function<void(const Err&, fxl::RefPtr<DataMember>, ExprValue)> cb);

// Takes a Collection value and a base class inside of it, computes the value of the base class and
// puts it in *out.
//
// For the version that takes an InheritedFrom, the base class must be a direct base class of the
// "value" collection, not an indirect base.
//
// For the version that takes a type and an offset, the type must already have been computed as some
// type of base class that lives at the given offset. It need not be a direct base and no type
// checking is done as long as the offsets and sizes are valid.
Err ResolveInherited(const ExprValue& value, const InheritedFrom* from, ExprValue* out);
Err ResolveInherited(const ExprValue& value, fxl::RefPtr<Type> base_type, uint64_t offset,
                     ExprValue* out);

// Verifies that |input| type is a pointer to a collection and fills the pointed-to type into
// |*pointed_to|. In other cases, returns an error. The input type can be null (which will produce
// an error) or non-concrete (const, forward definition, etc.) so the caller doesn't have to check.
//
// The returned type will be concrete which means the type may be modified to strip CV qualifiers.
// This is used when looking up collection members by pointer so this is needed. It should not be
// used to generate types that might be visible to the user (they'll want the qualifiers).
Err GetConcretePointedToCollection(const fxl::RefPtr<EvalContext>& eval_context, const Type* input,
                                   fxl::RefPtr<Collection>* pointed_to);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_EXPR_RESOLVE_COLLECTION_H_
