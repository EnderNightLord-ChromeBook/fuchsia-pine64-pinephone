// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package types

import (
	"encoding/json"
	"fmt"
	"strings"
)

/*
This file contains types which describe FIDL interfaces.

These types are intended to be directly deserialized from the FIDL interface
JSON representation. The types are then passed directly to language-specific
generators which produce source code.

Note that these are different from a naive AST-based representation of
FIDL text. Before being transformed into JSON, FIDL sources are preprocessed
to generate metadata required by all of the backends, such as the size of
types. Importantly, this removes the need for language-specific backends to
implement field, name, or type resolution and analysis.
*/

type Identifier string

type LibraryIdentifier []Identifier

type CompoundIdentifier struct {
	Library LibraryIdentifier
	Name    Identifier
}

type EncodedLibraryIdentifier string

type EncodedCompoundIdentifier string

func (eli EncodedLibraryIdentifier) Parts() LibraryIdentifier {
	return ParseLibraryName(eli)
}

func (eli EncodedLibraryIdentifier) PartsReversed() []string {
	parts := eli.Parts()
	partsReversed := make([]string, len(parts))
	for i, part := range parts {
		partsReversed[len(parts)-i-1] = string(part)
	}

	return partsReversed
}

func (eci EncodedCompoundIdentifier) Parts() CompoundIdentifier {
	return ParseCompoundIdentifier(eci)
}

func (eci EncodedCompoundIdentifier) LibraryName() EncodedLibraryIdentifier {
	parts := strings.SplitN(string(eci), "/", 2)
	raw_library := ""
	if len(parts) == 2 {
		raw_library = parts[0]
	}
	return EncodedLibraryIdentifier(raw_library)
}

func ParseLibraryName(eli EncodedLibraryIdentifier) LibraryIdentifier {
	raw_parts := strings.Split(string(eli), ".")
	parts := make([]Identifier, len(raw_parts))
	for i, raw_part := range raw_parts {
		parts[i] = Identifier(raw_part)
	}
	return LibraryIdentifier(parts)
}

func ParseCompoundIdentifier(eci EncodedCompoundIdentifier) CompoundIdentifier {
	parts := strings.SplitN(string(eci), "/", 2)
	raw_library := ""
	raw_name := parts[0]
	if len(parts) == 2 {
		raw_library = parts[0]
		raw_name = parts[1]
	}
	library := ParseLibraryName(EncodedLibraryIdentifier(raw_library))
	name := Identifier(raw_name)
	return CompoundIdentifier{library, name}
}

func EnsureLibrary(l EncodedLibraryIdentifier, eci EncodedCompoundIdentifier) EncodedCompoundIdentifier {
	if strings.Index(string(eci), "/") != -1 {
		return eci
	}
	new_eci := strings.Join([]string{string(l), "/", string(eci)}, "")
	return EncodedCompoundIdentifier(new_eci)
}

type Ordinal uint32

type PrimitiveSubtype string

const (
	Bool    PrimitiveSubtype = "bool"
	Int8                     = "int8"
	Int16                    = "int16"
	Int32                    = "int32"
	Int64                    = "int64"
	Uint8                    = "uint8"
	Uint16                   = "uint16"
	Uint32                   = "uint32"
	Uint64                   = "uint64"
	Float32                  = "float32"
	Float64                  = "float64"
)

type HandleSubtype string

const (
	Handle    HandleSubtype = "handle"
	Exception               = "exception"
	Process                 = "process"
	Thread                  = "thread"
	Vmo                     = "vmo"
	Channel                 = "channel"
	Event                   = "event"
	Port                    = "port"
	Interrupt               = "interrupt"
	Log                     = "debuglog"
	Socket                  = "socket"
	Resource                = "resource"
	Eventpair               = "eventpair"
	Job                     = "job"
	Vmar                    = "vmar"
	Fifo                    = "fifo"
	Guest                   = "guest"
	Time                    = "timer"
	Bti                     = "bti"
	Profile                 = "profile"
)

type LiteralKind string

const (
	StringLiteral  LiteralKind = "string"
	NumericLiteral             = "numeric"
	TrueLiteral                = "true"
	FalseLiteral               = "false"
	DefaultLiteral             = "default"
)

type Literal struct {
	Kind  LiteralKind `json:"kind"`
	Value string      `json:"value,omitempty"`
}

type ConstantKind string

const (
	IdentifierConstant ConstantKind = "identifier"
	LiteralConstant                 = "literal"
)

type Constant struct {
	Kind       ConstantKind              `json:"kind"`
	Identifier EncodedCompoundIdentifier `json:"identifier,omitempty"`
	Literal    Literal                   `json:"literal,omitempty"`
}

type TypeKind string

const (
	ArrayType      TypeKind = "array"
	VectorType              = "vector"
	StringType              = "string"
	HandleType              = "handle"
	RequestType             = "request"
	PrimitiveType           = "primitive"
	IdentifierType          = "identifier"
)

type Type struct {
	Kind             TypeKind
	ElementType      *Type
	ElementCount     *int
	HandleSubtype    HandleSubtype
	RequestSubtype   EncodedCompoundIdentifier
	PrimitiveSubtype PrimitiveSubtype
	Identifier       EncodedCompoundIdentifier
	Nullable         bool
}

// UnmarshalJSON customizes the JSON unmarshalling for Type.
func (t *Type) UnmarshalJSON(b []byte) error {
	var obj map[string]*json.RawMessage
	err := json.Unmarshal(b, &obj)
	if err != nil {
		return err
	}

	err = json.Unmarshal(*obj["kind"], &t.Kind)
	if err != nil {
		return err
	}

	switch t.Kind {
	case ArrayType:
		t.ElementType = &Type{}
		err = json.Unmarshal(*obj["element_type"], t.ElementType)
		if err != nil {
			return err
		}
		err = json.Unmarshal(*obj["element_count"], &t.ElementCount)
		if err != nil {
			return err
		}
	case VectorType:
		t.ElementType = &Type{}
		err = json.Unmarshal(*obj["element_type"], t.ElementType)
		if err != nil {
			return err
		}
		if elementCount, ok := obj["maybe_element_count"]; ok {
			err = json.Unmarshal(*elementCount, &t.ElementCount)
			if err != nil {
				return err
			}
		}
		err = json.Unmarshal(*obj["nullable"], &t.Nullable)
		if err != nil {
			return err
		}
	case StringType:
		if elementCount, ok := obj["maybe_element_count"]; ok {
			err = json.Unmarshal(*elementCount, &t.ElementCount)
			if err != nil {
				return err
			}
		}
		err = json.Unmarshal(*obj["nullable"], &t.Nullable)
		if err != nil {
			return err
		}
	case HandleType:
		err = json.Unmarshal(*obj["subtype"], &t.HandleSubtype)
		if err != nil {
			return err
		}
		err = json.Unmarshal(*obj["nullable"], &t.Nullable)
		if err != nil {
			return err
		}
	case RequestType:
		err = json.Unmarshal(*obj["subtype"], &t.RequestSubtype)
		if err != nil {
			return err
		}
		err = json.Unmarshal(*obj["nullable"], &t.Nullable)
		if err != nil {
			return err
		}
	case PrimitiveType:
		err = json.Unmarshal(*obj["subtype"], &t.PrimitiveSubtype)
		if err != nil {
			return err
		}
	case IdentifierType:
		err = json.Unmarshal(*obj["identifier"], &t.Identifier)
		if err != nil {
			return err
		}
		err = json.Unmarshal(*obj["nullable"], &t.Nullable)
		if err != nil {
			return err
		}
	default:
		return fmt.Errorf("Unknown type kind: %s", t.Kind)
	}

	return nil
}

type Attribute struct {
	Name  Identifier `json:"name"`
	Value string     `json:"value"`
}

// Attributes represents a list of attributes. It conveniently implements the
// `Annotated` interface, such that it can be embedded into other ndoe structs
// which are annotated.
type Attributes struct {
	Attributes []Attribute `json:"maybe_attributes,omitempty"`
}

func (el Attributes) LookupAttribute(name Identifier) (Attribute, bool) {
	for _, a := range el.Attributes {
		if a.Name == name {
			return a, true
		}
	}
	return Attribute{}, false
}

func (el Attributes) HasAttribute(name Identifier) bool {
	_, ok := el.LookupAttribute(name)
	return ok
}

func (el Attributes) GetAttribute(name Identifier) Attribute {
	attr, _ := el.LookupAttribute(name)
	return attr
}

func (el Attributes) DocComments() []string {
	doc, ok := el.LookupAttribute("Doc")
	if !ok || doc.Value == "" {
		return nil
	}
	return strings.Split(doc.Value[0:len(doc.Value)-1], "\n")
}

func (el Attributes) Transports() map[string]bool {
	transports := map[string]bool{}
	raw, ok := el.LookupAttribute("Transport")
	if ok && raw.Value != "" {
		for _, transport := range strings.Split(raw.Value, ",") {
			transports[strings.TrimSpace(transport)] = true
		}
	}
	// No transport attribute => just Channel
	if !ok {
		transports["Channel"] = true
	}
	return transports
}

// Union represents the declaration of a FIDL union.
type Union struct {
	Attributes
	Name         EncodedCompoundIdentifier `json:"name"`
	Members      []UnionMember             `json:"members"`
	Size         int                       `json:"size"`
	Alignment    int                       `json:"alignment"`
	MaxHandles   int                       `json:"max_handles"`
	MaxOutOfLine int                       `json:"max_out_of_line"`
}

// UnionMember represents the declaration of a field in a FIDL union.
type UnionMember struct {
	Attributes
	Type         Type       `json:"type"`
	Name         Identifier `json:"name"`
	Offset       int        `json:"offset"`
	MaxOutOfLine int        `json:"max_out_of_line"`
}

// XUnion represents the declaration of a FIDL extensible union.
type XUnion struct {
	Attributes
	Name         EncodedCompoundIdentifier `json:"name"`
	Members      []XUnionMember            `json:"members"`
	Size         int                       `json:"size"`
	Alignment    int                       `json:"alignment"`
	MaxHandles   int                       `json:"max_handles"`
	MaxOutOfLine int                       `json:"max_out_of_line"`
	Strictness   `json:"strict"`
}

// XUnionMember represents the declaration of a field in a FIDL extensible
// xunion.
type XUnionMember struct {
	Attributes
	Ordinal      int        `json:"ordinal"`
	Type         Type       `json:"type"`
	Name         Identifier `json:"name"`
	Offset       int        `json:"offset"`
	MaxOutOfLine int        `json:"max_out_of_line"`
}

// Table represents a declaration of a FIDL table.
type Table struct {
	Attributes
	Name         EncodedCompoundIdentifier `json:"name"`
	Members      []TableMember             `json:"members"`
	Size         int                       `json:"size"`
	Alignment    int                       `json:"alignment"`
	MaxHandles   int                       `json:"max_handles"`
	MaxOutOfLine int                       `json:"max_out_of_line"`
}

// TableMember represents the declaration of a field in a FIDL table.
type TableMember struct {
	Attributes
	Reserved          bool       `json:"reserved"`
	Type              Type       `json:"type"`
	Name              Identifier `json:"name"`
	Ordinal           int        `json:"ordinal"`
	MaybeDefaultValue *Constant  `json:"maybe_default_value,omitempty"`
	MaxOutOfLine      int        `json:"max_out_of_line"`
}

// Struct represents a declaration of a FIDL struct.
type Struct struct {
	Attributes
	Name         EncodedCompoundIdentifier `json:"name"`
	Members      []StructMember            `json:"members"`
	Size         int                       `json:"size"`
	Alignment    int                       `json:"alignment"`
	MaxHandles   int                       `json:"max_handles"`
	MaxOutOfLine int                       `json:"max_out_of_line"`
	HasPadding   bool                      `json:"has_padding"`
}

// StructMember represents the declaration of a field in a FIDL struct.
type StructMember struct {
	Attributes
	Type              Type       `json:"type"`
	Name              Identifier `json:"name"`
	Offset            int        `json:"offset"`
	MaybeDefaultValue *Constant  `json:"maybe_default_value,omitempty"`
	MaxHandles        int        `json:"max_handles"`
	MaxOutOfLine      int        `json:"max_out_of_line"`
}

// EmptyStructMember returns a StructMember that's suitable as the sole member
// of an empty struct.
func EmptyStructMember(name string) StructMember {
	// Empty structs have a size of 1, so the uint8 struct member returned by this
	// function can be used to pad the struct to the correct size.

	return StructMember{
		Type: Type{
			Kind:             PrimitiveType,
			PrimitiveSubtype: Uint8,
		},
		Name: Identifier(name),
		MaybeDefaultValue: &Constant{
			Kind:       "literal",
			Identifier: "",
			Literal: Literal{
				Kind:  "numeric",
				Value: "0",
			},
		},
	}
}

// Interface represents the declaration of a FIDL interface.
type Interface struct {
	Attributes
	Name    EncodedCompoundIdentifier `json:"name"`
	Methods []Method                  `json:"methods"`
}

func (d *Interface) GetServiceName() string {
	_, found := d.LookupAttribute("Discoverable")
	if found {
		ci := ParseCompoundIdentifier(d.Name)
		var parts []string
		for _, i := range ci.Library {
			parts = append(parts, string(i))
		}
		parts = append(parts, string(ci.Name))
		return "\"" + strings.Join(parts, ".") + "\""
	}
	return ""
}

// Method represents the declaration of a FIDL method.
type Method struct {
	Attributes
	Ordinal         uint64      `json:"ordinal"`
	GenOrdinal      uint64      `json:"generated_ordinal"`
	Name            Identifier  `json:"name"`
	HasRequest      bool        `json:"has_request"`
	Request         []Parameter `json:"maybe_request,omitempty"`
	RequestSize     int         `json:"maybe_request_size,omitempty"`
	RequestPadding  bool        `json:"maybe_request_has_padding,omitempty"`
	HasResponse     bool        `json:"has_response"`
	Response        []Parameter `json:"maybe_response,omitempty"`
	ResponseSize    int         `json:"maybe_response_size,omitempty"`
	ResponsePadding bool        `json:"maybe_response_has_padding,omitempty"`
}

// IsTransitional returns whether this method has the `Transitional` attribute.
func (m *Method) IsTransitional() bool {
	_, transitional := m.LookupAttribute("Transitional")
	return transitional
}

// Parameter represents a parameter to a FIDL method.
type Parameter struct {
	Type         Type       `json:"type"`
	Name         Identifier `json:"name"`
	Offset       int        `json:"offset"`
	MaxHandles   int        `json:"max_handles"`
	MaxOutOfLine int        `json:"max_out_of_line"`
}

// Enum represents a FIDL declaration of an enum.
type Enum struct {
	Attributes
	Type    PrimitiveSubtype          `json:"type"`
	Name    EncodedCompoundIdentifier `json:"name"`
	Members []EnumMember              `json:"members"`
}

// EnumMember represents a single variant in a FIDL enum.
type EnumMember struct {
	Attributes
	Name  Identifier `json:"name"`
	Value Constant   `json:"value"`
}

// Bits represents a FIDL declaration of an bits.
type Bits struct {
	Attributes
	Type    Type                      `json:"type"`
	Name    EncodedCompoundIdentifier `json:"name"`
	Mask    string                    `json:"mask"`
	Members []BitsMember              `json:"members"`
}

// BitsMember represents a single variant in a FIDL bits.
type BitsMember struct {
	Attributes
	Name  Identifier `json:"name"`
	Value Constant   `json:"value"`
}

// Const represents a FIDL declaration of a named constant.
type Const struct {
	Attributes
	Type  Type                      `json:"type"`
	Name  EncodedCompoundIdentifier `json:"name"`
	Value Constant                  `json:"value"`
}

// Strictness represents whether a FIDL object is strict or flexible. See
// <https://fuchsia.dev/fuchsia-src/development/languages/fidl/reference/ftp/ftp-033> for more
// information.
type Strictness bool

const (
	IsFlexible Strictness = false
	IsStrict   Strictness = true
)

func (s Strictness) IsStrict() bool {
	return s == IsStrict
}

func (s Strictness) IsFlexible() bool {
	return s == IsFlexible
}

type DeclType string

const (
	ConstDeclType     DeclType = "const"
	BitsDeclType               = "bits"
	EnumDeclType               = "enum"
	InterfaceDeclType          = "interface"
	StructDeclType             = "struct"
	TableDeclType              = "table"
	UnionDeclType              = "union"
	XUnionDeclType             = "xunion"
)

type DeclMap map[EncodedCompoundIdentifier]DeclType

// Library represents a FIDL dependency on a separate library.
type Library struct {
	Name  EncodedLibraryIdentifier `json:"name,omitempty"`
	Decls DeclMap                  `json:"declarations,omitempty"`
}

// Root is the top-level object for a FIDL library.
// It contains lists of all declarations and dependencies within the library.
type Root struct {
	Name       EncodedLibraryIdentifier    `json:"name,omitempty"`
	Consts     []Const                     `json:"const_declarations,omitempty"`
	Bits       []Bits                      `json:"bits_declarations,omitempty"`
	Enums      []Enum                      `json:"enum_declarations,omitempty"`
	Interfaces []Interface                 `json:"interface_declarations,omitempty"`
	Structs    []Struct                    `json:"struct_declarations,omitempty"`
	Tables     []Table                     `json:"table_declarations,omitempty"`
	Unions     []Union                     `json:"union_declarations,omitempty"`
	XUnions    []XUnion                    `json:"xunion_declarations,omitempty"`
	DeclOrder  []EncodedCompoundIdentifier `json:"declaration_order,omitempty"`
	Decls      DeclMap                     `json:"declarations,omitempty"`
	Libraries  []Library                   `json:"library_dependencies,omitempty"`
}
