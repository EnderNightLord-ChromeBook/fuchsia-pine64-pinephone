// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDLCAT_LIB_LIBRARY_LOADER_H_
#define TOOLS_FIDLCAT_LIB_LIBRARY_LOADER_H_

#include <iostream>
#include <map>
#include <optional>
#include <vector>

#include "rapidjson/document.h"

// This file contains a programmatic representation of a FIDL schema.  A
// LibraryLoader loads a set of Libraries.  The libraries contain structs,
// enums, interfaces, and so on.  Each element has the logic necessary to take
// wire-encoded bits of that type, and transform it to a representation of that
// type.

// A LibraryLoader object can be used to fetch a particular library or interface
// method, which can then be used for debug purposes.

// An example of building a LibraryLoader can be found in
// library_loader_test.cc:LoadSimple. Callers can then do something like the
// following, if they have a fidl::Message:
//
// fidl_message_header_t header = message.header();
// const std::vector<const InterfaceMethod*>* methods = loader_->GetByOrdinal(header.ordinal);
// rapidjson::Document actual;
// fidlcat::RequestToJSON(methods->at(0), message, actual);
//
// |actual| will then contain the contents of the message in JSON
// (human-readable) format.
//
// These libraries are currently thread-unsafe.

namespace fidlcat {

typedef uint32_t Ordinal32;
typedef uint64_t Ordinal64;

struct LibraryReadError {
  enum ErrorValue {
    kOk,
    kIoError,
    kParseError,
  };
  ErrorValue value;
  rapidjson::ParseResult parse_result;
};

class Interface;
class InterfaceMethod;
class Library;
class LibraryLoader;
class MessageDecoder;
class Object;
class Struct;
class Table;
class Type;
class Union;
class UnionField;
class XUnion;

class Enum {
 public:
  friend class Library;

  ~Enum();

  const std::string& name() const { return name_; }
  uint64_t size() const { return size_; }
  const Type* type() const { return type_.get(); }

  // Gets the name of the enum member corresponding to the value pointed to by
  // |bytes| of length |length|.  For example, if we had the following
  // definition:
  // enum i16_enum : int16 {
  //   x = -23;
  // };
  // and you pass |bytes| a 2-byte representation of -23, and |length| 2, this
  // function will return "x".  Returns "(Unknown enum member)" if it can't find
  // the member.
  std::string GetNameFromBytes(const uint8_t* bytes) const;

 private:
  explicit Enum(const rapidjson::Value& value);

  // Decode all the values from the JSON definition.
  void DecodeTypes();

  const rapidjson::Value& value_;
  bool decoded_ = false;
  std::string name_;
  uint64_t size_;
  std::unique_ptr<Type> type_;
};

// TODO: Consider whether this is duplicative of Struct / Table member.
class UnionMember {
 public:
  UnionMember(const Library& enclosing_library, const rapidjson::Value& value);
  ~UnionMember();

  std::string_view name() const { return name_; }
  uint64_t size() const { return size_; }
  uint64_t offset() const { return offset_; }
  Ordinal32 ordinal() const { return ordinal_; }
  const Type* type() const { return type_.get(); }

 private:
  const std::string name_;
  const uint64_t offset_;
  const uint64_t size_;
  const Ordinal32 ordinal_;
  std::unique_ptr<Type> type_;
};

class Union {
 public:
  friend class Library;
  friend class XUnion;

  const Library& enclosing_library() const { return enclosing_library_; }
  const std::string& name() const { return name_; }
  uint64_t alignment() const { return alignment_; }
  uint32_t size() const { return size_; }
  const std::vector<std::unique_ptr<UnionMember>>& members() const { return members_; }

  const UnionMember* MemberWithTag(uint32_t tag) const;

  const UnionMember* MemberWithOrdinal(Ordinal32 ordinal) const;

  std::unique_ptr<UnionField> DecodeUnion(MessageDecoder* decoder, std::string_view name,
                                          const Type* type, uint64_t offset, bool nullable) const;

 private:
  Union(const Library& enclosing_library, const rapidjson::Value& value);

  // Decode all the values from the JSON definition.
  void DecodeTypes();

  const Library& enclosing_library_;
  const rapidjson::Value& value_;
  bool decoded_ = false;
  std::string name_;
  uint64_t alignment_;
  uint64_t size_;
  std::vector<std::unique_ptr<UnionMember>> members_;
};

class XUnion : public Union {
 public:
  friend class Library;

 private:
  XUnion(const Library& enclosing_library, const rapidjson::Value& value)
      : Union(enclosing_library, value) {}
};

class StructMember {
 public:
  StructMember(const Library& enclosing_library, const rapidjson::Value& value);
  ~StructMember();

  std::string_view name() const { return name_; }
  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }
  const Type* type() const { return type_.get(); }

 private:
  const std::string name_;
  const uint64_t offset_;
  const uint64_t size_;
  std::unique_ptr<Type> type_;
};

class Struct {
 public:
  friend class Library;
  friend class InterfaceMethod;

  const Library& enclosing_library() const { return enclosing_library_; }
  const std::string& name() const { return name_; }
  uint32_t size() const { return size_; }
  const std::vector<std::unique_ptr<StructMember>>& members() const { return members_; }

  std::unique_ptr<Object> DecodeObject(MessageDecoder* decoder, std::string_view name,
                                       const Type* type, uint64_t offset, bool nullable) const;

 private:
  Struct(const Library& enclosing_library, const rapidjson::Value& value);

  // Decode all the values from the JSON definition if the object represents a
  // structure.
  void DecodeStructTypes();

  // Decode all the values from the JSON definition if the object represents a
  // request message.
  void DecodeRequestTypes();

  // Decode all the values from the JSON definition if the object represents a
  // response message.
  void DecodeResponseTypes();

  // Decode all the values from the JSON definition.
  void DecodeTypes(std::string size_name, std::string member_name);

  const Library& enclosing_library_;
  const rapidjson::Value& value_;
  bool decoded_ = false;
  std::string name_;
  uint32_t size_ = 0;
  std::vector<std::unique_ptr<StructMember>> members_;
};

class TableMember {
 public:
  TableMember(const Library& enclosing_library, const rapidjson::Value& value);
  ~TableMember();

  const std::string_view name() const { return name_; }
  Ordinal32 ordinal() const { return ordinal_; }
  uint64_t size() const { return size_; }
  const Type* type() const { return type_.get(); }

 private:
  const std::string name_;
  const Ordinal32 ordinal_;
  const uint64_t size_;
  std::unique_ptr<Type> type_;
};

class Table {
 public:
  friend class Library;

  ~Table();

  const Library& enclosing_library() const { return enclosing_library_; }
  const std::string& name() const { return name_; }
  uint32_t size() const { return size_; }
  const Type* unknown_member_type() const { return unknown_member_type_.get(); }

  // Returns a vector of pointers to the table's members.  The ordinal of each
  // member is its index in the vector.  Omitted ordinals are indicated by
  // nullptr.  Also, note that ordinal 0 is disallowed, so element 0 is always
  // nullptr.
  const std::vector<const TableMember*>& members() const { return members_; }

 private:
  Table(const Library& enclosing_library, const rapidjson::Value& value);

  // Decode all the values from the JSON definition.
  void DecodeTypes();

  const Library& enclosing_library_;
  const rapidjson::Value& value_;
  bool decoded_ = false;
  std::string name_;
  uint64_t size_;
  std::unique_ptr<Type> unknown_member_type_;

  // This indirection - elements of members_ pointing to elements of
  // backing_members_ - is so that we can have empty members.  The author
  // thought that use sites would be more usable than a map.
  // These structures are not modified after the constructor.
  std::vector<const TableMember*> members_;
  std::vector<std::unique_ptr<TableMember>> backing_members_;
};

class InterfaceMethod {
 public:
  friend class Interface;

  const Interface& enclosing_interface() const { return enclosing_interface_; }
  Ordinal64 ordinal() const { return ordinal_; }
  Ordinal64 old_ordinal() const { return old_ordinal_; }
  bool is_composed() const { return is_composed_; }
  std::string name() const { return name_; }
  Struct* request() const {
    if (request_ != nullptr) {
      request_->DecodeRequestTypes();
    }
    return request_.get();
  }
  Struct* response() const {
    if (response_ != nullptr) {
      response_->DecodeResponseTypes();
    }
    return response_.get();
  }

  std::string fully_qualified_name() const;

  InterfaceMethod(const InterfaceMethod& other) = delete;
  InterfaceMethod& operator=(const InterfaceMethod&) = delete;

 private:
  InterfaceMethod(const Interface& interface, const rapidjson::Value& value);

  const Interface& enclosing_interface_;
  const rapidjson::Value& value_;
  const Ordinal64 ordinal_;
  const Ordinal64 old_ordinal_;
  const bool is_composed_;
  const std::string name_;
  std::unique_ptr<Struct> request_;
  std::unique_ptr<Struct> response_;
};

class Interface {
 public:
  friend class Library;

  Interface(const Interface& other) = delete;
  Interface& operator=(const Interface&) = delete;

  const Library& enclosing_library() const { return enclosing_library_; }
  std::string_view name() const { return name_; }

  void AddMethodsToIndex(
      std::map<Ordinal64, std::unique_ptr<std::vector<const InterfaceMethod*>>>& index) {
    for (size_t i = 0; i < interface_methods_.size(); i++) {
      const InterfaceMethod* method = interface_methods_[i].get();
      // TODO(FIDL-524): At various steps of the migration, the ordinals may be
      // the same value. Avoid creating duplicate entries.
      bool ords_are_same = method->ordinal() == method->old_ordinal();
      if (index[method->ordinal()] == nullptr) {
        index[method->ordinal()] = std::make_unique<std::vector<const InterfaceMethod*>>();
        if (!ords_are_same)
          index[method->old_ordinal()] = std::make_unique<std::vector<const InterfaceMethod*>>();
      }
      // Ensure composed methods come after non-composed methods.  The fidlcat
      // libraries pick the first one they find.
      if (method->is_composed()) {
        index[method->ordinal()]->push_back(method);
        if (!ords_are_same)
          index[method->old_ordinal()]->push_back(method);
      } else {
        index[method->ordinal()]->insert(index[method->ordinal()]->begin(), method);
        if (!ords_are_same)
          index[method->old_ordinal()]->insert(index[method->old_ordinal()]->begin(), method);
      }
    }
  }

  // Sets *|method| to the fully qualified |name|'s InterfaceMethod
  bool GetMethodByFullName(const std::string& name, const InterfaceMethod** method) const;

  const std::vector<std::unique_ptr<InterfaceMethod>>& methods() const {
    return interface_methods_;
  }

 private:
  Interface(const Library& library, const rapidjson::Value& value)
      : enclosing_library_(library), name_(value["name"].GetString()) {
    for (auto& method : value["methods"].GetArray()) {
      interface_methods_.emplace_back(new InterfaceMethod(*this, method));
    }
  }

  const Library& enclosing_library_;
  std::string name_;
  std::vector<std::unique_ptr<InterfaceMethod>> interface_methods_;
};

class Library {
 public:
  friend class LibraryLoader;

  LibraryLoader* enclosing_loader() const { return enclosing_loader_; }
  const std::string name() { return name_; }
  const std::vector<std::unique_ptr<Interface>>& interfaces() const { return interfaces_; }

  std::unique_ptr<Type> TypeFromIdentifier(bool is_nullable, std::string& identifier,
                                           size_t inline_size);

  // The size of the type with name |identifier| when it is inline (e.g.,
  // embedded in an array)
  size_t InlineSizeFromIdentifier(std::string& identifier) const;

  // Set *ptr to the Interface called |name|
  bool GetInterfaceByName(const std::string& name, const Interface** ptr) const;

  Library& operator=(const Library&) = delete;
  Library(const Library&) = delete;

 private:
  Library(LibraryLoader* enclosing_loader, rapidjson::Document& document,
          std::map<Ordinal64, std::unique_ptr<std::vector<const InterfaceMethod*>>>& index);

  // Decode all the values from the JSON definition.
  void DecodeTypes();

  LibraryLoader* enclosing_loader_;
  rapidjson::Document backing_document_;
  bool decoded_ = false;
  std::string name_;
  std::vector<std::unique_ptr<Interface>> interfaces_;
  std::map<std::string, std::unique_ptr<Enum>> enums_;
  std::map<std::string, std::unique_ptr<Struct>> structs_;
  std::map<std::string, std::unique_ptr<Table>> tables_;
  std::map<std::string, std::unique_ptr<Union>> unions_;
  std::map<std::string, std::unique_ptr<XUnion>> xunions_;
};

// An indexed collection of libraries.
// WARNING: All references on Enum, Struct, Table, ... and all references on
//          types and fields must be destroyed before this class (LibraryLoader
//          should be one of the last objects we destroy).
class LibraryLoader {
 public:
  LibraryLoader(std::vector<std::unique_ptr<std::istream>>& library_streams, LibraryReadError* err);

  LibraryLoader& operator=(const LibraryLoader&) = delete;
  LibraryLoader(const LibraryLoader&) = delete;

  // Returns a pointer to a set of methods that have this ordinal.  There may be
  // more than one if the method was composed into multiple protocols.  For
  // convenience, the methods that are not composed are at the front of the
  // vector.  Returns |nullptr| if there is no such method.  The returned
  // pointer continues to be owned by the LibraryLoader, and should not be
  // deleted.
  const std::vector<const InterfaceMethod*>* GetByOrdinal(Ordinal64 ordinal) {
    auto m = ordinal_map_.find(ordinal);
    if (m != ordinal_map_.end()) {
      return m->second.get();
    }
    return nullptr;
  }

  // If the library with name |name| is present in this loader, returns the
  // library. Otherwise, returns null.
  // |name| is of the format "a.b.c"
  Library* GetLibraryFromName(const std::string& name) {
    auto l = representations_.find(name);
    if (l != representations_.end()) {
      Library* library = l->second.get();
      library->DecodeTypes();
      return library;
    }
    return nullptr;
  }

 private:
  void Add(std::string& ir, LibraryReadError* err) {
    rapidjson::Document document;
    err->parse_result = document.Parse<rapidjson::kParseNumbersAsStringsFlag>(ir.c_str());
    // TODO: This would be a good place to validate that the resulting JSON
    // matches the schema in zircon/tools/fidl/schema.json.  If there are
    // errors, we will currently get mysterious crashes.
    if (document.HasParseError()) {
      err->value = LibraryReadError::kParseError;
      return;
    }
    std::string library_name = document["name"].GetString();
    representations_.emplace(std::piecewise_construct, std::forward_as_tuple(library_name),
                             std::forward_as_tuple(new Library(this, document, ordinal_map_)));
  }

  std::map<std::string, std::unique_ptr<Library>> representations_;
  std::map<Ordinal64, std::unique_ptr<std::vector<const InterfaceMethod*>>> ordinal_map_;
};

}  // namespace fidlcat

#endif  // TOOLS_FIDLCAT_LIB_LIBRARY_LOADER_H_
