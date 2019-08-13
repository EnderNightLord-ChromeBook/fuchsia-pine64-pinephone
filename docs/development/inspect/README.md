Fuchsia Component Inspection
=====

Components in Fuchsia may expose structured information about themselves
conforming to the Inspect API. This document describes the concepts of
Component Inspection, the interface, the C++ language implementation
of the interface, and user-facing tools for interacting with components
that expose information.

[TOC]

# Quick Links

**Not sure where to start? [Quick Start](quickstart.md)**

* [iquery](iquery.md) &mdash; The userspace tool for inspecting components.
* [Getting started with Inspect](gsw-inspect.md) &mdash; A quick start guide.
* [VMO format](vmo-format/README.md) &mdash; Describes the Inspect VMO File Format.
* [Health checks](health.md) &mdash; Describes the health check subsystem.

# Concepts

Components may expose a tree of **Nodes**, each of which has a set of
**Properties**.

![Figure: A tree of **Nodes**s](tree.png)

## Node

A node is an exported entity within a component that may have 0 or
more children. Each node has a name, and each child of a node
must have a unique name among the children.

![Figure: A **Node**](node.png)

## Property

Nodes may have any number of properties. A property has a string key and a value
which may be any one of a number of types:

### Numeric Types

- `UintProperty` - 64-bit unsigned integer.
- `IntProperty` - 64-bit signed integer.
- `DoubleProperty` - 64-bit floating point value.

### String Types

- `StringProperty` - UTF-8 string.
- `ByteVectorProperty` - Vector of bytes.

### Array Types

- `UintArray`, `IntArray`, `DoubleArray` - An array of the corresponding numeric type.

### Histogram Types

- `LinearUintHistogram`, `LinearIntHistogram`, `LinearDoubleHistogram`

A histogram with fixed-size buckets stored in an array.

- `ExponentialUintHistogram`, `ExponentialIntHistogram`, `ExponentialDoubleHistogram`

A histogram with exponentially sized buckets stored in an array.

## Inspect File Format

The [Inspect File Format](vmo-format/README.md) is a binary format
that supports efficient insertion, modification, and deletion of Nodes and
Properties at runtime. Readers take a consistent snapshot of the contents
without communicating with writers.

## Filesystem Interface

Components by default obtain a reference to their `out/` directory in
their hub.

*Top-level* nodes are exposed as VmoFiles in the Hub ending in the extension `.inspect`.
It is customary for components to expose their primary or root tree as
`out/objects/root.inspect`.

The manager for a component's environment may expose its own information
about the component to the hub. For instance, appmgr exposes
`system_objects` for each component.

# Language Libraries

## [C++](/zircon/system/ulib/inspect)

The C++ Inspect Library provides full [writing][cpp-1] and
[reading][cpp-2] support for the Inspect File Format.

Components that write inspect data should refrain from reading that data.
Reading requires traversing the entire buffer, which is very expensive.

The `Inspector` class provides a wrapper around creating a new buffer
with one root Node that can be added to. Nodes and Properties have typed
[wrappers][cpp-3] that automatically delete the underlying data from the
buffer when they go out of scope.

The [sys\_inspect][cpp-4] library provides a simple `ComponentInspector`
singleton interface to help with the common case of exposing a single
hierarchy from the component.

The [health][cpp-5] feature supports exposing structured health information
in a format known by health checking tools.

The [test matchers][cpp-6] library provides GMock matchers for verifying
data that is read out of an Inspect hierarchy in tests.

[cpp-1]: /zircon/system/ulib/inspect/include/lib/inspect/cpp/inspect.h
[cpp-2]: /zircon/system/ulib/inspect/include/lib/inspect/cpp/reader.h
[cpp-3]: /zircon/system/ulib/inspect/include/lib/inspect/cpp/vmo/types.h
[cpp-4]: /sdk/lib/sys/inspect
[cpp-5]: /zircon/system/ulib/inspect/include/lib/inspect/cpp/health.h
[cpp-6]: /sdk/lib/inspect/testing

## [Rust](/garnet/public/rust/fuchsia-inspect)

The Rust Inspect Library provides full [writing][rust-1] and
[reading][rust-2] support for the Inspect File Format.

Components that write inspect data should refrain from reading that data.
Reading requires traversing the entire buffer, which is very expensive.

The `Inspector` class provides a wrapper around creating a new buffer
with one root Node that can be added to. Nodes and Properties have typed
[wrappers][rust-3] that automatically delete the underlying data from the
buffer when they go out of scope.

The [component][rust-4] module supports a simple `inspector` function to
handle the common use of exposing a single hierarchy from the component.

The [health][rust-5] module supports exposing structured health information
in a format known by health checking tools.

The [testing][rust-6] module supports the `assert_inspect_tree!` macro to
match Inspect data for testing.

[rust-1]: https://fuchsia-docs.firebaseapp.com/rust/fuchsia_inspect/struct.Inspector.html
[rust-2]: https://fuchsia-docs.firebaseapp.com/rust/fuchsia_inspect/reader/index.html
[rust-3]: https://fuchsia-docs.firebaseapp.com/rust/fuchsia_inspect/index.html
[rust-4]: https://fuchsia-docs.firebaseapp.com/rust/fuchsia_inspect/component/index.html
[rust-5]: https://fuchsia-docs.firebaseapp.com/rust/fuchsia_inspect/health/index.html
[rust-6]: https://fuchsia-docs.firebaseapp.com/rust/fuchsia_inspect/testing/index.html

## [Dart](https://fuchsia.googlesource.com/topaz/+/refs/heads/master/public/dart/fuchsia_inspect/)

The Dart Inspect Library provides [write][dart-1] support for the Inspect File Format.

The `Inspect` class provides a wrapper around exposing and writing
to named Inspect files on the Hub.  Nodes and Properties have typed
[wrappers][dart-2].

Node children and properties are deduplicated automatically by the
library, so creating the same named property twice simply returns a
reference to the previously existing property.

[Deletion][dart-3] is manual, but it is compatible with Futures and callbacks in Dart:

```
var item = parent.child('item');
itemDeletedFuture.then(() => item.delete());
```

[dart-1]: https://fuchsia-docs.firebaseapp.com/dart/package-fuchsia_inspect_inspect/Inspect-class.html
[dart-2]: https://fuchsia-docs.firebaseapp.com/dart/package-fuchsia_inspect_inspect/package-fuchsia_inspect_inspect-library.html
[dart-3]: https://fuchsia-docs.firebaseapp.com/dart/package-fuchsia_inspect_inspect/Node/delete.html

# Userspace Tools

The primary userspace tool is [iquery](iquery.md), which has its own
manual page.

You can use the `fx iquery` command to dump out data for the entire
system, or `fx bugreport` to generate a directory of diagnostic
information from the system (which includes inspect).
