// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_CPP_STRING_H_
#define LIB_FIDL_CPP_STRING_H_

#include <lib/fidl/cpp/builder.h>
#include <lib/fidl/cpp/string_view.h>
#include <zircon/assert.h>

#include <iosfwd>
#include <string>
#include <utility>

#include "lib/fidl/cpp/coding_traits.h"
#include "lib/fidl/cpp/traits.h"

namespace fidl {

// A representation of a FIDL string that owns the memory for the string.
//
// A StringPtr has three states: (1) null, (2) empty, (3) contains a string. In
// the second state, operations that return an std::string return the empty
// std::string. The null and empty states can be distinguished using the
// |is_null| and |operator bool| methods.
class StringPtr final {
 public:
  StringPtr() = default;
  StringPtr(const StringPtr& other) = default;
  StringPtr(StringPtr&& other) noexcept = default;
  ~StringPtr() = default;

  StringPtr& operator=(const StringPtr&) = default;
  StringPtr& operator=(StringPtr&& other) = default;

  StringPtr(std::string str) : str_(std::move(str)), is_null_if_empty_(false) {}
  StringPtr(const char* str)
      : str_(str ? std::string(str) : std::string()), is_null_if_empty_(!str) {}
  StringPtr(const char* str, size_t length)
      : str_(str ? std::string(str, length) : std::string()), is_null_if_empty_(!str) {}

  // Accesses the underlying std::string object.
  const std::string& get() const { return str_; }

  // Stores the given std::string in this StringPtr.
  //
  // After this method returns, the StringPtr is non-null.
  void reset(std::string str) {
    str_ = std::move(str);
    is_null_if_empty_ = false;
  }

  // Causes this StringPtr to become null.
  void reset() {
    str_.clear();
    is_null_if_empty_ = true;
  }

  void swap(StringPtr& other) {
    using std::swap;
    swap(str_, other.str_);
    swap(is_null_if_empty_, other.is_null_if_empty_);
  }

  // Whether this StringPtr is null.
  //
  // The null state is separate from the empty state.
  bool is_null() const { return is_null_if_empty_ && str_.empty(); }

  // Tests as true if non-null, false if null.
  explicit operator bool() const { return !is_null(); }

  // Provides access to the underlying std::string.
  std::string* operator->() { return &str_; }
  const std::string* operator->() const { return &str_; }

  // Provides access to the underlying std::string.
  const std::string& operator*() const { return str_; }

  operator const std::string&() const { return str_; }

  void Encode(Encoder* encoder, size_t offset);
  static void Decode(Decoder* decoder, StringPtr* value, size_t offset);

  template <class EncoderImpl>
  static void EncodeString(EncoderImpl* encoder, const std::string& value, size_t offset) {
    fidl_string_t* string = encoder->template GetPtr<fidl_string_t>(offset);
    string->size = value.size();
    string->data = reinterpret_cast<char*>(FIDL_ALLOC_PRESENT);
    size_t base = encoder->Alloc(value.size());
    char* payload = encoder->template GetPtr<char>(base);
    memcpy(payload, value.data(), value.size());
  }

  template <class DecoderImpl>
  static void DecodeString(DecoderImpl* decoder, std::string* value, size_t offset) {
    fidl_string_t* string = decoder->template GetPtr<fidl_string_t>(offset);
    ZX_ASSERT(string->data != nullptr);
    *value = std::string(string->data, string->size);
  }

 private:
  std::string str_;
  bool is_null_if_empty_ = true;
};

template <>
struct Equality<StringPtr> {
  static inline bool Equals(const StringPtr& a, const StringPtr& b) {
    if (a.is_null()) {
      return b.is_null();
    }
    return !b.is_null() && a.get() == b.get();
  }
};

inline bool operator==(const StringPtr& a, const StringPtr& b) {
  if (a.is_null()) {
    return b.is_null();
  }
  return !b.is_null() && a.get() == b.get();
}

inline bool operator==(const char* a, const StringPtr& b) {
  if (a == nullptr) {
    return b.is_null();
  }
  return !b.is_null() && a == b.get();
}

inline bool operator==(const StringPtr& a, const char* b) {
  if (a.is_null()) {
    return b == nullptr;
  }
  return b != nullptr && a.get() == b;
}

inline bool operator!=(const StringPtr& a, const StringPtr& b) { return !(a == b); }

inline bool operator!=(const char* a, const StringPtr& b) { return !(a == b); }

inline bool operator!=(const StringPtr& a, const char* b) { return !(a == b); }

inline bool operator<(const StringPtr& a, const StringPtr& b) {
  if (a.is_null() || b.is_null()) {
    return !b.is_null();
  }
  return *a < *b;
}

inline bool operator<(const char* a, const StringPtr& b) {
  if (a == nullptr || b.is_null()) {
    return !b.is_null();
  }
  return a < *b;
}

inline bool operator<(const StringPtr& a, const char* b) {
  if (a.is_null() || b == nullptr) {
    return b != nullptr;
  }
  return *a < b;
}

inline bool operator>(const StringPtr& a, const StringPtr& b) {
  if (a.is_null() || b.is_null()) {
    return !a.is_null();
  }
  return *a > *b;
}

inline bool operator>(const char* a, const StringPtr& b) {
  if (a == nullptr || b.is_null()) {
    return a != nullptr;
  }
  return a > *b;
}

inline bool operator>(const StringPtr& a, const char* b) {
  if (a.is_null() || b == nullptr) {
    return a != nullptr;
  }
  return *a > b;
}

inline bool operator<=(const StringPtr& a, const StringPtr& b) { return !(a > b); }

inline bool operator<=(const char* a, const StringPtr& b) { return !(a > b); }

inline bool operator<=(const StringPtr& a, const char* b) { return !(a > b); }

inline bool operator>=(const StringPtr& a, const StringPtr& b) { return !(a < b); }

inline bool operator>=(const char* a, const StringPtr& b) { return !(a < b); }

inline bool operator>=(const StringPtr& a, const char* b) { return !(a < b); }

inline std::ostream& operator<<(std::ostream& out, const StringPtr& str) {
  return out << str.get();
}

template <>
struct CodingTraits<StringPtr> : public EncodableCodingTraits<StringPtr, sizeof(fidl_string_t)> {};

template <>
struct CodingTraits<::std::string> {
  static constexpr size_t encoded_size = sizeof(fidl_string_t);
  template <class EncoderImpl>
  static void Encode(EncoderImpl* encoder, std::string* value, size_t offset) {
    ::fidl::StringPtr::EncodeString(encoder, *value, offset);
  }
  template <class DecoderImpl>
  static void Decode(DecoderImpl* decoder, std::string* value, size_t offset) {
    ::fidl::StringPtr::DecodeString(decoder, value, offset);
  }
};

}  // namespace fidl

#endif  // LIB_FIDL_CPP_STRING_H_
