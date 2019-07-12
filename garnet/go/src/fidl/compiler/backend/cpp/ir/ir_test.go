// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package ir

import (
	"reflect"
	"testing"

	"fidl/compiler/backend/types"
	. "fidl/compiler/backend/typestest"
)

func TestCompileInterface(t *testing.T) {
	cases := []struct {
		name     string
		input    types.Interface
		expected Interface
	}{
		{
			name: "Basic",
			input: types.Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Name: types.EncodedCompoundIdentifier("Test"),
				Methods: []types.Method{
					{
						Ordinal:    1,
						GenOrdinal: 314159,
						Name:       types.Identifier("First"),
						HasRequest: true,
						Request: []types.Parameter{
							{
								Type: PrimitiveType(types.Int16),
								Name: types.Identifier("Value"),
							},
						},
						RequestSize: 18,
					},
					{
						Ordinal:    2,
						GenOrdinal: 271828,
						Name:       types.Identifier("Second"),
						HasRequest: true,
						Request: []types.Parameter{
							{
								Type: Nullable(StringType(nil)),
								Name: types.Identifier("Value"),
							},
						},
						RequestSize: 32,
						HasResponse: true,
						Response: []types.Parameter{
							{
								Type: PrimitiveType(types.Uint32),
								Name: types.Identifier("Value"),
							},
						},
						ResponseSize: 20,
					},
				},
			},
			//b EventSenderName: SyncName: SyncProxyName: Methods:[
			//Request:[{Type:{Decl:int16_t Dtor: DeclType:} Name:Value Offset:0}]
			//
			expected: Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Namespace:             "::",
				Name:                  "Test",
				ClassName:             "Test_clazz",
				ServiceName:           "",
				ProxyName:             "Test_Proxy",
				StubName:              "Test_Stub",
				EventSenderName:       "Test_EventSender",
				SyncName:              "Test_Sync",
				SyncProxyName:         "Test_SyncProxy",
				HasEvents:             false,
				StackAllocEventBuffer: true,
				Methods: []Method{
					{
						Ordinal:              1,
						OrdinalName:          "kTest_First_Ordinal",
						GenOrdinal:           314159,
						GenOrdinalName:       "kTest_First_GenOrdinal",
						Name:                 "First",
						NameInLowerSnakeCase: "first",
						HasRequest:           true,
						Request: []Parameter{
							{
								Type: Type{
									Decl:                "int16_t",
									LLDecl:              "int16_t",
									OvernetEmbeddedDecl: "int16_t",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						RequestSize:         18,
						RequestTypeName:     "_TestFirstRequestTable",
						RequestMaxHandles:   0,
						HasResponse:         false,
						Response:            []Parameter{},
						ResponseSize:        0,
						ResponseTypeName:    "_TestFirstResponseTable",
						ResponseMaxHandles:  0,
						CallbackType:        "",
						ResponseHandlerType: "Test_First_ResponseHandler",
						ResponderType:       "Test_First_Responder",
						LLProps: LLProps{
							InterfaceName:      "Test",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: true,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
					{
						Ordinal:              2,
						OrdinalName:          "kTest_Second_Ordinal",
						GenOrdinal:           271828,
						GenOrdinalName:       "kTest_Second_GenOrdinal",
						Name:                 "Second",
						NameInLowerSnakeCase: "second",
						HasRequest:           true,
						Request: []Parameter{
							{
								Type: Type{
									Decl:                "::fidl::StringPtr",
									LLDecl:              "::fidl::StringView",
									Dtor:                "~StringPtr",
									OvernetEmbeddedDecl: "::fidl::StringPtr",
									OvernetEmbeddedDtor: "~StringPtr",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						RequestSize:       32,
						RequestTypeName:   "_TestSecondRequestTable",
						RequestMaxHandles: 0,
						HasResponse:       true,
						Response: []Parameter{
							{
								Type: Type{
									Decl:                "uint32_t",
									LLDecl:              "uint32_t",
									OvernetEmbeddedDecl: "uint32_t",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						ResponseSize:        20,
						ResponseTypeName:    "_TestSecondResponseTable",
						ResponseMaxHandles:  0,
						CallbackType:        "SecondCallback",
						ResponseHandlerType: "Test_Second_ResponseHandler",
						ResponderType:       "Test_Second_Responder",
						LLProps: LLProps{
							InterfaceName:      "Test",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: true,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
				},
			},
		},
		{
			name: "Events",
			input: types.Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Name: types.EncodedCompoundIdentifier("EventTest"),
				Methods: []types.Method{
					{
						Ordinal:     1,
						GenOrdinal:  314159,
						Name:        types.Identifier("First"),
						HasResponse: true,
						Response: []types.Parameter{
							{
								Type: PrimitiveType(types.Int16),
								Name: types.Identifier("Value"),
							},
						},
						ResponseSize: 18,
					},
				},
			},
			expected: Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Namespace:             "::",
				Name:                  "EventTest",
				ClassName:             "EventTest_clazz",
				ServiceName:           "",
				ProxyName:             "EventTest_Proxy",
				StubName:              "EventTest_Stub",
				EventSenderName:       "EventTest_EventSender",
				SyncName:              "EventTest_Sync",
				SyncProxyName:         "EventTest_SyncProxy",
				HasEvents:             true,
				StackAllocEventBuffer: true,
				Methods: []Method{
					{
						Ordinal:              1,
						OrdinalName:          "kEventTest_First_Ordinal",
						GenOrdinal:           314159,
						GenOrdinalName:       "kEventTest_First_GenOrdinal",
						Name:                 "First",
						NameInLowerSnakeCase: "first",
						Request:              []Parameter{},
						RequestSize:          0,
						RequestTypeName:      "_EventTestFirstRequestTable",
						RequestMaxHandles:    0,
						HasResponse:          true,
						Response: []Parameter{
							{
								Type: Type{
									Decl:                "int16_t",
									LLDecl:              "int16_t",
									OvernetEmbeddedDecl: "int16_t",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						ResponseSize:        18,
						ResponseTypeName:    "_EventTestFirstEventTable",
						ResponseMaxHandles:  0,
						CallbackType:        "FirstCallback",
						ResponseHandlerType: "EventTest_First_ResponseHandler",
						ResponderType:       "EventTest_First_Responder",
						LLProps: LLProps{
							InterfaceName:      "EventTest",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: true,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
				},
			},
		},
		{
			name: "EventsTooBigForStack",
			input: types.Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Name: types.EncodedCompoundIdentifier("EventTest"),
				Methods: []types.Method{
					{
						Ordinal:     2,
						GenOrdinal:  271828,
						Name:        types.Identifier("Second"),
						HasResponse: true,
						Response: []types.Parameter{
							{
								Type: types.Type{
									Kind: types.ArrayType,
									ElementType: &types.Type{
										Kind:             types.PrimitiveType,
										PrimitiveSubtype: types.Int64,
									},
									ElementCount: addrOf(8000),
								},
								Name: types.Identifier("Value"),
							},
						},
						ResponseSize: 64016,
					},
				},
			},
			expected: Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Namespace:             "::",
				Name:                  "EventTest",
				ClassName:             "EventTest_clazz",
				ServiceName:           "",
				ProxyName:             "EventTest_Proxy",
				StubName:              "EventTest_Stub",
				EventSenderName:       "EventTest_EventSender",
				SyncName:              "EventTest_Sync",
				SyncProxyName:         "EventTest_SyncProxy",
				HasEvents:             true,
				StackAllocEventBuffer: false,
				Methods: []Method{
					{
						Ordinal:              2,
						OrdinalName:          "kEventTest_Second_Ordinal",
						GenOrdinal:           271828,
						GenOrdinalName:       "kEventTest_Second_GenOrdinal",
						Name:                 "Second",
						NameInLowerSnakeCase: "second",
						HasRequest:           false,
						Request:              []Parameter{},
						RequestSize:          0,
						RequestTypeName:      "_EventTestSecondRequestTable",
						RequestMaxHandles:    0,
						HasResponse:          true,
						Response: []Parameter{
							{
								Type: Type{
									Decl:                "::std::array<int64_t, 8000>",
									LLDecl:              "::fidl::Array<int64_t, 8000>",
									Dtor:                "~array",
									LLDtor:              "~Array",
									OvernetEmbeddedDecl: "::std::array<int64_t, 8000>",
									OvernetEmbeddedDtor: "~array",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						ResponseSize:        64016,
						ResponseTypeName:    "_EventTestSecondEventTable",
						ResponseMaxHandles:  0,
						CallbackType:        "SecondCallback",
						ResponseHandlerType: "EventTest_Second_ResponseHandler",
						ResponderType:       "EventTest_Second_Responder",
						LLProps: LLProps{
							InterfaceName:      "EventTest",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: false,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
				},
			},
		},
	}
	for _, ex := range cases {
		t.Run(ex.name, func(t *testing.T) {
			root := types.Root{
				Interfaces: []types.Interface{ex.input},
				DeclOrder: []types.EncodedCompoundIdentifier{
					ex.input.Name,
				},
			}
			result := Compile(root)
			actual, ok := result.Decls[0].(*Interface)
			if !ok || actual == nil {
				t.Fatalf("decls[0] not an interface, was instead %T", result.Decls[0])
			}
			if !reflect.DeepEqual(ex.expected, *actual) {
				t.Fatalf("expected %+v\nactual %+v", ex.expected, *actual)
			}
		})
	}
}

func TestCompileInterfaceLLCPP(t *testing.T) {
	cases := []struct {
		name     string
		input    types.Interface
		expected Interface
	}{
		{
			name: "Basic",
			input: types.Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Name: types.EncodedCompoundIdentifier("Test"),
				Methods: []types.Method{
					{
						Ordinal:    1,
						GenOrdinal: 314159,
						Name:       types.Identifier("First"),
						HasRequest: true,
						Request: []types.Parameter{
							{
								Type: PrimitiveType(types.Int16),
								Name: types.Identifier("Value"),
							},
						},
						RequestSize: 18,
					},
					{
						Ordinal:    2,
						GenOrdinal: 271828,
						Name:       types.Identifier("Second"),
						HasRequest: true,
						Request: []types.Parameter{
							{
								Type: Nullable(StringType(nil)),
								Name: types.Identifier("Value"),
							},
						},
						RequestSize: 32,
						HasResponse: true,
						Response: []types.Parameter{
							{
								Type: PrimitiveType(types.Uint32),
								Name: types.Identifier("Value"),
							},
						},
						ResponseSize: 20,
					},
				},
			},
			//b EventSenderName: SyncName: SyncProxyName: Methods:[
			//Request:[{Type:{Decl:int16_t Dtor: DeclType:} Name:Value Offset:0}]
			//
			expected: Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Namespace:             "::llcpp::",
				Name:                  "Test",
				ClassName:             "Test_clazz",
				ServiceName:           "",
				ProxyName:             "Test_Proxy",
				StubName:              "Test_Stub",
				EventSenderName:       "Test_EventSender",
				SyncName:              "Test_Sync",
				SyncProxyName:         "Test_SyncProxy",
				HasEvents:             false,
				StackAllocEventBuffer: true,
				Methods: []Method{
					{
						Ordinal:              1,
						OrdinalName:          "kTest_First_Ordinal",
						GenOrdinal:           314159,
						GenOrdinalName:       "kTest_First_GenOrdinal",
						Name:                 "First",
						NameInLowerSnakeCase: "first",
						HasRequest:           true,
						Request: []Parameter{
							{
								Type: Type{
									Decl:                "int16_t",
									LLDecl:              "int16_t",
									OvernetEmbeddedDecl: "int16_t",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						RequestSize:         18,
						RequestTypeName:     "_TestFirstRequestTable",
						RequestMaxHandles:   0,
						HasResponse:         false,
						Response:            []Parameter{},
						ResponseSize:        0,
						ResponseTypeName:    "_TestFirstResponseTable",
						ResponseMaxHandles:  0,
						CallbackType:        "",
						ResponseHandlerType: "Test_First_ResponseHandler",
						ResponderType:       "Test_First_Responder",
						LLProps: LLProps{
							InterfaceName:      "Test",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: true,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
					{
						Ordinal:              2,
						OrdinalName:          "kTest_Second_Ordinal",
						GenOrdinal:           271828,
						GenOrdinalName:       "kTest_Second_GenOrdinal",
						Name:                 "Second",
						NameInLowerSnakeCase: "second",
						HasRequest:           true,
						Request: []Parameter{
							{
								Type: Type{
									Decl:                "::fidl::StringPtr",
									LLDecl:              "::fidl::StringView",
									Dtor:                "~StringPtr",
									OvernetEmbeddedDecl: "::fidl::StringPtr",
									OvernetEmbeddedDtor: "~StringPtr",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						RequestSize:       32,
						RequestTypeName:   "_TestSecondRequestTable",
						RequestMaxHandles: 0,
						HasResponse:       true,
						Response: []Parameter{
							{
								Type: Type{
									Decl:                "uint32_t",
									LLDecl:              "uint32_t",
									OvernetEmbeddedDecl: "uint32_t",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						ResponseSize:        20,
						ResponseTypeName:    "_TestSecondResponseTable",
						ResponseMaxHandles:  0,
						CallbackType:        "SecondCallback",
						ResponseHandlerType: "Test_Second_ResponseHandler",
						ResponderType:       "Test_Second_Responder",
						LLProps: LLProps{
							InterfaceName:      "Test",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: true,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
				},
			},
		},
		{
			name: "Events",
			input: types.Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Name: types.EncodedCompoundIdentifier("EventTest"),
				Methods: []types.Method{
					{
						Ordinal:     1,
						GenOrdinal:  314159,
						Name:        types.Identifier("First"),
						HasResponse: true,
						Response: []types.Parameter{
							{
								Type: PrimitiveType(types.Int16),
								Name: types.Identifier("Value"),
							},
						},
						ResponseSize: 18,
					},
				},
			},
			expected: Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Namespace:             "::llcpp::",
				Name:                  "EventTest",
				ClassName:             "EventTest_clazz",
				ServiceName:           "",
				ProxyName:             "EventTest_Proxy",
				StubName:              "EventTest_Stub",
				EventSenderName:       "EventTest_EventSender",
				SyncName:              "EventTest_Sync",
				SyncProxyName:         "EventTest_SyncProxy",
				HasEvents:             true,
				StackAllocEventBuffer: true,
				Methods: []Method{
					{
						Ordinal:              1,
						OrdinalName:          "kEventTest_First_Ordinal",
						GenOrdinal:           314159,
						GenOrdinalName:       "kEventTest_First_GenOrdinal",
						Name:                 "First",
						NameInLowerSnakeCase: "first",
						Request:              []Parameter{},
						RequestSize:          0,
						RequestTypeName:      "_EventTestFirstRequestTable",
						RequestMaxHandles:    0,
						HasResponse:          true,
						Response: []Parameter{
							{
								Type: Type{
									Decl:                "int16_t",
									LLDecl:              "int16_t",
									OvernetEmbeddedDecl: "int16_t",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						ResponseSize:        18,
						ResponseTypeName:    "_EventTestFirstEventTable",
						ResponseMaxHandles:  0,
						CallbackType:        "FirstCallback",
						ResponseHandlerType: "EventTest_First_ResponseHandler",
						ResponderType:       "EventTest_First_Responder",
						LLProps: LLProps{
							InterfaceName:      "EventTest",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: true,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
				},
			},
		},
		{
			name: "EventsTooBigForStack",
			input: types.Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Name: types.EncodedCompoundIdentifier("EventTest"),
				Methods: []types.Method{
					{
						Ordinal:     2,
						GenOrdinal:  271828,
						Name:        types.Identifier("Second"),
						HasResponse: true,
						Response: []types.Parameter{
							{
								Type: types.Type{
									Kind: types.ArrayType,
									ElementType: &types.Type{
										Kind:             types.PrimitiveType,
										PrimitiveSubtype: types.Int64,
									},
									ElementCount: addrOf(8000),
								},
								Name: types.Identifier("Value"),
							},
						},
						ResponseSize: 64016,
					},
				},
			},
			expected: Interface{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "EventTest"},
					},
				},
				Namespace:             "::llcpp::",
				Name:                  "EventTest",
				ClassName:             "EventTest_clazz",
				ServiceName:           "",
				ProxyName:             "EventTest_Proxy",
				StubName:              "EventTest_Stub",
				EventSenderName:       "EventTest_EventSender",
				SyncName:              "EventTest_Sync",
				SyncProxyName:         "EventTest_SyncProxy",
				HasEvents:             true,
				StackAllocEventBuffer: false,
				Methods: []Method{
					{
						Ordinal:              2,
						OrdinalName:          "kEventTest_Second_Ordinal",
						GenOrdinal:           271828,
						GenOrdinalName:       "kEventTest_Second_GenOrdinal",
						Name:                 "Second",
						NameInLowerSnakeCase: "second",
						HasRequest:           false,
						Request:              []Parameter{},
						RequestSize:          0,
						RequestTypeName:      "_EventTestSecondRequestTable",
						RequestMaxHandles:    0,
						HasResponse:          true,
						Response: []Parameter{
							{
								Type: Type{
									Decl:                "::std::array<int64_t, 8000>",
									LLDecl:              "::fidl::Array<int64_t, 8000>",
									Dtor:                "~array",
									LLDtor:              "~Array",
									OvernetEmbeddedDecl: "::std::array<int64_t, 8000>",
									OvernetEmbeddedDtor: "~array",
								},
								Name:   "Value",
								Offset: 0,
							},
						},
						ResponseSize:        64016,
						ResponseTypeName:    "_EventTestSecondEventTable",
						ResponseMaxHandles:  0,
						CallbackType:        "SecondCallback",
						ResponseHandlerType: "EventTest_Second_ResponseHandler",
						ResponderType:       "EventTest_Second_Responder",
						LLProps: LLProps{
							InterfaceName:      "EventTest",
							CBindingCompatible: true,
							LinearizeRequest:   false,
							LinearizeResponse:  false,
							StackAllocRequest:  true,
							StackAllocResponse: false,
							EncodeRequest:      false,
							DecodeResponse:     false,
						},
					},
				},
			},
		},
	}
	for _, ex := range cases {
		t.Run(ex.name, func(t *testing.T) {
			root := types.Root{
				Interfaces: []types.Interface{ex.input},
				DeclOrder: []types.EncodedCompoundIdentifier{
					ex.input.Name,
				},
			}
			result := CompileLL(root)
			actual, ok := result.Decls[0].(*Interface)
			if !ok || actual == nil {
				t.Fatalf("decls[0] not an interface, was instead %T", result.Decls[0])
			}
			if !reflect.DeepEqual(ex.expected, *actual) {
				t.Fatalf("expected %+v\nactual %+v", ex.expected, *actual)
			}
		})
	}
}

func TestCompileTable(t *testing.T) {
	cases := []struct {
		name     string
		input    types.Table
		expected Table
	}{
		{
			name: "Basic",
			input: types.Table{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Name: types.EncodedCompoundIdentifier("Test"),
				Members: []types.TableMember{
					{
						Reserved: true,
						Ordinal:  1,
					},
					{
						Ordinal:  2,
						Name:     types.Identifier("second"),
						Reserved: false,
						Type: types.Type{
							Kind:             types.PrimitiveType,
							PrimitiveSubtype: types.Int64,
						},
					},
				},
			},
			expected: Table{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Namespace:      "::",
				Name:           "Test",
				TableType:      "_TestTable",
				BiggestOrdinal: 2,
				MaxHandles:     0,
				Members: []TableMember{
					{
						Type: Type{
							Decl:                "int64_t",
							LLDecl:              "int64_t",
							OvernetEmbeddedDecl: "int64_t",
						},
						Name:              "second",
						Ordinal:           2,
						FieldPresenceName: "has_second_",
						FieldDataName:     "second_value_",
						MethodHasName:     "has_second",
						MethodClearName:   "clear_second",
						ValueUnionName:    "ValueUnion_second",
					},
				},
			},
		},
	}
	for _, ex := range cases {
		t.Run(ex.name, func(t *testing.T) {
			root := types.Root{
				Tables: []types.Table{ex.input},
				DeclOrder: []types.EncodedCompoundIdentifier{
					ex.input.Name,
				},
			}
			result := Compile(root)
			actual, ok := result.Decls[0].(*Table)

			if !ok || actual == nil {
				t.Fatalf("decls[0] not an table, was instead %T", result.Decls[0])
			}
			if !reflect.DeepEqual(ex.expected, *actual) {
				t.Fatalf("expected %+v\nactual %+v", ex.expected, *actual)
			}
		})
	}
}

func TestCompileTableLlcppNamespaceShouldBeRenamed(t *testing.T) {
	cases := []struct {
		name     string
		input    types.Table
		expected Table
	}{
		{
			name: "Basic",
			input: types.Table{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Name: types.EncodedCompoundIdentifier("llcpp.foo/Test"),
				Members: []types.TableMember{
					{
						Reserved: true,
						Ordinal:  1,
					},
					{
						Ordinal:  2,
						Name:     types.Identifier("second"),
						Reserved: false,
						Type: types.Type{
							Kind:             types.PrimitiveType,
							PrimitiveSubtype: types.Int64,
						},
					},
				},
			},
			expected: Table{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{Name: types.Identifier("ServiceName"), Value: "Test"},
					},
				},
				Namespace:      "::llcpp::llcpp_::foo",
				Name:           "Test",
				TableType:      "llcpp_foo_TestTable",
				BiggestOrdinal: 2,
				MaxHandles:     0,
				Members: []TableMember{
					{
						Type: Type{
							Decl:                "int64_t",
							LLDecl:              "int64_t",
							OvernetEmbeddedDecl: "int64_t",
						},
						Name:              "second",
						Ordinal:           2,
						FieldPresenceName: "has_second_",
						FieldDataName:     "second_value_",
						MethodHasName:     "has_second",
						MethodClearName:   "clear_second",
						ValueUnionName:    "ValueUnion_second",
					},
				},
			},
		},
	}
	for _, ex := range cases {
		t.Run(ex.name, func(t *testing.T) {
			root := types.Root{
				Name:   types.EncodedLibraryIdentifier("llcpp.foo"),
				Tables: []types.Table{ex.input},
				DeclOrder: []types.EncodedCompoundIdentifier{
					ex.input.Name,
				},
			}
			result := CompileLL(root)
			actual, ok := result.Decls[0].(*Table)

			if !ok || actual == nil {
				t.Fatalf("decls[0] not an table, was instead %T", result.Decls[0])
			}
			if !reflect.DeepEqual(ex.expected, *actual) {
				t.Fatalf("expected %+v\nactual %+v", ex.expected, *actual)
			}
		})
	}
}

func addrOf(x int) *int {
	return &x
}

func TestCompileXUnion(t *testing.T) {
	cases := []struct {
		name     string
		input    types.XUnion
		expected XUnion
	}{
		{
			name: "SingleInt64",
			input: types.XUnion{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{
							Name:  types.Identifier("Foo"),
							Value: "Bar",
						},
					},
				},
				Name: types.EncodedCompoundIdentifier("Test"),
				Members: []types.XUnionMember{
					{
						Attributes: types.Attributes{},
						Ordinal:    0xdeadbeef,
						Type: types.Type{
							Kind:             types.PrimitiveType,
							PrimitiveSubtype: types.Int64,
						},
						Name:         types.Identifier("i"),
						Offset:       0,
						MaxOutOfLine: 0,
					},
				},
				Size:         24,
				MaxHandles:   0,
				MaxOutOfLine: 4294967295,
				Strictness:   types.IsFlexible,
			},
			expected: XUnion{
				Attributes: types.Attributes{
					Attributes: []types.Attribute{
						{
							Name:  types.Identifier("Foo"),
							Value: "Bar",
						},
					},
				},
				Namespace: "::",
				Name:      "Test",
				TableType: "_TestTable",
				Members: []XUnionMember{
					{
						Attributes: types.Attributes{},
						Ordinal:    0xdeadbeef,
						Type: Type{
							Decl:                "int64_t",
							LLDecl:              "int64_t",
							OvernetEmbeddedDecl: "int64_t",
						},
						Name:        "i",
						StorageName: "i_",
						TagName:     "kI",
						Offset:      0,
					},
				},
				Size:         24,
				MaxHandles:   0,
				MaxOutOfLine: 4294967295,
				Strictness:   types.IsFlexible,
			},
		},
		{
			name: "TwoArrays",
			input: types.XUnion{
				Attributes: types.Attributes{},
				Name:       types.EncodedCompoundIdentifier("Test"),
				Members: []types.XUnionMember{
					{
						Attributes: types.Attributes{},
						Ordinal:    0x11111111,
						Type: types.Type{
							Kind: types.ArrayType,
							ElementType: &types.Type{
								Kind:             types.PrimitiveType,
								PrimitiveSubtype: types.Int64,
							},
							ElementCount: addrOf(10),
						},
						Name:         types.Identifier("i"),
						Offset:       0,
						MaxOutOfLine: 0,
					},
					{
						Attributes: types.Attributes{},
						Ordinal:    0x22222222,
						Type: types.Type{
							Kind: types.ArrayType,
							ElementType: &types.Type{
								Kind:             types.PrimitiveType,
								PrimitiveSubtype: types.Int64,
							},
							ElementCount: addrOf(20),
						},
						Name:         types.Identifier("j"),
						Offset:       0,
						MaxOutOfLine: 0,
					},
				},
				Size:         24,
				MaxHandles:   0,
				MaxOutOfLine: 4294967295,
				Strictness:   types.IsFlexible,
			},
			expected: XUnion{
				Attributes: types.Attributes{},
				Namespace:  "::",
				Name:       "Test",
				TableType:  "_TestTable",
				Members: []XUnionMember{
					{
						Attributes: types.Attributes{},
						Ordinal:    0x11111111,
						Type: Type{
							Decl:                "::std::array<int64_t, 10>",
							LLDecl:              "::fidl::Array<int64_t, 10>",
							Dtor:                "~array",
							LLDtor:              "~Array",
							OvernetEmbeddedDecl: "::std::array<int64_t, 10>",
							OvernetEmbeddedDtor: "~array",
						},
						Name:        "i",
						StorageName: "i_",
						TagName:     "kI",
						Offset:      0,
					},
					{
						Attributes: types.Attributes{},
						Ordinal:    0x22222222,
						Type: Type{
							Decl:                "::std::array<int64_t, 20>",
							LLDecl:              "::fidl::Array<int64_t, 20>",
							Dtor:                "~array",
							LLDtor:              "~Array",
							OvernetEmbeddedDecl: "::std::array<int64_t, 20>",
							OvernetEmbeddedDtor: "~array",
						},
						Name:        "j",
						StorageName: "j_",
						TagName:     "kJ",
						Offset:      0,
					},
				},
				Size:         24,
				MaxHandles:   0,
				MaxOutOfLine: 4294967295,
				Strictness:   types.IsFlexible,
			},
		},
	}
	for _, ex := range cases {
		t.Run(ex.name, func(t *testing.T) {
			root := types.Root{
				XUnions: []types.XUnion{ex.input},
				DeclOrder: []types.EncodedCompoundIdentifier{
					ex.input.Name,
				},
			}
			result := Compile(root)
			actual, ok := result.Decls[0].(*XUnion)

			if !ok || actual == nil {
				t.Fatalf("decls[0] not a xunion, was instead %T", result.Decls[0])
			}
			if !reflect.DeepEqual(ex.expected, *actual) {
				t.Fatalf("expected %+v\nactual %+v", ex.expected, *actual)
			}
		})
	}
}

func makeLiteralConstant(value string) types.Constant {
	return types.Constant{
		Kind: types.LiteralConstant,
		Literal: types.Literal{
			Kind:  types.NumericLiteral,
			Value: value,
		},
	}
}

func makePrimitiveType(subtype types.PrimitiveSubtype) types.Type {
	return types.Type{
		Kind:             types.PrimitiveType,
		PrimitiveSubtype: subtype,
	}
}

func TestCompileConstant(t *testing.T) {
	var c compiler
	cases := []struct {
		input    types.Constant
		typ      types.Type
		expected string
	}{
		{
			input:    makeLiteralConstant("10"),
			typ:      makePrimitiveType(types.Uint32),
			expected: "10u",
		},
		{
			input:    makeLiteralConstant("10"),
			typ:      makePrimitiveType(types.Float32),
			expected: "10",
		},
		{
			input:    makeLiteralConstant("-1"),
			typ:      makePrimitiveType(types.Int16),
			expected: "-1",
		},
		{
			input:    makeLiteralConstant("0xA"),
			typ:      makePrimitiveType(types.Uint32),
			expected: "0xA",
		},
		{
			input:    makeLiteralConstant("1.23"),
			typ:      makePrimitiveType(types.Float32),
			expected: "1.23",
		},
	}
	for _, ex := range cases {
		actual := c.compileConstant(ex.input, nil, ex.typ, "")
		if ex.expected != actual {
			t.Errorf("%v: expected %s, actual %s", ex.input, ex.expected, actual)
		}
	}
}
