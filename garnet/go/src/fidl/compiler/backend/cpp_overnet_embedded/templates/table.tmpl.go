// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Table = `
{{- define "TableForwardDeclaration" }}
class {{ .Name }};
{{- end }}

{{- define "TableDeclaration" }}
{{range .DocComments}}
//{{ . }}
{{- end}}
class {{ .Name }}  {
 public:
  static const fidl_type_t* FidlType;
  {{- range .Members }}
  {{range .DocComments}}
  //{{ . }}
  {{- end}}
  const {{ .Type.OvernetEmbeddedDecl }}& {{ .Name }}() const {
    assert({{ .FieldPresenceName }});
    return {{ .FieldDataName }}.value;
  }
  bool {{ .MethodHasName }}() const {
    return {{ .FieldPresenceName }};
  }
  {{range .DocComments}}
  //{{ . }}
  {{- end}}
  {{ .Type.OvernetEmbeddedDecl }}* mutable_{{ .Name }}() {
    if (!{{ .FieldPresenceName }}) {
      {{ .FieldPresenceName }} = true;
      Construct(&{{ .FieldDataName }}.value);
    }
    return &{{ .FieldDataName }}.value;
  }
  void set_{{ .Name }}({{ .Type.OvernetEmbeddedDecl }} _value) {
    if (!{{ .FieldPresenceName }}) {
      {{ .FieldPresenceName }} = true;
      Construct(&{{ .FieldDataName }}.value, std::move(_value));
    } else {
      {{ .FieldDataName }}.value = std::move(_value);
    }
  }
  void {{ .MethodClearName }}() {
    if (!{{ .FieldPresenceName }}) {
      return;
    }
    {{ .FieldPresenceName }} = false;
    Destruct(&{{ .FieldDataName }}.value);
  }
  {{- end }}

  {{ .Name }}();
  {{ .Name }}({{ .Name }}&& other);
  ~{{ .Name }}();
  {{ .Name }}& operator=({{ .Name }}&& other);

  static inline ::std::unique_ptr<{{ .Name }}> New() { return ::std::make_unique<{{ .Name }}>(); }

  void Encode(::overnet::internal::Encoder* _encoder, size_t _offset);
  static void Decode(::overnet::internal::Decoder* _decoder, {{ .Name }}* _value, size_t _offset);
  zx_status_t Clone({{ .Name }}* _result) const;

 private:
  template <class T, class... Args>
  void Construct(T* p, Args&&... args) {
    new (p) T(std::forward<Args>(args)...);
  }

  template <class T>
  void Destruct(T* p) {
    p->~T();
  }

  {{- range .Members }}
  bool {{ .FieldPresenceName }} : 1;
  {{- end }}

  {{- range .Members }}
  {{/* The raw values of a table field are placed inside a union to ensure
       that they're not initialized (since table fields are optional by
       default). Placement new must be used to initialize the value. */ -}}
  union {{ .ValueUnionName }} {
    {{ .ValueUnionName }}() {}
    ~{{ .ValueUnionName }}() {}

    {{ .Type.OvernetEmbeddedDecl }} value;
  };
  {{ .ValueUnionName }} {{ .FieldDataName }};
  {{- end }}
};


using {{ .Name }}Ptr = ::std::unique_ptr<{{ .Name }}>;
{{- end }}

{{- define "TableDefinition" }}
extern "C" const fidl_type_t {{ .TableType }};
const fidl_type_t* {{ .Name }}::FidlType = &{{ .TableType }};

{{ .Name }}::{{ .Name }}(){{- if len .Members }} : {{- end -}}
{{- range $index, $element := .Members -}}
  {{ if $index }},{{ end }}
  {{ $element.FieldPresenceName }}(false)
{{- end }} {
}

{{ .Name }}::{{ .Name }}({{ .Name }}&& other) {
  {{- range .Members }}
  {{ .FieldPresenceName }} = other.{{ .FieldPresenceName }};
  if ({{ .FieldPresenceName }}) {
    Construct(&{{ .FieldDataName }}.value, std::move(other.{{ .FieldDataName }}.value));
  }
  {{- end }}
}

{{ .Name }}::~{{ .Name }}() {
  {{- range .Members }}
  if ({{ .FieldPresenceName }}) {
    Destruct(&{{ .FieldDataName }}.value);
  }
  {{- end }}
}

{{ .Name }}& {{ .Name }}::operator=({{ .Name }}&& other) {
  {{- range .Members }}
  if (other.{{ .FieldPresenceName }}) {
    if ({{ .FieldPresenceName }}) {
      {{ .FieldDataName }}.value = std::move(other.{{ .FieldDataName }}.value);
    } else {
      {{ .FieldPresenceName }} = true;
      Construct(&{{ .FieldDataName }}.value, std::move(other.{{ .FieldDataName }}.value));
    }
  } else if ({{ .FieldPresenceName }}) {
    {{ .FieldPresenceName }} = false;
    Destruct(&{{ .FieldDataName }}.value);
  }
  {{- end }}
  return *this;
}

void {{ .Name }}::Encode(::overnet::internal::Encoder* _encoder, size_t _offset) {
  size_t max_ordinal = 0;
  {{- range .Members }}
  if ({{ .FieldPresenceName }}) max_ordinal = {{ .Ordinal }};
  {{- end }}
  ::fidl::EncodeVectorPointer(_encoder, max_ordinal, _offset);
  if (max_ordinal == 0) return;
  {{- if len .Members }}
  size_t base = _encoder->Alloc(max_ordinal * 2 * sizeof(uint64_t));
  {{- end }}
  {{- range .Members }}
  if ({{ .FieldPresenceName }}) {
    const size_t length_before = _encoder->CurrentLength();
    const size_t handles_before = _encoder->CurrentHandleCount();
    ::fidl::Encode(
        _encoder,
        &{{ .FieldDataName }}.value,
        _encoder->Alloc(::fidl::CodingTraits<{{ .Type.OvernetEmbeddedDecl }}>::encoded_size));
    size_t envelope_base = base + ({{ .Ordinal }} - 1) * 2 * sizeof(uint64_t);
    uint64_t num_bytes_then_num_handles =
        (_encoder->CurrentLength() - length_before) |
        ((_encoder->CurrentHandleCount() - handles_before) << 32);
    ::fidl::Encode(_encoder, &num_bytes_then_num_handles, envelope_base);
    *_encoder->GetPtr<uintptr_t>(envelope_base + sizeof(uint64_t)) = FIDL_ALLOC_PRESENT;
  }
  {{- end }}
}

void {{ .Name }}::Decode(::overnet::internal::Decoder* _decoder, {{ .Name }}* _value, size_t _offset) {
  fidl_vector_t* encoded = _decoder->GetPtr<fidl_vector_t>(_offset);
  size_t base;
  size_t count;
  if (!encoded->data) {
    goto clear_all;
  }

  base = _decoder->GetOffset(encoded->data);
  count = encoded->count;

  {{- range .Members }}
  if (count >= {{ .Ordinal }}) {
    size_t envelope_base = base + ({{ .Ordinal }} - 1) * 2 * sizeof(uint64_t);
    uint64_t presence;
    ::fidl::Decode(_decoder, &presence, envelope_base + sizeof(uint64_t));
    if (presence != 0) {
      ::fidl::Decode(_decoder, _value->mutable_{{ .Name }}(), _decoder->GetOffset(presence));
    } else {
      _value->{{ .MethodClearName }}();
    }
  } else {
    goto done_{{ .Ordinal }};
  }
  {{- end }}

  return;

  // Clear unset values.
clear_all:
  {{- range .Members }}
done_{{ .Ordinal }}:
  _value->{{ .MethodClearName }}();
  {{- end }}
  return;
}

zx_status_t {{ .Name }}::Clone({{ .Name }}* result) const {
  {{- range .Members }}
  if ({{ .FieldPresenceName }}) {
    zx_status_t _status = ::fidl::Clone({{ .FieldDataName }}.value, result->mutable_{{ .Name }}());
    if (_status != ZX_OK)
      return _status;
  } else {
    result->{{ .MethodClearName }}();
  }
  {{- end }}
  return ZX_OK;
}

{{- end }}

{{- define "TableTraits" }}
template <>
struct CodingTraits<{{ .Namespace }}::embedded::{{ .Name }}>
    : public EncodableCodingTraits<{{ .Namespace }}::embedded::{{ .Name }}, {{ .Size }}> {};

inline zx_status_t Clone(const {{ .Namespace }}::embedded::{{ .Name }}& _value,
                         {{ .Namespace }}::embedded::{{ .Name }}* result) {
  return _value.Clone(result);
}

template <>
struct ToEmbeddedTraits<{{ .Namespace }}::{{ .Name }}> {
  static {{ .Namespace }}::embedded::{{ .Name }} Lift(const {{ .Namespace }}::{{ .Name }}& _value) {
    {{ .Namespace }}::embedded::{{ .Name }} _out;
    {{- range .Members }}
    if (_value.{{ .MethodHasName }}()) {
      *_out.mutable_{{ .Name }}() = ToEmbedded(_value.{{ .Name }}());
    }
    {{- end }}
    return _out;
  }
};

template<>
struct Equality<{{ .Namespace }}::embedded::{{ .Name }}> {
  static inline bool Equals(const {{ .Namespace }}::embedded::{{ .Name }}& _lhs, const {{ .Namespace }}::embedded::{{ .Name }}& _rhs) {
    {{- range .Members }}
    if (_lhs.{{ .MethodHasName }}()) {
      if (!_rhs.{{ .MethodHasName }}()) {
	return false;
      }
      if (!::fidl::Equals(_lhs.{{ .Name }}(), _rhs.{{ .Name }}())) {
	return false;
      }
    } else if (_rhs.{{ .MethodHasName }}()) {
      return false;
    }
    {{- end }}
    return true;
  }
};
{{- end }}
`
