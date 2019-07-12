// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package cpp

import (
	"fmt"
	"io"
	"strconv"
	"strings"
	"text/template"

	fidlir "fidl/compiler/backend/types"
	gidlir "gidl/ir"
	gidlmixer "gidl/mixer"
)

var tmpls = template.Must(template.New("tmpls").Parse(`
{{- define "Header"}}
#include <gtest/gtest.h>

#include <lib/fidl/cpp/test/test_util.h>

#include <conformance/cpp/fidl.h>

{{end -}}

{{- define "SuccessCase"}}

TEST(Conformance, {{ .name }}_Encoding) {
  {{ .value_build }}

  auto expected = std::vector<uint8_t>{
    {{ .bytes }}
  };

  EXPECT_TRUE(::fidl::test::util::ValueToBytes({{ .value_var }}, expected));
}

TEST(Conformance, {{ .name }}_Decoding) {
  auto input = std::vector<uint8_t>{
    {{ .bytes }}
  };

  {{ .value_build }}

  auto expected = ::fidl::test::util::DecodedBytes<decltype({{ .value_var }})>(input);
  EXPECT_TRUE(::fidl::Equals({{ .value_var }}, expected));
}

{{end -}}
`))

func Generate(wr io.Writer, gidl gidlir.All, fidl fidlir.Root) error {
	if err := tmpls.ExecuteTemplate(wr, "Header", nil); err != nil {
		return err
	}
	for _, success := range gidl.Success {
		decl, err := gidlmixer.ExtractDeclaration(success.Value, fidl)
		if err != nil {
			return fmt.Errorf("success %s: %s", success.Name, err)
		}

		var valueBuilder cppValueBuilder
		gidlmixer.Visit(&valueBuilder, success.Value, decl)

		if err := tmpls.ExecuteTemplate(wr, "SuccessCase", map[string]interface{}{
			"name":        success.Name,
			"value_build": valueBuilder.String(),
			"value_var":   valueBuilder.lastVar,
			"bytes":       bytesBuilder(success.Bytes),
		}); err != nil {
			return err
		}
	}

	return nil
}

// extract out to common library (this is the same code as golang.go)
func bytesBuilder(bytes []byte) string {
	var builder strings.Builder
	for i, b := range bytes {
		builder.WriteString(fmt.Sprintf("0x%02x", b))
		builder.WriteString(", ")
		if i%8 == 7 {
			builder.WriteString("\n")
		}
	}
	return builder.String()
}

type cppValueBuilder struct {
	strings.Builder

	varidx  int
	lastVar string

	// `context` references the FIDL declaration that a visited object is
	// contained in when one of the On*() methods is called. Given "struct S1 { };
	// struct S2 { S1 s1; };", if the .s1 field in an S2 struct is being visited,
	// `context.decl` will be S1, while `context.key` will be "s1".
	context cppValueBuilderContext
}

type cppValueBuilderContext struct {
	key  string
	decl gidlmixer.Declaration
}

func (b *cppValueBuilder) newVar() string {
	b.varidx++
	return fmt.Sprintf("v%d", b.varidx)
}

func (b *cppValueBuilder) OnBool(value bool) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("bool %s = %t;\n", newVar, value))
	b.lastVar = newVar
}

func (b *cppValueBuilder) OnInt64(value int64, _ fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("int64_t %s = %dll;\n", newVar, value))
	b.lastVar = newVar
}

func (b *cppValueBuilder) OnUint64(value uint64, _ fidlir.PrimitiveSubtype) {
	newVar := b.newVar()
	b.Builder.WriteString(fmt.Sprintf("uint64_t %s = %dull;\n", newVar, value))
	b.lastVar = newVar
}

func (b *cppValueBuilder) OnString(value string) {
	newVar := b.newVar()

	// strconv.Quote() below produces a quoted _Go_ string (not C string), which
	// isn't technically correct since Go & C strings will have different escape
	// characters, etc. However, this should be OK until we we find a use-case
	// that breaks it.
	b.Builder.WriteString(fmt.Sprintf(
		"std::string %s = %s;\n", newVar, strconv.Quote(value)))

	b.lastVar = newVar
}

func (b *cppValueBuilder) OnStruct(value gidlir.Object, decl *gidlmixer.StructDecl) {
	b.onObject(value, decl)
}

func (b *cppValueBuilder) OnTable(value gidlir.Object, decl *gidlmixer.TableDecl) {
	b.onObject(value, decl)
}

func (b *cppValueBuilder) OnXUnion(value gidlir.Object, decl *gidlmixer.XUnionDecl) {
	b.onObject(value, decl)
}

func (b *cppValueBuilder) OnUnion(value gidlir.Object, decl *gidlmixer.UnionDecl) {
	b.onObject(value, decl)
}

func (b *cppValueBuilder) onObjectField(decl gidlmixer.Declaration, key string, f func()) {
	oldContext := b.context
	defer func() {
		b.context = oldContext
	}()

	b.context = cppValueBuilderContext{decl: decl, key: key}
	f()
}

func (b *cppValueBuilder) onObject(value gidlir.Object, decl gidlmixer.Declaration) {
	containerVar := b.newVar()

	isOptionalStructMember := false
	switch decl := b.context.decl.(type) {
	case *gidlmixer.StructDecl:
		isOptionalStructMember = decl.IsKeyNullable(b.context.key)
	}

	if isOptionalStructMember {
		b.Builder.WriteString(fmt.Sprintf(
			"auto %s = std::make_unique<conformance::%s>();\n", containerVar, value.Name))
	} else {
		b.Builder.WriteString(fmt.Sprintf(
			"conformance::%s %s;\n", value.Name, containerVar))
	}

	for key, field := range value.Fields {
		b.Builder.WriteString("\n")

		fieldDecl, _ := decl.ForKey(key)

		b.onObjectField(decl, key, func() {
			gidlmixer.Visit(b, field, fieldDecl)
		})

		accessor := "."
		if isOptionalStructMember {
			accessor = "->"
		}

		switch decl.(type) {
		case *gidlmixer.StructDecl:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%s%s = std::move(%s);\n", containerVar, accessor, key, b.lastVar))
		default:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%sset_%s(std::move(%s));\n", containerVar, accessor, key, b.lastVar))
		}
	}
	b.lastVar = containerVar
}
