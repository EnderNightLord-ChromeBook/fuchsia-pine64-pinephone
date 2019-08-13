# Low-Level C++ Language FIDL Tutorial

[TOC]

## About this tutorial

This tutorial describes how to make client calls and write servers in C++
using the Low-Level C++ Bindings (LLCPP).

[Getting Started](#getting-started) has a walk-through of using the bindings
with an example FIDL library. The [reference](#reference) section documents
the detailed bindings interface and design.

See [Comparing C, Low-Level C++, and High-Level C++ Language
Bindings](c-family-comparison.md) for a comparative analysis of the goals and
use cases for all the C-family language bindings.

Note: LLCPP is in currently in beta. The bindings are designed to exploit the
compatibility between FIDL wire-format and C++ memory layouts, and offer
precise control over allocation. As such, viewers are encouraged to
familiarize themselves with the [C Language Bindings](tutorial-c.md#reference)
and the [FIDL wire-format](../reference/wire-format/README.md). Parts of this
tutorial assume knowledge of these related concepts.

# Getting Started

Two build setups exist in the source tree: the Zircon build and the Fuchsia
build. The LLCPP code generator is not supported by the Zircon build. Therefore,
the steps to use the bindings depend on where the consumer code is located:

*   **Code is outside `zircon/`:**
    Add `//[library path]:[library name]_llcpp` to the GN dependencies e.g.
    `"//sdk/fidl/fuchsia.math:fuchsia.math_llcpp"`, and the bindings code
    will be automatically generated as part of the build.
*   **Code is inside `zircon/`:**
    Add a GN dependency of the form: `"$zx/system/fidl/[library-name]:llcpp"`.
    Run a special [command](/tools/fidlgen_llcpp_zircon/README.md) which
    extracts the set of FIDL libraries used through LLCPP in Zircon, and builds
    and runs the code generator during the Fuchsia build phase. The generated
    code is placed in a `gen` folder next to the corresponding FIDL definition,
    and has to be checked into source control
    ([example](/zircon/system/fidl/fuchsia-io/gen/llcpp)). Whenever the FIDL
    library changes, re-run the command to update the checked in bindings.

## Preliminary Concepts

*   **Decoded Message:**
    A FIDL message in [decoded form](../reference/wire-format#Decoded-Messages)
    is a contiguous buffer that is directly accessible by reinterpreting the
    memory as the corresponding LLCPP FIDL type. That is, all pointers point
    within the same buffer, and the pointed objects are in a specific order
    defined by the FIDL wire-format. When making a call, a response buffer is
    used to decode the response message.

*   **Encoded Message:**
    A FIDL message in [encoded form](../reference/wire-format#Encoded-Messages)
    is an opaque contiguous buffer plus an array of handles. The buffer is
    of the same length as the decoded counterpart, but pointers are replaced
    with placeholders, and handles are moved to the accompanying array.
    When making a call, a request buffer is used to encode the request message.

*   **Message Linearization:**
    FIDL messages have to be in a contiguous buffer packed according to the
    wire-format. When making a call however, the arguments to the bindings code
    and out-of-line objects are usually scattered in memory, unless careful
    attention is spent to follow the wire-format order. The process of walking
    down the tree of objects and packing them is termed *linearization*, and
    usually involves `O(message size)` copying.

*   **Message Ownership:**
    Crucially, LLCPP generated structures are views over some underlying buffer;
    they do not own memory or handles located out-of-line. In practice, one
    must ensure the object managing the buffer outlives the views.

## Generated API Overview

Low-Level C++ bindings are full featured, and support control over allocation as
well as zero-copy encoding/decoding. (Note that contrary to the C bindings they
are meant to replace, the LLCPP bindings cover non-simple messages.)

Let's use this FIDL protocol as a motivating example:

```fidl
// fleet.fidl
library fuchsia.fleet;

struct Planet {
    string name;
    float64 mass;
    handle<channel> radio;
};
```

The following code is generated (simplified for readability):

```cpp
// fleet.h
struct Planet {
  fidl::StringView name;
  double mass;
  zx::channel radio;
};
```

Note that `string` maps to `fidl::StringView`, hence the `Planet` struct
will not own the memory associated with the `name` string. Rather, all strings
point within some buffer space that is managed by the bindings library, or that
the caller could customize. The same goes for the `fidl::VectorView<Planet>`
in the code below.

Continuing with the FIDL protocol:

```fidl
// fleet.fidl continued...
protocol SpaceShip {
    SetHeading(int16 heading);
    ScanForPlanets() -> (vector<Planet> planets);
};
```

The following code is generated (simplified for readability):

```cpp
// fleet.h continued...
class SpaceShip final {
 public:
  struct SetHeadingRequest final {
    fidl_message_header_t _hdr;
    int16_t heading;
  };

  struct ScanForPlanetsResponse final {
    fidl_message_header_t _hdr;
    fidl::VectorView<Planet> planets;
  };
  using ScanForPlanetsRequest = fidl::AnyZeroArgMessage;

  class SyncClient final { /* ... */ };
  class Call final { /* ... */ };
  class Interface { /* ... */ };

  static bool TryDispatch(Interface* impl, fidl_msg_t* msg, fidl::Transaction* txn);
  static bool Dispatch(Interface* impl, fidl_msg_t* msg, fidl::Transaction* txn);

  class ResultOf final { /* ... */ };
  class UnownedResultOf final { /* ... */ };
  class InPlace final { /* ... */ };
};
```

Notice that every request and response is modelled as a `struct`:
`SetHeadingRequest`, `ScanForPlanetsResponse`, etc.
In particular, `ScanForPlanets()` has a request that contains no arguments, and
we provide a special type for that, `fidl::AnyZeroArgMessage`.

Following those, there are three related concepts in the generated code:

+ [`SyncClient`](#sync-client): A class that owns a Zircon channel, providing
  methods to make requests to the FIDL server.
+ [`Call`](#static-functions): A class that contains static functions to make
  sync FIDL calls directly on an unowned channel, avoiding setting up a
  `SyncClient`. This is similar to the simple client wrappers from the C
  bindings, which take a `zx_handle_t`.
+ `Interface` and `[Try]Dispatch`: A server should implement the `Interface`
  pure virtual class, which allows `Dispatch` to call one of the defined
  handlers with a received FIDL message.

[`[Unowned]ResultOf`](#resultof-and-unownedresultof) are "scoping" classes
containing return type definitions of FIDL calls inside `SyncClient` and `Call`.
This allows one to conveniently write `ResultOf::SetHeading` to denote the
result of calling `SetHeading`.

[`InPlace`](#in_place-calls) is another "scoping" class that houses functions
to make a FIDL call with encoding and decoding performed in-place directly on
the user buffer. It is more efficient than those `SyncClient` or `Call`, but
comes with caveats. We will dive into these separately.

## Client API

### Sync Client `(Protocol::SyncClient)`

The following code is generated for `SpaceShape::SyncClient`. Each FIDL method
always correspond to two overloads which differ in memory management strategies,
termed *flavors* in LLCPP: *managed flavor* and *caller-allocating flavor*.

```cpp
class SyncClient final {
 public:
  SyncClient(zx::channel channel);

  // FIDL: SetHeading(int16 heading);
  ResultOf::SetHeading SetHeading(int16_t heading);
  UnownedResultOf::SetHeading SetHeading(fidl::BytePart request_buffer, int16_t heading);

  // FIDL: ScanForPlanets() -> (vector<Planet> planets);
  ResultOf::ScanForPlanets ScanForPlanets();
  UnownedResultOf::ScanForPlanets ScanForPlanets(fidl::BytePart response_buffer);
};
```

The one-way FIDL method `SetHeading(int16 heading)` maps to:

+ `ResultOf::SetHeading SetHeading(int16_t heading)`:
This is the *managed flavor*.
Buffer allocation for requests and responses are entirely handled within this
function, as is the case in simple C bindings. The bindings calculate a safe
buffer size specific to this call at compile time based on FIDL wire-format and
maximum length constraints. The buffers are allocated on the stack if they fit
under 512 bytes, or else on the heap. Here is an example of using it:

```cpp
// Create a client from a Zircon channel.
SpaceShip::SyncClient client(zx::channel(client_end));

// Calling |SetHeading| with heading = 42.
SpaceShip::ResultOf::SetHeading result = client.SetHeading(42);

// Check the transport status (encoding error, channel writing error, etc.)
if (result.status() != ZX_OK) {
  // Handle error...
}
```

In general, the managed flavor is easier to use, but may result in extra
allocation. See [ResultOf](#resultof-and-unownedresultof) for details on buffer
management.

+ `UnownedResultOf::SetHeading SetHeading(fidl::BytePart request_buffer, int16_t heading)`:
This is the *caller-allocating flavor*, which defers all memory allocation
responsibilities to the caller.
Here we see an additional parameter `request_buffer` which is always the first
argument in this flavor. The type `fidl::BytePart` references a buffer address
and size. It will be used by the bindings library to construct the FIDL request,
hence it must be sufficiently large.
The method parameters (e.g. `heading`) are *linearized* to appropriate locations
within the buffer. If `SetHeading` had a return value, this flavor would ask for
a `response_buffer` too, as the last argument. Here is an example of using it:

```cpp
// Call SetHeading with an explicit buffer, there are multiple ways...

// 1. On the stack
fidl::Buffer<SetHeadingRequest> request_buffer;
auto result = client.SetHeading(request_buffer.view(), 42);

// 2. On the heap
auto request_buffer = std::make_unique<fidl::Buffer<SetHeadingRequest>>();
auto result = client.SetHeading(request_buffer->view(), 42);

// 3. Some other means, e.g. thread-local storage
constexpr uint32_t request_size = fidl::MaxSizeInChannel<SetHeadingRequest>();
uint8_t* buffer = allocate_buffer_of_size(request_size);
fidl::BytePart request_buffer(/* data = */buffer, /* capacity = */request_size);
auto result = client.SetHeading(std::move(request_buffer), 42);

// Check the transport status (encoding error, channel writing error, etc.)
if (result.status() != ZX_OK) {
  // Handle error...
}

// Don't forget to free the buffer at the end if approach #3 was used...
```

> When the caller-allocating flavor is used, the `result` object borrows the
> request and response buffers (hence its type is under `UnownedResultOf`).
> Make sure the buffers outlive the `result` object.
> See [UnownedResultOf](#resultof-and-unownedresultof).

Caution: Buffers passed to the bindings must be aligned to 8 bytes. The
`fidl::Buffer` helper class does this automatically. Failure to align would
result in a run-time error.

* * * *

The two-way FIDL method
`ScanForPlanets() -> (vector<Planet> planets)` maps to:

+ `ResultOf::ScanForPlanets ScanForPlanets()`:
This is the *managed flavor*. Different from the C bindings, response arguments
are not returned via out-parameters. Instead, they are accessed through the
return value. Here is an example to illustrate:

```cpp
// It is cleaner to omit the |UnownedResultOf::ScanForPlanets| result type.
auto result = client.ScanForPlanets();

// Check the transport status (encoding error, channel writing error, etc.)
if (result.status() != ZX_OK) {
  // handle error & early exit...
}

// Obtains a pointer to the response struct inside |result|.
// This requires that the transport status is |ZX_OK|.
SpaceShip::ScanForPlanetsResponse* response = result.Unwrap();

// Access the |planets| response vector in the FIDL call.
for (const auto& planet : response->planets) {
  // Do something with |planet|...
}
```

> When the managed flavor is used, the returned object (`result` in this
> example) manages ownership of all buffer and handles, while `result.Unwrap()`
> returns a view over it. Therefore, the `result` object must outlive any
> references to the response.

+ `UnownedResultOf::ScanForPlanets ScanForPlanets(fidl::BytePart response_buffer)`:
The *caller-allocating flavor* receives the message into `response_buffer`.
Here is an example using it:

```cpp
fidl::Buffer<ScanForPlanetsResponse> response_buffer;
auto result = client.ScanForPlanets(response_buffer.view());
if (result.status() != ZX_OK) { /* ... */ }
auto response = result.Unwrap();
// |response->planets| points to a location within |response_buffer|.
```

> The buffers passed to caller-allocating flavor do not have to be initialized.
> A buffer may be re-used multiple times, as long as it is large enough for
> the calls involved.

Note: Since each `Planet` has a handle `zx::channel radio`, and the
`fidl::VectorView<Planet>` type does not own the individual `Planet` objects,
there needs to be a reliable way to capture the lifetime of those handles.
Here the return value `result` owns them, and takes care of closing them when
it goes out of scope.
If any handle is `std::move`ed away, `result` would not accidentally close it.

### Static Functions `(Protocol::Call)`

The following code is generated for `SpaceShape::Call`:

```cpp
class Call final {
 public:
  static ResultOf::SetHeading
  SetHeading(zx::unowned_channel client_end, int16_t heading);
  static UnownedResultOf::SetHeading
  SetHeading(zx::unowned_channel client_end, fidl::BytePart request_buffer, int16_t heading);

  static ResultOf::ScanForPlanets
  ScanForPlanets(zx::unowned_channel client_end);
  static UnownedResultOf::ScanForPlanets
  ScanForPlanets(zx::unowned_channel client_end, fidl::BytePart response_buffer);
};
```

These methods are similar to those found in `SyncClient`. However, they do not
own the channel. This is useful if one is migrating existing code from the
C bindings to low-level C++. Another use case is when implementing C APIs
which take a raw `zx_handle_t`. For example:

```cpp
// C interface which does not own the channel.
zx_status_t spaceship_set_heading(zx_handle_t spaceship, int16_t heading) {
  auto result = fuchsia::fleet::SpaceShip::Call::SetHeading(
      zx::unowned_channel(spaceship), heading);
  return result.status();
}
```

### ResultOf and UnownedResultOf

For a method named `Foo`, `ResultOf::Foo` is the return type of the *managed
flavor*. `UnownedResultOf::Foo` is the return type of the *caller-allocating
flavor*. Both types define the same set of methods:

*   `zx_status status() const` returns the transport status. it returns the
    first error encountered during (if applicable) linearizing, encoding, making
    a call on the underlying channel, and decoding the result.
    If the status is `ZX_OK`, the call has succeeded, and vice versa.
*   `const char* error() const` contains a brief error message when status is
    not `ZX_OK`. Otherwise, returns `nullptr`.
*   **(only for two-way calls)** `FooResponse* Unwrap()` returns a pointer
    to the FIDL response message. For `ResultOf::Foo`, the pointer points to
    memory owned by the result object. For `UnownedResultOf::Foo`, the pointer
    points to a caller-provided buffer. `Unwrap()` should only be called when
    the status is `ZX_OK`.

#### Allocation Strategy And Move Semantics

`ResultOf::Foo` stores the response buffer inline if the message is guaranteed
to fit under 512 bytes. Since the result object is usually instantiated on the
caller's stack, this effectively means the response is stack-allocated when it
is reasonably small. If the maximal response size exceeds 512 bytes,
`ResultOf::Foo` instead contains a `std::unique_ptr` to a heap-allocated buffer.

Therefore, a `std::move()` on `ResultOf::Foo` may be costly if the response
buffer is inline: the content has to be copied, and pointers to out-of-line
objects have to be updated to locations within the destination object.
Consider the following snippet:

```cpp
int CountPlanets(ResultOf::ScanForPlanets result) { /* ... */ }

auto result = client.ScanForPlanets();
SpaceShip::ScanForPlanetsResponse* response = result.Unwrap();
Planet* planet = &response->planets[0];
int count = CountPlanets(std::move(result));    // Costly
// In addition, |response| and |planet| are invalidated due to the move
```

It may be written more efficiently as:

```cpp
int CountPlanets(fidl::VectorView<SpaceShip::Planet> planets) { /* ... */ }

auto result = client.ScanForPlanets();
int count = CountPlanets(result.Unwrap()->planets);
```

> If the result object need to be passed around multiple function calls,
> consider pre-allocating a buffer in the outer-most function and use the
> caller-allocating flavor.

### In-Place Calls

Both the *managed flavor* and the *caller-allocating flavor* will copy the
arguments into the request buffer. When there is out-of-line data involved,
*message linearization* is additionally required to collate them as per the
wire-format. When the request is large, these copying overhead can add up.
LLCPP supports making a call directly on a caller-provided buffer containing
a request message in decoded form, without any parameter copying. The request
is encoded in-place, hence the name of the scoping class `InPlace`.

```cpp
class InPlace final {
 public:
  static ::fidl::internal::StatusAndError
  SetHeading(zx::unowned_channel client_end,
             fidl::DecodedMessage<SetHeadingRequest> params);

  static ::fidl::DecodeResult<ScanForPlanets>
  ScanForPlanets(zx::unowned_channel client_end,
                 fidl::DecodedMessage<ScanForPlanetsRequest> params,
                 fidl::BytePart response_buffer);
};
```

These functions always take a
[`fidl::DecodedMessage<FooRequest>`](#fidl_decodedmessage_t) which wraps the
user-provided buffer. To use it properly, initialize the request buffer with a
FIDL message in decoded form. *In particular, out-of-line objects have to be
packed according to the wire-format, and therefore any pointers in the message
have to point within the same buffer.*

When there is a response defined, the generated functions additionally ask for a
`response_buffer` as the last argument. The response buffer does not have to be
initialized.

```cpp
// Allocate buffer for in-place call
fidl::Buffer<SetHeadingRequest> request_buffer;
fidl::BytePart request_bytes = request_buffer.view();
memset(request_bytes.data(), 0, request_bytes.capacity());

// Manually construct the message
auto msg = reinterpret_cast<SetHeadingRequest*>(request_bytes.data());
msg->heading = 42;
// Here since our message is a simple struct,
// the request size is equal to the capacity.
request_bytes.set_actual(request_bytes.capacity());

// Wrap with a fidl::DecodedMessage
fidl::DecodedMessage<SetHeadingRequest> request(std::move(request_bytes));

// Finally, make the call.
auto result = SpaceShape::InPlace::SetHeading(channel, std::move(request));
// Check result.status(), result.error()
```

Despite the verbosity, there is actually very little work involved.
The buffer passed to the underlying `zx_channel_call` system call is in fact
`request_bytes`. The performance benefits become apparent when say the request
message contains a large inline array. One could set up the buffers once, then
make repeated calls while mutating the array by directly editing the buffer
in between.

Key Point: in-place calls only reduce overhead in the request part of the call.
Responses are already processed in-place even in the managed and
caller-allocating flavors.

## Server API

```cpp
class Interface {
 public:
  virtual void SetHeading(int16_t heading,
                          SetHeadingCompleter::Sync completer) = 0;

  class ScanForPlanetsCompleterBase {
   public:
    void Reply(fidl::VectorView<Planet> planets);
    void Reply(fidl::BytePart buffer, fidl::VectorView<Planet> planets);
    void Reply(fidl::DecodedMessage<ScanForPlanetsResponse> params);
  };

  using ScanForPlanetsCompleter = fidl::Completer<ScanForPlanetsCompleterBase>;

  virtual void ScanForPlanets(ScanForPlanetsCompleter::Sync completer) = 0;
};

bool TryDispatch(Interface* impl, fidl_msg_t* msg, fidl::Transaction* txn);
```

The generated `Interface` class has pure virtual functions corresponding to the
method calls defined in the FIDL protocol. One may override these functions in
a subclass, and dispatch FIDL messages to a server instance by calling
`TryDispatch`.
The bindings runtime would invoke these handler functions appropriately.

```cpp
class MyServer final : fuchsia::fleet::SpaceShip::Interface {
 public:
  void SetHeading(int16_t heading,
                  SetHeadingCompleter::Sync completer) override {
    // Update the heading...
  }
  void ScanForPlanets(ScanForPlanetsCompleter::Sync completer) override {
    fidl::VectorView<Planet> discovered_planets = /* perform planet scan */;
    // Send the |discovered_planets| vector as the response.
    completer.Reply(discovered_planets);
  }
};
```

Each handler function has an additional last argument `completer`.
It captures the various ways one may complete a FIDL transaction, by sending a
reply, closing the channel with epitaph, etc.
For FIDL methods with a reply e.g. `ScanForPlanets`, the corresponding completer
defines up to three overloads of a `Reply()` function
(managed, caller-allocating, in-place), similar to the client side API.
The completer always defines a `Close(zx_status_t)` function, to close the
connection with a specified epitaph.

### Responding Asynchronously

Notice that the type for the completer `ScanForPlanetsCompleter::Sync` has
`::Sync`. This indicates the default mode of operation: the server must
synchronously make a reply before returning from the handler function.
Enforcing this allows optimizations: the bookkeeping metadata for making
a reply may be stack-allocated.
To asynchronously make a reply, one may call the `ToAsync()` method on a `Sync`
completer, converting it to `ScanForPlanetsCompleter::Async`. The `Async`
completer supports the same `Reply()` functions, and may out-live the scope of
the handler function by e.g. moving it into a lambda capture.

```cpp
void ScanForPlanets(ScanForPlanetsCompleter::Sync completer) override {
  // Suppose scanning for planets takes a long time,
  // and returns the result via a callback...
  EnqueuePlanetScan(some_parameters)
      .OnDone([completer = completer.ToAsync()] (auto planets) mutable {
        // Here the type of |completer| is |ScanForPlanetsCompleter::Async|.
        completer.Reply(planets);
      });
}
```

# Reference

## Design

### Goals

*   Support encoding and decoding FIDL messages with C++17.
*   Provide fine-grained control over memory allocation.
*   More type-safety and more features than the C language bindings.
*   Match the size and efficiency of the C language bindings.
*   Depend only on a small subset of the standard library.
*   Minimize code bloat through table-driven encoding and decoding.
*   Reuse encoders, decoders, and coding tables generated for C language
    bindings.

## Code Generator

### Mapping FIDL Types to Low-Level C++ Types

This is the mapping from FIDL types to Low-Level C++ types which the code
generator produces.

FIDL                                        | Low-Level C++
--------------------------------------------|------------------------------------------------------
`bool`                                      | `bool`, *(requires sizeof(bool) == 1)*
`int8`                                      | `int8_t`
`uint8`                                     | `uint8_t`
`int16`                                     | `int16_t`
`uint16`                                    | `uint16_t`
`int32`                                     | `int32_t`
`uint32`                                    | `uint32_t`
`int64`                                     | `int64_t`
`uint64`                                    | `uint64_t`
`float32`                                   | `float`
`float64`                                   | `double`
`handle`, `handle?`                         | `zx::handle`
`handle<T>`,`handle<T>?`                    | `zx::T` *(subclass of zx::object<T>)*
`string`                                    | `fidl::StringView`
`string?`                                   | `fidl::StringView`
`vector<T>`                                 | `fidl::VectorView<T>`
`vector<T>?`                                | `fidl::VectorView<T>`
`array<T>:N`                                | `fidl::Array<T, N>`
*protocol, protocol?*                       | `zx::channel`
*request\<Protocol\>, request\<Protocol\>?* | `zx::channel`
*struct* Struct                             | *struct* Struct
*struct?* Struct                            | *struct* Struct*
*table* Table                               | (not yet supported)
*union* Union                               | *struct* Union
*union?* Union                              | *struct* Union*
*xunion* Xunion                             | *struct* Xunion
*xunion?* Xunion                            | *struct* Xunion*
*enum* Foo                                  | *enum class Foo : data type*

#### fidl::StringView

Defined in [lib/fidl/cpp/string_view.h](/zircon/system/ulib/fidl/include/lib/fidl/cpp/string_view.h)

Holds a reference to a variable-length string stored within the buffer. C++
wrapper of **fidl_string**. Does not own the memory of the contents.

It is memory layout compatible with **fidl_string**.
No constructor or destructor so this is POD.

#### fidl::VectorView\<T\>

Defined in [lib/fidl/cpp/vector_view.h](/zircon/system/ulib/fidl/include/lib/fidl/cpp/vector_view.h)

Holds a reference to a variable-length vector of elements stored within the
buffer. C++ wrapper of **fidl_vector**. Does not own the memory of elements.

It is memory layout compatible with **fidl_vector**.
No constructor or destructor so this is POD.

#### fidl::Array\<T, N\>

Defined in [lib/fidl/llcpp/array.h](/zircon/system/ulib/fidl/include/lib/fidl/llcpp/array.h)

Owns a fixed-length array of elements.
Similar to `std::array<T, N>` but intended purely for in-place use.

It is memory layout compatible with FIDL arrays, and is standard-layout.
The destructor closes handles if applicable e.g. it is an array of handles.

## Bindings Library

### Dependencies

The low-level C++ bindings depend only on a small subset of header-only parts
of the standard library. As such, they may be used in environments where linking
against the C++ standard library is discouraged or impossible.

### Helper Types

#### fidl::DecodedMessage\<T\>

Defined in [lib/fidl/llcpp/decoded_message.h](/zircon/system/ulib/fidl/include/lib/fidl/llcpp/decoded_message.h)

Manages a FIDL message in [decoded form](../reference/wire-format#Dual-Forms_Encoded-vs-Decoded).
The message type is specified in the template parameter `T`.
This class takes care of releasing all handles which were not consumed
(std::moved from the decoded message) when it goes out of scope.

`fidl::Encode(std::move(decoded_message))` encodes in-place.

#### fidl::EncodedMessage\<T\>

Defined in [lib/fidl/llcpp/encoded_message.h](/zircon/system/ulib/fidl/include/lib/fidl/llcpp/encoded_message.h)
Holds a FIDL message in [encoded form](../reference/wire-format#Dual-Forms_Encoded-vs-Decoded),
that is, a byte array plus a handle table.
The bytes part points to an external caller-managed buffer, while the handles part
is owned by this class. Any handles will be closed upon destruction.

`fidl::Decode(std::move(encoded_message))` decodes in-place.

##### Example

```cpp
zx_status_t SayHello(const zx::channel& channel, fidl::StringView text,
                     zx::handle token) {
  assert(text.size() <= MAX_TEXT_SIZE);

  // Manually allocate the buffer used for this FIDL message,
  // here we assume the message size will not exceed 512 bytes.
  uint8_t buffer[512] = {};
  fidl::DecodedMessage<example::Animal::SayRequest> decoded(
      fidl::BytePart(buffer, 512));

  // Fill in header and contents
  auto& header = decoded.message()->_hdr;
  header.transaction_id = 1;
  header.ordinal = example_Animal_Say_ordinal;

  decoded.message()->text = text;
  // Handle types have to be moved
  decoded.message()->token = std::move(token);

  // Encode the message in-place
  fidl::EncodeResult<example::Animal::SayRequest> encode_result =
      fidl::Encode(std::move(decoded));
  if (encode_result.status != ZX_OK) {
    return encode_result.status;
  }

  fidl::EncodedMessage<example::Animal::SayRequest>& encoded =
      encode_result.message;
  return channel.write(0, encoded.bytes().data(), encoded.bytes().size(),
                       encoded.handles().data(), encoded.handles().size());
}
```
