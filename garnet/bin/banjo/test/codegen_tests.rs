// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use super::codegen_test;

mod c {
    use super::*;

    codegen_test!(alignment, CBackend, ["banjo/alignment.test.banjo"], "c/alignment.h");
    codegen_test!(empty, CBackend, ["banjo/empty.test.banjo"], "c/empty.h");
    codegen_test!(enums, CBackend, ["banjo/enums.test.banjo"], "c/enums.h");
    codegen_test!(example_0, CBackend, ["banjo/example-0.test.banjo"], "c/example-0.h");
    codegen_test!(example_1, CBackend, ["banjo/example-1.test.banjo"], "c/example-1.h");
    codegen_test!(example_2, CBackend, ["banjo/example-2.test.banjo"], "c/example-2.h");
    codegen_test!(example_3, CBackend, ["banjo/example-3.test.banjo"], "c/example-3.h");
    codegen_test!(example_4, CBackend, ["banjo/example-4.test.banjo"], "c/example-4.h");
    codegen_test!(example_6, CBackend, ["banjo/example-6.test.banjo"], "c/example-6.h");
    codegen_test!(example_7, CBackend, ["banjo/example-7.test.banjo"], "c/example-7.h");
    codegen_test!(example_8, CBackend, ["banjo/example-8.test.banjo"], "c/example-8.h");
    codegen_test!(example_9, CBackend, ["banjo/example-9.test.banjo"], "c/example-9.h");
    codegen_test!(point, CBackend, ["banjo/point.test.banjo"], "c/point.h");
    codegen_test!(table, CBackend, ["banjo/tables.test.banjo"], "c/tables.h");
    codegen_test!(simple, CBackend, ["../zx.banjo", "banjo/simple.test.banjo"], "c/simple.h");
    codegen_test!(view, CBackend, ["banjo/point.test.banjo", "banjo/view.test.banjo"], "c/view.h");
    codegen_test!(
        protocol_primative,
        CBackend,
        ["../zx.banjo", "banjo/protocol-primative.test.banjo"],
        "c/protocol-primative.h"
    );
    codegen_test!(
        protocol_base,
        CBackend,
        ["../zx.banjo", "banjo/protocol-base.test.banjo"],
        "c/protocol-base.h"
    );
    codegen_test!(
        protocol_handle,
        CBackend,
        ["../zx.banjo", "banjo/protocol-handle.test.banjo"],
        "c/protocol-handle.h"
    );
    codegen_test!(
        protocol_array,
        CBackend,
        ["../zx.banjo", "banjo/protocol-array.test.banjo"],
        "c/protocol-array.h"
    );
    codegen_test!(
        protocol_vector,
        CBackend,
        ["../zx.banjo", "banjo/protocol-vector.test.banjo"],
        "c/protocol-vector.h"
    );
    codegen_test!(
        protocol_other_types,
        CBackend,
        ["../zx.banjo", "banjo/protocol-other-types.test.banjo"],
        "c/protocol-other-types.h"
    );
    codegen_test!(
        interface,
        CBackend,
        ["../zx.banjo", "banjo/interface.test.banjo"],
        "c/interface.h"
    );
    codegen_test!(callback, CBackend, ["../zx.banjo", "banjo/callback.test.banjo"], "c/callback.h");
}

mod cpp {
    use super::*;

    codegen_test!(empty, CppBackend, ["banjo/empty.test.banjo"], "cpp/empty.h");
    codegen_test!(example_4, CppBackend, ["banjo/example-4.test.banjo"], "cpp/example-4.h");
    codegen_test!(example_6, CppBackend, ["banjo/example-6.test.banjo"], "cpp/example-6.h");
    codegen_test!(example_7, CppBackend, ["banjo/example-7.test.banjo"], "cpp/example-7.h");
    codegen_test!(example_9, CppBackend, ["banjo/example-9.test.banjo"], "cpp/example-9.h");
    codegen_test!(simple, CppBackend, ["../zx.banjo", "banjo/simple.test.banjo"], "cpp/simple.h");
    codegen_test!(
        view,
        CppBackend,
        ["banjo/point.test.banjo", "banjo/view.test.banjo"],
        "cpp/view.h"
    );
    codegen_test!(
        protocol_primative,
        CppBackend,
        ["../zx.banjo", "banjo/protocol-primative.test.banjo"],
        "cpp/protocol-primative.h"
    );
    codegen_test!(
        protocol_base,
        CppBackend,
        ["../zx.banjo", "banjo/protocol-base.test.banjo"],
        "cpp/protocol-base.h"
    );
    codegen_test!(
        protocol_handle,
        CppBackend,
        ["../zx.banjo", "banjo/protocol-handle.test.banjo"],
        "cpp/protocol-handle.h"
    );
    codegen_test!(
        protocol_array,
        CppBackend,
        ["../zx.banjo", "banjo/protocol-array.test.banjo"],
        "cpp/protocol-array.h"
    );
    codegen_test!(
        protocol_vector,
        CppBackend,
        ["../zx.banjo", "banjo/protocol-vector.test.banjo"],
        "cpp/protocol-vector.h"
    );
    codegen_test!(
        protocol_other_types,
        CppBackend,
        ["../zx.banjo", "banjo/protocol-other-types.test.banjo"],
        "cpp/protocol-other-types.h"
    );
    codegen_test!(
        interface,
        CppBackend,
        ["../zx.banjo", "banjo/interface.test.banjo"],
        "cpp/interface.h"
    );
    codegen_test!(
        callback,
        CppBackend,
        ["../zx.banjo", "banjo/callback.test.banjo"],
        "cpp/callback.h"
    );
}

mod cpp_internal {
    use super::*;

    codegen_test!(empty, CppInternalBackend, ["banjo/empty.test.banjo"], "cpp/empty-internal.h");
    codegen_test!(
        example_4,
        CppInternalBackend,
        ["banjo/example-4.test.banjo"],
        "cpp/example-4-internal.h"
    );
    codegen_test!(
        example_6,
        CppInternalBackend,
        ["banjo/example-6.test.banjo"],
        "cpp/example-6-internal.h"
    );
    codegen_test!(
        example_7,
        CppInternalBackend,
        ["banjo/example-7.test.banjo"],
        "cpp/example-7-internal.h"
    );
    codegen_test!(
        example_9,
        CppInternalBackend,
        ["banjo/example-9.test.banjo"],
        "cpp/example-9-internal.h"
    );
    codegen_test!(
        simple,
        CppInternalBackend,
        ["../zx.banjo", "banjo/simple.test.banjo"],
        "cpp/simple-internal.h"
    );
    codegen_test!(
        view,
        CppInternalBackend,
        ["banjo/point.test.banjo", "banjo/view.test.banjo"],
        "cpp/view-internal.h"
    );
    codegen_test!(
        protocol_primative,
        CppInternalBackend,
        ["../zx.banjo", "banjo/protocol-primative.test.banjo"],
        "cpp/protocol-primative-internal.h"
    );
    codegen_test!(
        protocol_base,
        CppInternalBackend,
        ["../zx.banjo", "banjo/protocol-base.test.banjo"],
        "cpp/protocol-base-internal.h"
    );
    codegen_test!(
        protocol_handle,
        CppInternalBackend,
        ["../zx.banjo", "banjo/protocol-handle.test.banjo"],
        "cpp/protocol-handle-internal.h"
    );
    codegen_test!(
        protocol_array,
        CppInternalBackend,
        ["../zx.banjo", "banjo/protocol-array.test.banjo"],
        "cpp/protocol-array-internal.h"
    );
    codegen_test!(
        protocol_vector,
        CppInternalBackend,
        ["../zx.banjo", "banjo/protocol-vector.test.banjo"],
        "cpp/protocol-vector-internal.h"
    );
    codegen_test!(
        protocol_other_types,
        CppInternalBackend,
        ["../zx.banjo", "banjo/protocol-other-types.test.banjo"],
        "cpp/protocol-other-types-internal.h"
    );
    codegen_test!(
        interface,
        CppInternalBackend,
        ["../zx.banjo", "banjo/interface.test.banjo"],
        "cpp/interface-internal.h"
    );
    codegen_test!(
        callback,
        CppInternalBackend,
        ["../zx.banjo", "banjo/callback.test.banjo"],
        "cpp/callback-internal.h"
    );
}

mod rust {
    use super::*;

    codegen_test!(alignment, RustBackend, ["banjo/alignment.test.banjo"], "rust/alignment.rs");
    codegen_test!(empty, RustBackend, ["banjo/empty.test.banjo"], "rust/empty.rs");
    codegen_test!(enums, RustBackend, ["banjo/enums.test.banjo"], "rust/enums.rs");
    codegen_test!(example_0, RustBackend, ["banjo/example-0.test.banjo"], "rust/example-0.rs");
    codegen_test!(example_1, RustBackend, ["banjo/example-1.test.banjo"], "rust/example-1.rs");
    codegen_test!(example_2, RustBackend, ["banjo/example-2.test.banjo"], "rust/example-2.rs");
    codegen_test!(example_3, RustBackend, ["banjo/example-3.test.banjo"], "rust/example-3.rs");
    codegen_test!(example_4, RustBackend, ["banjo/example-4.test.banjo"], "rust/example-4.rs");
    codegen_test!(example_6, RustBackend, ["banjo/example-6.test.banjo"], "rust/example-6.rs");
}

