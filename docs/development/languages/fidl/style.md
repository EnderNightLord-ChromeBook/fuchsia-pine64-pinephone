# FIDL Style Guide

This section contains style-related information for
[Fuchsia Interface Definition Language](/docs/development/languages/fidl/README.md) files.

[TOC]

## Names

```
The Naming of Cats is a difficult matter,
It isn't just one of your holiday games;
  --- T.S. Eliot
```

Names defined in FIDL are used to generate identifiers in each target language.
Some languages attach semantic or conventional meaning to names of various
forms.  For example, in Go, whether the initial letter in an identifier is
capitalized controls the visibility of the identifier.  For this reason, many of
the language back ends transform the names in your library to make them more
appropriate for their target language.  The naming rules in this section are a
balancing act between readability in the FIDL source, usability in each target
language, and consistency across target languages.

Avoid commonly reserved words, such as `goto`.  The language back ends will
transform reserved words into non-reserved identifiers, but these transforms
reduce usability in those languages.  Avoiding commonly reserved words reduces
the frequency with which these transformations are applied.

While some FIDL keywords are also commonly reserved words in target languages,
(such as `struct` in C and C++), and should thus be avoided, other FIDL
keywords, particularly `request` and `handle`, are generally descriptive and
can be used as appropriate.

Names must not contain leading or trailing underscores.  Leading or trailing
underscores have semantic meaning in some languages (e.g., leading underscores
control visibility in Dart) and conventional meaning in other languages (e.g.,
trailing underscores are conventionally used for member variables in C++).
Additionally, the FIDL compiler uses leading and trailing underscores to munge
identifiers to avoid collisions.

Use the term `size` to name a number of bytes. Use the term `count` to name a
number of some other quantity (e.g., the number of items in a vector of
structs).

### Case definitions

Sometimes there is more than one way to decide on how to delimit words in
identifiers.  Our style is as follows:

 * Start with the original phrase in US English (e.g., "Non-Null HTTP Client")
 * Remove any punctuation. ("Non Null HTTP Client")
 * Make everything lowercase ("non null http client")
 * Do one of the following, depending on what style is appropriate for the given
   identifier:
    * Replace spaces with underscores ('_') for _lower snake case_
      (`non_null_http_client`).
    * Capitalize and replace spaces with underscores for _upper snake case_
      (`NON_NULL_HTTP_CLIENT`).
    * Capitalize the first letter of each word and join all words together for
      _upper camel case_ (`NonNullHttpClient`).

#### Usage

The following table maps the case usage to the element:

Element                    | Casing             | Example
---------------------------|--------------------|-----------------
`bits`                     | _upper camel case_ | `InfoFeatures`
bitfield members           | _upper snake case_ | `WLAN_SNOOP`
`const`                    | _upper snake case_ | `MAX_NAMES`
primitive alias            | _lower snake case_ | `hw_partition`
`protocol`                 | _upper camel case_ | `AudioRenderer`
protocol method parameters | _lower snake case_ | `enable_powersave`
protocol methods           | _upper camel case_ | `GetBatteryStatus`
`struct`                   | _upper camel case_ | `KeyboardEvent`
struct members             | _lower snake case_ | `child_pid`
`table`                    | _upper camel case_ | `ComponentDecl`
table members              | _lower snake case_ | `num_rx`
`union`                    | _upper camel case_ | `BufferFormat`
union members              | _lower snake case_ | `vax_primary`
`xunion`                   | _upper camel case_ | `ZirconHandle`
xunion members             | _lower snake case_ | `pdp8_iot`
`enum`                     | _upper camel case_ | `PixelFormat`
enum members               | _upper snake case_ | `RGB_888`

### Libraries

#### Syntax

Library names are period-separated lists of identifiers. Portions of the library
name other than the last are also referred to as namespaces.  Each component of
the name is in lowercase and must match the following regular expression:
`[a-z][a-z0-9]*`.

We use these restrictive rules because different target languages have different
restrictions on how they qualify namespaces, libraries, or packages.  We have
selected a conservative least common denominator in order for FIDL to work well
with our current set of target languages and with potential future target
languages.

#### Identifier Names: Prefer Functional Roles with Meaning

Prefer functional names (e.g., `fuchsia.media`) over product or code names
(e.g., `fuchsia.amber` or `fuchsia.scenic`).  Product names are appropriate
when the product has some external existence beyond Fuchsia and when the
protocol is specific to that product.  For example, `fuchsia.cobalt` is a
better name for the Cobalt interface protocol than `fuchsia.metrics` because
other metrics implementations (e.g., Firebase) are unlikely to implement the same
protocol.

Identifier names should relate to the specific *role* that participants play;
avoid encoding access control into the name. Names based on roles are
descriptive and won't outdate as quickly as names based on access control, which
prescribe an externally-defined relationship that is subject to change as the
platform evolves. For example, for an API involving `FocusChain` objects, an
appropriate name would be `fuchsia.ui.focus`, instead of
`fuchsia.ui.privileged`; if we decide to make `FocusChain` objects more widely
accessible, then `fuchsia.ui.focus` isn't a problematic name.  The following
example words should be avoided:

* `constrained`, `limited`, `oem`,  `private`, `privileged`, `protected`,
  `special`, `vendor`.

Identifier names should have meaning; avoid meaningless names.  If
`fuchsia.foo.bar` and `fuchsia.foo.baz` share a number of concepts that you
wish to factor out into a separate library, consider defining those concepts in
`fuchsia.foo` rather than in `fuchsia.foo.common`.  The following example words
should be avoided:

* `common`, `service`, `util`, `base`, `f<letter>l`, `zx<word>`.

#### Fuchsia Libraries

FIDL libraries defined in the Platform Source Tree (i.e., defined in
fuchsia.googlesource.com) must be in the `fuchsia` top-level namespace (e.g.,
`fuchsia.ui`) unless (a) the library defines portions of the FIDL language
itself or its conformance test suite, in which case the top-level namespace must
be `fidl`, or (b) the library is used only for internal testing and is not
included in the SDK or in production builds, in which case the top-level
namespace must be `test`.

FIDL libraries defined in the Platform Source Tree for the purpose of exposing
hardware functionality to applications must be in the `fuchsia.hardware`
namespace.  For example, a protocol for exposing an ethernet device might
be named `fuchsia.hardware.ethernet.Device`.  Higher-level functionality built
on top of these FIDL protocols does not belong in the `fuchsia.hardware` namespace.
For example, it is more appropriate for network protocols to be under
`fuchsia.net` than `fuchsia.hardware`.

#### Namespace Nesting: Not Too Deeply

Avoid library names with more than two dots (e.g., `fuchsia.foo.bar.baz`).
There are some cases when a third dot is appropriate, but those cases are rare.
If you use more than two dots, you should have a specific reason for that
choice.  For the case of the `fuchsia.hardware` namespace described above, this
is relaxed to "three" and "four" dots, instead of "two" and "three", to
accomodate the longer namespace.

#### Library Dependencies

Prefer to introduce dependencies from libraries with more specific names to
libraries with less specific names rather than the reverse.  For example,
`fuchsia.foo.bar` might depend on `fuchsia.foo`, but `fuchsia.foo` should not
depend on `fuchsia.foo.bar`.  This pattern is better for extensibility because
over time we can add more libraries with more specific names but there are only
a finite number of libraries with less specific names.  Having libraries with
less specific names know about libraries with more specific names privileges the
current status quo relative to the future.

### Top-level

Avoid repeating the names from the library name.  For example, in the
`fuchsia.process` library, a protocol that launches process should be named
`Launcher` rather than `ProcessLauncher` because the name `process` already
appears in the library name.  In all target languages, top-level names are
scoped by the library name in some fashion.

### Primitive aliases

Primitive aliases must not repeat names from the enclosing library.  In all
target languages, primitive aliases are replaced by the underlying primitive
type and therefore do not cause name collisions.

```fidl
using vaddr = uint64;
```

### Constants

Constant names must not repeat names from the enclosing library.  In all target
languages, constant names are scoped by their enclosing library.

Constants that describe minimum and maximum bounds should use the prefix `MIN_`
and `MAX_`, respectively.

```fidl
const uint64 MAX_NAMES = 32;
```

### Protocols

Protocols are specified with the `protocol` keyword.

Protocols must be noun phrases.
Typically, protocols are named using nouns that suggest an action.  For
example, `AudioRenderer` is a noun that suggests that the protocol is related
to rendering audio.  Similarly, `Launcher` is a noun that suggests that the
protocol is related to launching something.  Protocols can also be passive
nouns, particularly if they relate to some state held by the implementation.
For example, `Directory` is a noun that suggests that the protocol is used for
interacting with a directory held by the implementation.

A protocol may be named using object-oriented design patterns.  For example,
`fuchsia.fonts.Provider` uses the "provider" suffix, which indicates that the
protocol provides fonts (rather than represents a font itself).  Similarly,
`fuchsia.tracing.Controller` uses the "controller" suffix, which indicates that
the protocol controls the tracing system (rather than represents a trace
itself).

The name `Manager` may be used as a name of last resort for a protocol with
broad scope.  For example, `fuchsia.power.Manager`.  However, be warned that
"manager" protocols tend to attract a large amount of loosely related
functionality that might be better factored into multiple protocols.

Protocols must not include the name "service."  All protocols define services.
The term is meaningless.  For example, `fuchsia.net.oldhttp.HttpService`
violates this rubric in two ways.  First, the "http" prefix is redundant with
the library name.  Second, the "service" suffix is banned.
Notice that the successor library simply omits this altogether by being
explicit in naming the service it offers `fuchsia.net.http.Loader`.

#### Methods

Methods must must be verb phrases.
For example, `GetBatteryStatus` and `CreateSession` are verb phrases that
indicate what action the method performs.

Methods on "listener" or "observer" protocols that are called when an event
occurs should be prefixed with `On` and describe the event that occurred in the
past tense.  For example, the `ViewContainerListener` protocol has a method
named `OnChildAttached`.

#### Events

Similarly, events (i.e., unsolicited messages from the server to the client)
should be prefixed with `On` and describe the event that occurred in the past
tense.
For example, the `AudioCapturer` protocol has an event named
`OnPacketCaptured`.

### Structs, unions, xunions, and tables

Structs, unions, xunions, and tables must be noun phrases.
For example, `Point` is a struct that defines a location in space and
`KeyboardEvent` is a struct that defines a keyboard-related event.

### Struct, union, xunion, and table members

Prefer struct, union, xunion, and table member names with a single word when
practical (single-word names render more consistently across target languages).
However, do not be afraid to use multiple words if a single word would be
ambiguous or confusing.

Member names must not repeat names from the enclosing type (or library) unless
the member name is ambiguous without a name from the enclosing type.  For
example, a member of type `KeyboardEvent` that contains the time the event was
delivered should be named `time`, rather than `event_time`, because the name
`event` already appears in the name of the enclosing type. In all target
languages, member names are scoped by their enclosing type.

However, a type `DeviceToRoom`--that associates a smart device with the room
it's located in--may need to have members `device_id` and `room_name`, because
`id` and `name` are ambiguous; they could refer to either the device or
the room.

### Enums

Enums must be noun phrases.
For example, `PixelFormat` is an enum that defines how colors are encoded
into bits in an image.

### Enum members

Enum member names must not repeat names from the enclosing type (or library).
For example, members of `PixelFormat` enum should be named `ARGB` rather than
`PIXEL_FORMAT_ARGB` because the name `PIXEL_FORMAT` already appears in the name
of the enclosing type.  In all target languages, enum member names are scoped by
their enclosing type.

### Bitfields

Bitfields must be noun phrases.
For example, `InfoFeatures` is a bitfield that indicates which features
are present on an Ethernet interface.

### Bitfield members

Bitfield members must not repeat names from the enclosing type (or library).
For example, members of `InfoFeatures` bitfield should be named `WLAN`
rather than `INFO_FEATURES_WLAN` because the name `INFO_FEATURES` already
appears in the name of the enclosing type.
In all target languages, bitfield member names are scoped by their
enclosing type.

## Organization

### Syntax

 * Use 4 space indents.
 * Never use tabs.
 * Avoid trailing whitespace.
 * Separate declarations for `bits`, `enum`, `protocol`, `struct`, `table`,
   `table`, `union`, and `xunion` constructs from other declarations with
   one blank line (two consecutive newline characters).
 * End files with exactly one newline character.

### Comments

Comments use `///` (three forward slashes). Comments in a library will also
appear in the generated code to ease development when coding against the
library. We say that comments "flow-through" to the target language.

Place comments above the thing being described. Except in the cases listed
below, use reasonably complete sentences with proper capitalization and
periods. Limit comment widths to 80 characters unless a longer comment is
unavoidable (e.g., for a long URL).

Comments should be written in Markdown. We rely on the
[CommonMark](http://www.commonmark.org) specification for our markdown. Some
tools may render output using other Markdown standards; in cases where your tool
does not use CommonMark, we encourage developers to write Markdown that is
compatible with both CommonMark and their tool. References to FIDL elements
should always be in code font.

A documented entity is any FIDL element that has a comment attached. The first
reference to any documented entity in a comment should be given with its fully
qualified name, in the form ``[`<library>/<top level declaration>.<member>`]``
(e.g., ``[`fuchsia.io/Node.clone`]``). This form may generate a hyperlink, if
the tooling supports it.  Subsequent references to that documented entity can
use an abbreviated version, as long as that abbreviated version is unambiguous
(e.g., `clone`). The form without brackets does not generate a hyperlink.


Request parameters, response parameters, and error types should be documented as
lists of the form:

```fidl
+ request `param1` <description>
+ request `param2` <description>
- response `param1` <description>
- response `param2` <description>
* error <description>
```

Requests, responses, and errors must appear in that order. A given set of
parameters must also follow the order in which they were declared in the
parameter list.  The terms "request" and "response" may be elided if the
parameter names are only found in one of the request or response parameter list.

The first part of a doc comment describing a variable, field, or type should be
a noun phrase that briefly states the intended purpose of the documented entity,
including information that cannot be deduced from the name and type. The
description should be terminated with a period. The description should not
reiterate the name of the documented entity, or its particular type of FIDL
language element (e.g., `struct` or `protocol`).

```fidl
/// A representation of violins displayed on the screen.
struct Widget {
    /// A monotonically increasing id, uniquely identifying the widget.
    uint64 id;
    /// Location of the top left corner of the widget.
    Point location;
};
```

The following are examples of what you should not do:

```fidl
/// BAD: Widget is a representation of violins displayed on the screen.
/// BAD: struct Widget is a representation of violins displayed on the screen.
```

The first part of a doc comment attached to a protocol method should be a brief
description of the behavior of that method, starting with a verb, including
information that cannot be deduced from the name and type. The verb should be
written in the present tense, agree with a third person singular pronoun, and
use the indicative mood (this effectively means that you should pretend the word
"it" comes before the verb, and that you are making a statement of fact).  The
phrase should end with a period.

A full example:

```fidl

/// An abstract representation of a [`fuchsia.io/Node`] whose layout is flat.
protocol File {
    compose Node;

    /// Acquires a [`fuchsia.mem/Buffer`] representing this file, if
    /// there is one, with the requested access rights.
    ///
    /// ## Rights
    ///
    /// This method requires the following rights:
    ///
    /// * [`fuchsia.io/OPEN_RIGHT_WRITABLE`] if `flags` includes
    ///   [`fuchsia.io/VMO_FLAG_WRITE`].
    /// * [`fuchsia.io/OPEN_RIGHT_READABLE`] if `flags` includes
    ///   [`fuchsia.io/VMO_FLAG_READ`] or [`fuchsia.io/VMO_FLAG_EXEC`].
    ///
    /// + request `flags` a bit field composing any of
    ///     `VMO_FLAG_READ`, `VMO_FLAG_WRITE`, or `VMO_FLAG_EXEC`.
    /// - response `buffer` the requested `fuchsia.mem/Buffer`, or
    ///     null if there was an error, or the buffer does not exist.
    /// * error a zx_status value indicating success or failure.
    /// * see [`fuchsia.mem/Buffer`]
    /// [`fuchsia.mem/Buffer`]:
    ///    https://fuchsia.googlesource.com/fuchsia/+/9853fad50ca70256f0e86201c0e20424f1c25ab5/zircon/system/fidl/fuchsia-io/io.fidl
    GetBuffer(uint32 flags) ->
        (fuchsia.mem.Buffer? buffer) error zx.status;
};
```

Types or values defined by some external source of truth should be commented
with references to the external thing. For example, reference the WiFi
specification that describes a configuration structure.  Similarly, if a
structure must match an ABI defined in a C header, reference the C header.

For more information about what your comments should contain, consult the [API
Documentation Rubric](/docs/development/api/documentation.md).

#### Referencing FIDL protocols or protocol methods

References to FIDL protocols or their methods in comments should follow the
pattern:

```fidl
/// See fuchsia.library/ProtocolName.Method for more information.
```

When referring to a protocol in the same library as the comment, the library
name may be left off: `ProtocolName.Method`.

Similarly, when referring to a method in the same protocol as the comment,
the library name and protocol name may be left off: `Method`.

#### Non flow-through comments

If your comments are meant for library authors, use the simpler comments `//`
(two forward slashes) which do not flow-through to the target language.

When deciding what should be a regular `///` comment versus a non flow-through
comment, keep in mind the following.

Regular comments:

 * descriptions of parameters, arguments, function
 * usage notes

Non flow-through comments:

 * internal "todo" comments
 * copyright notices
 * implementation details

Both style of comments can be combined:

```fidl
/// A widget displaying violins on the screen.
// TODO -- widgets should use UUIDs instead of sequential ids
struct Widget {
    /// A monotonically increasing id, uniquely identifying the widget.
    uint64 id;
    /// Location of the top left corner of the widget.
    ...
};
```

### Files

A library is comprised of one or more files.  The files are stored in a
directory hierarchy with the following conventions:

```fidl
fidl/<library>/[<dir>/]*<file>.fidl
```

The `<library>` directory is named using the dot-separated name of the FIDL
library.  The `<dir>` subdirectories are optional and typically not used for
libraries with less than a dozen files.  This directory structure matches how
FIDL files are included in the Fuchsia SDK.

The division of a library into files has no technical impact on consumers of the
library.  Declarations, including protocols, can reference each other and
themselves throughout the library, regardless of the file in which they appear.
Divide libraries into files to maximize readability.

 * Prefer a DAG dependency diagram for files in a library.

 * Prefer keeping mutually referring definitions textually close to each other,
   ideally in the same file.

 * For complex libraries, prefer defining pure data types or constants in leaf
   files and defining protocols that reference those types together in a trunk
   file.
