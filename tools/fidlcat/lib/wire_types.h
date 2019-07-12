// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDLCAT_LIB_WIRE_TYPES_H_
#define TOOLS_FIDLCAT_LIB_WIRE_TYPES_H_

#include <lib/fit/function.h>

#ifdef __Fuchsia__
#include <zircon/types.h>
#else
typedef uint32_t zx_handle_t;
#endif

#include <sstream>
#include <string>
#include <vector>

#include "rapidjson/document.h"
#include "tools/fidlcat/lib/library_loader.h"
#include "tools/fidlcat/lib/memory_helpers.h"
#include "tools/fidlcat/lib/message_decoder.h"
#include "tools/fidlcat/lib/wire_object.h"

namespace fidlcat {

// A FIDL type.  Provides methods for generating instances of this type.
class Type {
  friend class InterfaceMethodParameter;
  friend class Library;

 public:
  Type() = default;
  virtual ~Type() = default;

  // Return true if the type is a RawType.
  virtual bool IsRaw() const { return false; }

  // Return a readable representation of the type.
  virtual std::string Name() const = 0;

  // Takes a pointer |bytes| and length of the data part |length|, and
  // returns whether that is equal to the Value represented by |value| according
  // to this type.
  virtual bool ValueEquals(const uint8_t* bytes, size_t length,
                           const rapidjson::Value& value) const;

  // Returns the size of this type when embedded in another object.
  virtual size_t InlineSize() const;

  // Decodes the type's inline part. It generates a Field and, eventually,
  // registers the field for further decoding (secondary objects).
  virtual std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                        uint64_t offset) const;

  // Gets a Type object representing the |type|.  |type| is a JSON object a
  // field "kind" that states the type (e.g., "array", "vector", "foo.bar/Baz").
  // |loader| is the set of libraries to use to find types that need to be given
  // by identifier (e.g., "foo.bar/Baz").
  static std::unique_ptr<Type> GetType(LibraryLoader* loader, const rapidjson::Value& type,
                                       size_t inline_size);

  // Gets a Type object representing the |type|.  |type| is a JSON object with a
  // "subtype" field that represents a scalar type (e.g., "float64", "uint32")
  static std::unique_ptr<Type> TypeFromPrimitive(const rapidjson::Value& type, size_t inline_size);

  // Gets a Type object representing the |type_name|.  |type| is a string that
  // represents a scalar type (e.g., "float64", "uint32").
  static std::unique_ptr<Type> ScalarTypeFromName(const std::string& type_name, size_t inline_size);

  // Gets a Type object representing the |type|.  |type| is a JSON object a
  // field "kind" that states the type.  "kind" is an identifier
  // (e.g.,"foo.bar/Baz").  |loader| is the set of libraries to use to lookup
  // that identifier.
  static std::unique_ptr<Type> TypeFromIdentifier(LibraryLoader* loader,
                                                  const rapidjson::Value& type, size_t inline_size);

  Type& operator=(const Type& other) = default;
  Type(const Type& other) = default;
};

// An instance of this class is created when the system can't determine the real
// class (e.g., in cases of corrupted metadata). Only a hexa dump is generated.
class RawType : public Type {
 public:
  RawType(size_t inline_size) : inline_size_(inline_size) {}

  bool IsRaw() const override { return true; }

  std::string Name() const override { return "unknown"; }

  virtual size_t InlineSize() const override { return inline_size_; }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;

 private:
  size_t inline_size_;
};

class StringType : public Type {
 public:
  std::string Name() const override { return "string"; }

  virtual size_t InlineSize() const override { return sizeof(uint64_t) + sizeof(uint64_t); }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;
};

// A generic type that can be used for any numeric value that corresponds to a
// C++ arithmetic value.
template <typename T>
class NumericType : public Type {
  static_assert(std::is_arithmetic<T>::value && !std::is_same<T, bool>::value,
                "NumericType can only be used for numerics");

 public:
  virtual bool ValueEquals(const uint8_t* bytes, size_t length,
                           const rapidjson::Value& value) const override {
    T lhs = internal::MemoryFrom<T, const uint8_t*>(bytes);
    std::istringstream input(value["value"].GetString());
    // Because int8_t is really char, and we don't want to read that.
    using R = typename std::conditional<std::is_same<T, int8_t>::value, int, T>::type;
    R rhs;
    input >> rhs;
    return lhs == rhs;
  }

  virtual size_t InlineSize() const override { return sizeof(T); }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override {
    return std::make_unique<NumericField<T>>(name, this, decoder->GetAddress(offset, sizeof(T)));
  }
};

class Float32Type : public NumericType<float> {
 public:
  std::string Name() const override { return "float32"; }
};

class Float64Type : public NumericType<double> {
 public:
  std::string Name() const override { return "float64"; }
};

class Int8Type : public NumericType<int8_t> {
 public:
  std::string Name() const override { return "int8"; }
};

class Int16Type : public NumericType<int16_t> {
 public:
  std::string Name() const override { return "int16"; }
};

class Int32Type : public NumericType<int32_t> {
 public:
  std::string Name() const override { return "int32"; }
};

class Int64Type : public NumericType<int64_t> {
 public:
  std::string Name() const override { return "int64"; }
};

class Uint8Type : public NumericType<uint8_t> {
 public:
  std::string Name() const override { return "uint8"; }
};

class Uint16Type : public NumericType<uint16_t> {
 public:
  std::string Name() const override { return "uint16"; }
};

class Uint32Type : public NumericType<uint32_t> {
 public:
  std::string Name() const override { return "uint32"; }
};

class Uint64Type : public NumericType<uint64_t> {
 public:
  std::string Name() const override { return "uint64"; }
};

class BoolType : public Type {
 public:
  std::string Name() const override { return "bool"; }

  virtual size_t InlineSize() const override { return sizeof(uint8_t); }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;
};

class StructType : public Type {
 public:
  StructType(const Struct& str, bool nullable) : struct_(str), nullable_(nullable) {}

  std::string Name() const override { return struct_.name(); }

  virtual size_t InlineSize() const override;

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;

 private:
  const Struct& struct_;
  const bool nullable_;
};

class TableType : public Type {
 public:
  TableType(const Table& tab) : table_(tab) {}

  std::string Name() const override { return table_.name(); }

  virtual size_t InlineSize() const override;

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;

 private:
  const Table& table_;
};

class UnionType : public Type {
 public:
  UnionType(const Union& uni, bool nullable);

  std::string Name() const override { return union_.name(); }

  virtual size_t InlineSize() const override;

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;

 private:
  const Union& union_;
  const bool nullable_;
};

class XUnionType : public Type {
 public:
  XUnionType(const XUnion& uni, bool is_nullable);

  std::string Name() const override { return xunion_.name(); }

  virtual size_t InlineSize() const override;

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;

 private:
  const XUnion& xunion_;
  const bool is_nullable_;
};

class ElementSequenceType : public Type {
 public:
  explicit ElementSequenceType(std::unique_ptr<Type>&& component_type);

 protected:
  std::unique_ptr<Type> component_type_;
};

class ArrayType : public ElementSequenceType {
 public:
  ArrayType(std::unique_ptr<Type>&& component_type, uint32_t count);

  std::string Name() const override {
    return std::string("array<") + component_type_->Name() + ">";
  }

  virtual size_t InlineSize() const override { return component_type_->InlineSize() * count_; }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;

 private:
  uint32_t count_;
};

class VectorType : public ElementSequenceType {
 public:
  VectorType(std::unique_ptr<Type>&& component_type);

  std::string Name() const override {
    return std::string("vector<") + component_type_->Name() + ">";
  }

  virtual size_t InlineSize() const override { return sizeof(uint64_t) + sizeof(uint64_t); }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;
};

class EnumType : public Type {
 public:
  EnumType(const Enum& e);

  std::string Name() const override { return enum_.name(); }

  virtual size_t InlineSize() const override { return enum_.size(); }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;

 private:
  const Enum& enum_;
};

class HandleType : public Type {
 public:
  HandleType() {}

  std::string Name() const override { return "handle"; }

  virtual size_t InlineSize() const override { return sizeof(zx_handle_t); }

  std::unique_ptr<Field> Decode(MessageDecoder* decoder, std::string_view name,
                                uint64_t offset) const override;
};

}  // namespace fidlcat

#endif  // TOOLS_FIDLCAT_LIB_WIRE_TYPES_H_
