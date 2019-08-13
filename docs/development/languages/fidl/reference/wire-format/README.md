# Wire Format Specification

This document is a specification of the Fuchsia Interface Definition Language
(**FIDL**) message format.

See [Overview](../../README.md) for more information about FIDL's overall
purpose, goals, and requirements, as well as links to related documents.

[TOC]

## Concepts

This section provides requisite background information for the concepts
used throughout the description.

### Message

A FIDL **message** is a collection of data.

The message is a contiguous structure consisting of a single
**in-line primary object** followed by zero or more
**out-of-line secondary objects**.

Objects are stored in **traversal order**, and are subject to **padding**.

![drawing](objorder.png)

### Primary and Secondary Objects

The first object is called the **primary object**.
It is a structure of fixed size whose type and size are known from the
context.

The primary object may refer to **secondary objects** (such as in the
case of strings, vectors, unions, and so on) if additional variable-sized
or optional data is required.

Secondary objects are stored **out-of-line** in traversal order.

Both primary and secondary objects are 8-byte aligned, and are stored
without gaps (other than those required for alignment).

Together, a primary object and its secondary objects are called a
**message**.

#### Messages for transactions

A transactional FIDL message (**transactional message**) is used to
send data from one application to another.

> The roles of the applications (e.g. **client** vs **server**) are not
> relevant to the formatting of the data.

As we will see in the [transactional messages](#transactional-messages) section
below, a transactional message is composed of a header message optionally followed
by a body message.

### Traversal Order

The **traversal order** of a message is determined by a recursive depth-first
walk of all of the **objects** it contains, as obtained by following the chain
of references.

Given the following structure:

```fidl
struct Cart {
    vector<Item> items;
};
struct Item {
    Product product;
    uint32 quantity;
};
struct Product {
    string sku;
    string name;
    string? description;
    uint32 price;
};
```

The depth-first traversal order for a `Cart` message is defined by the following
pseudo-code:

```
visit Cart:
    for each Item in Cart.items vector data:
        visit Item.product:
            visit Product.sku
            visit Product.name
            visit Product.description
            visit Product.price
        visit Item.quantity
```

### Dual Forms: Encoded vs Decoded

The same message content can be expressed in one of two forms:
**encoded** and **decoded**.
These have the same size and overall layout, but differ in terms of their
representation of pointers (memory addresses) or handles (capabilities).

FIDL is designed such that **encoding** and **decoding** of messages can occur
in place in memory.

Message encoding is canonical &mdash; there is exactly one encoding for
a given message.

![drawing](dual-forms.png)

#### Encoded Messages

An **encoded message** has been prepared for transfer to another process: it
does not contain pointers (memory addresses) or handles (capabilities).

During **encoding**...

*   all pointers to sub-objects within the message are replaced with flags which
    indicate whether their referent is present or not-present,
*   all handles within the message are extracted to an associated **handle
    vector** and replaced with flags which indicate whether their referent is
    present or not-present.

The resulting **encoded message** and **handle vector** can then be sent to
another process using [**zx_channel_write()**][channel write] or a similar IPC
mechanism.
There are additional constraints on this kind of IPC; see [transactional
messages](#transactional-messages).

> Note that the handle vector is *not* stored as part of the message, it's
> sent separately (also known as "**out-of-band**, not to be confused with
> **out-of-line**).
> For example, the [**zx_channel_write()**][channel write] function
> takes two sets of data pointers; one for the message, and one for the
> handle vector.
> The message data pointer will contain all of the **in-line** and
> **out-of-line** data, and the handle vector pointer will contain the
> handles.

#### Decoded Messages

A **decoded message** has been prepared for use within a process's address
space: it may contain pointers (memory addresses) or handles (capabilities).

During **decoding**...

*   all pointers to sub-objects within the message are reconstructed using the
    encoded present / not-present flags,
*   all handles within the message are restored from the associated **handle
    vector** using the encoded present / not-present flags.

The resulting **decoded message** is ready to be consumed directly from memory.

### Inlined Objects

Objects may also contain **inlined objects** which are aggregated within the
body of the containing object, such as embedded structs and fixed-size arrays of
structs.

### Example

In the following example, the `Region` structure contains a vector of
`Rect` structures, with each `Rect` consisting of two `Point`s.
Each `Point` consists of an `x` and `y` value.

```fidl
struct Region {
    vector<Rect> rects;
};
struct Rect {
    Point top_left;
    Point bottom_right;
};
struct Point { uint32 x, y; };
```

Examining the objects in traversal order means that we start with the
`Region` structure &mdash; it's the **primary object**.

The `rects` member is a `vector`, so its contents are stored **out-of-line**.
This means that the `vector` content immediately follows the `Region` object.

Each `Rect` struct contains two `Point`s, which are stored **in-line**
(because there are a fixed number of them), and each of the `Point`s'
primitive data types (`x` and `y`) are also stored **in-line**.
The reason is the same; there is a fixed number of the member types.

![drawing](objects.png)

We use **in-line** storage when the size of the subordinate
object is fixed, and **out-of-line** when it's variable (including
optional).

# Type Details

In this section, we illustrate the encodings for all FIDL objects.

## Primitives

*   Value stored in [little-endian format][ftp-030].
*   Packed with natural alignment.
    *   Each _m_-byte primitive is stored on an _m_-byte boundary.
*   Not nullable.

The following primitive types are supported:

Category                | Types
----------------------- | ----------------------------
Boolean                 | `bool`
Signed integer          | `int8`, `int16`, `int32`, `int64`
Unsigned integer        | `uint8`, `uint16`, `uint32`, `uint64`
IEEE 754 floating-point | `float32`, `float64`
strings                 | (not a primitive, see [Strings](#strings) below)

Number types are suffixed with their size in bits.

The Boolean type, `bool`, is stored as a single byte, and has only the
value **0** or **1**.

All floating point values represent valid IEEE 754 bit patterns.

![drawing](primitive-int.png)

![drawing](primitive-fp.png)

## Enums and Bits

Bit fields and enumerations are stored as their underlying primitive
type (e.g., `uint32`).

## Handles

A handle is a 32-bit integer, but with special treatment.
When encoded for transfer, the handle's on-wire representation is replaced with
a present / not-present indication, and the handle itself is stored in a separate
handle vector.
When decoded, the handle presence indication is replaced with zero (if not
present) or a valid handle (if present).

The handle *value* itself is **not** transferred from one application to another.
In this respect, handles are like pointers; they reference a context that's
unique to each application.
Handles are moved from one application's context to the other's.

![drawing](handlexlate.png)

The value zero can be used to indicate a nullable handle is null[[1]](#Footnote-1).

## Aggregate objects

Aggregate objects serve as containers of other objects.
They may store that data in-line or out-of-line, depending on their type.

### Arrays

*   Fixed length sequence of homogeneous elements.
*   Packed with natural alignment of their elements.
    *   Alignment of array is the same as the alignment of its elements.
    *   Each subsequent element is aligned on element's alignment boundary.
*   The stride of the array is exactly equal to the size of the element (which
    includes the padding required to satisfy element alignment constraints).
*   Not nullable.
*   There is no special case for arrays of bools. Each bool element takes one
    byte as usual.

Arrays are denoted:

*   `array<T>:N`: where **T** can be any FIDL type
    (including an array) and **N** is the number of elements in the array.

![drawing](arrays.png)

### Vectors

*   Variable-length sequence of homogeneous elements.
*   Nullable; null vectors and empty vectors are distinct.
*   Can specify a maximum size, e.g. `vector<T>:40`
    for a maximum 40 element vector.
*   Stored as a 16 byte record consisting of:
    *   `size`: 64-bit unsigned number of elements
    *   `data`: 64-bit presence indication or pointer to out-of-line element data
*   When encoded for transfer, `data` indicates
    presence of content:
    *   `0`: vector is null
    *   `UINTPTR_MAX`: vector is non-null, data is the next out-of-line object
*   When decoded for consumption, `data` is a
    pointer to content:
    *   `0`: vector is null
    *   `<valid pointer>`: vector is non-null, data is at indicated memory address
*   There is no special case for vectors of bools. Each bool element takes one
    byte as usual.

Vectors are denoted as follows:

*   `vector<T>`: non-nullable vector of element type **T** (validation error
    occurs if null `data` is encountered)
*   `vector<T>?`: nullable vector of element type **T**
*   `vector<T>:N`, `vector<T>:N?`: vector with maximum length of **N** elements

**T** can be any FIDL type.

![drawing](vectors.png)

### Strings

Strings are implemented as a vector of `uint8` bytes, with the constraint
that the bytes MUST be valid UTF-8.

### Structures

A structure contains a sequence of typed fields.

Internally, the structure is padded so that all members are aligned to the largest
alignment requirement of all members.
Externally, the structure is aligned on an 8-byte boundary, and may therefore contain
final padding to meet that requirement.

Here are some examples.

A struct with an **int32** and an **int8** field has an alignment of 4 bytes (due to
the **int32**), and a size of 8 bytes (3 bytes of padding after the **int8**):

![drawing](struct1.png)

A struct with a **bool** and a **string** field has an alignment of 8 bytes (due to
the **string**) and a size of 24 bytes (7 bytes of padding after the **bool**):
![drawing](struct2.png)

> Keep in mind that a **string** is really just a special
> case of `vector<uint8>`.

A struct with a **bool** and two **uint8** fields has an alignment of 1 byte and a
size of 3 bytes (no padding!):

![drawing](struct3.png)

Note that a structure can be:
* empty &mdash; it has no fields. Such a structure is 1 byte in size, with an
  alignment of 1 byte, and is exactly equivalent to a structure containing a
  `uint8` with the value zero.
* non-nullable &mdash; the structure's contents are stored in-line.
* nullable &mdash; the structure's contents are stored out-of-line and
  accessed through an indirect reference.

Storage of a structure depends on whether it is nullable at point of reference.

* Non-nullable structures:
  * Contents are stored in-line within their containing type, enabling very
    efficient aggregate structures to be constructed.
  * The structure layout does not change when inlined; its fields are not
    repacked to fill gaps in its container.
* Nullable structures:
  * Contents are stored out-of-line and accessed through an indirect
    reference.
  * When encoded for transfer, stored reference indicates presence of
    structure:
    * `0`: reference is null
    * `UINTPTR_MAX`: reference is non-null, structure content
      is the next out-of-line object
  * When decoded for consumption, stored reference is a pointer:
    * `0`: reference is null
    * `<valid pointer>`: reference is non-null, structure content is at
      indicated memory address

Structs are denoted by their declared name (e.g. `Circle`) and nullability:

*   `Point`: non-nullable `Point`
*   `Color?`: nullable `Color`

The following example illustrates:
  * structure layout (order, packing, and alignment),
  * a non-nullable structure (`Point`),
  * a nullable structure (`Color`)

```fidl
struct Circle {
    bool filled;
    Point center;    // Point will be stored in-line
    float32 radius;
    Color? color;    // Color will be stored out-of-line
    bool dashed;
};
struct Point { float32 x, y; };
struct Color { float32 r, g, b; };
```

The `Color` content is padded to the 8 byte secondary object alignment boundary.
Going through the layout in detail:

![drawing](structs.png)

1. The first member, `bool filled`, occupies one byte, but requires three bytes
   of padding because of the next member, which has a 4-byte alignment requirement.
2. The `Point center` member is an example of a non-nullable struct. As such,
   its content (the `x` and `y` 32-bit floats) are inlined, and the entire thing
   consumes 8 bytes.
3. `radius` is a 32-bit item, requiring 4 byte alignment. Since the next available
   location is already on a 4 byte alignment boundary, no padding is required.
4. The `Color? color` member is an example of a nullable structure. Since the
   `color` data may or may not be present, the most efficient way of handling this
   is to keep a pointer to the structure as the in-line data. That way, if the
   `color` member is indeed present, the pointer points to its data (or, in the
   case of the encoded format, indicates "is present"), and the data itself is
   stored out-of-line (after the data for the `Circle` structure). If the
   `color` member is not present, the pointer is `NULL` (or, in the encoded
   format, indicates "is not present" by storing a zero).
5. The `bool dashed` doesn't require any special alignment, so it goes next.
   Now, however, we've reached the end of the object, and *all objects must be
   8-byte aligned*. That means we need an additional 7 bytes of padding.
6. The out-of-line data for `color` follows the `Circle` data structure, and
   contains three 32-bit `float` values (`r`, `g`, and `b`); they require 4
   byte alignment and so can follow each other without padding. But, just as
   in the case of the `Circle` object, we require the object itself to be
   8-byte aligned, so 4 bytes of padding are required.

Overall, this structure takes 48 bytes.

By moving the `bool dashed` to be immediately after the `bool filled`, though,
you can realize significant space savings [[2]](#Footnote-2):

![drawing](struct-reorg.png)

1. The two `bool` values are "packed" together within what would have been
   wasted space.
2. The padding is reduced to two bytes &mdash; this would be a good place to
   add a 16-bit value, or some more `bool`s or 8-bit integers.
3. Notice how there's no padding required after the `color` pointer; everything
   is perfectly aligned on an 8 byte boundary.

The structure now takes 40 bytes.

> While `fidlc` could automatically pack structs, like Rust, we chose not to do
> that in order to simplify [ABI compatibility changes](../abi-compat.md).

### Unions

*   Tagged option type consisting of tag field and variadic contents.
*   Tag field is represented with a **uint32 enum**.
*   Size of union is the size of the tag field plus the size of the largest
    union variant including padding necessary to satisfy its alignment requirements.
*   Alignment factor of union is defined by the maximal alignment factor of the
    tag field and any of its options.
*   Union is padded so that its size is a multiple of its alignment factor.
    For example:
    1. a union with an **int32** and an **int8** option has an alignment of 4 bytes (due to
       the **int32**), and a size of 8 bytes including the 4 byte tag (0 or 3 bytes of padding,
       depending on the option / variant).
    2. a union with a **bool** and a **string** option has an alignment of 8 bytes (due to
       the **string**), and a size of 24 bytes (4 or 19 bytes of padding, depending on the
       option / variant).
*   In general, changing the definition of a union will break binary
    compatibility; instead prefer to extend interfaces by adding new methods
    which use new unions.

Storage of a union depends on whether it is nullable at point of reference.

*   Non-nullable unions:
    *   Contents are stored in-line within their containing type, enabling very
        efficient aggregate structures to be constructed.
    *   The union layout does not change when inlined; its options are not
        repacked to fill gaps in its container.
*   Nullable unions:
    *   Contents are stored out-of-line and accessed through an indirect
        reference.
    *   When encoded for transfer, stored reference indicates presence of union:
        *   `0`: reference is null
        *   `UINTPTR_MAX`: reference is non-null, union content is the next out-of-line object
    *   When decoded for consumption, stored reference is a pointer:
        *   `0`: reference is null
        *   `<valid pointer>`: reference is non-null, union content is at indicated memory address

Unions are denoted by their declared name (e.g. `Pattern`) and nullability:

*   `Pattern`: non-nullable `Pattern`
*   `Pattern?`: nullable `Pattern`

The following example shows how unions are laid out according to their options.

```fidl
struct Paint {
    Pattern fg;
    Pattern? bg;
};
union Pattern {
    Color color;
    Texture texture;
};
struct Color { float32 r, g, b; };
struct Texture { string name; };
```

When laying out `Pattern`, space is first allotted to the tag (4 bytes), then
to the selected option.

![drawing](unions.png)

### Envelopes

An envelope is a container for out-of-line data, used internally by tables
and extensible unions.
It is not exposed to the FIDL language.

It has a fixed, 16 byte format, and is not nullable:

![drawing](envelope.png)

An envelope can, however, point to empty content.
In that case, `num_bytes`, `num_handles`, and the pointer will all be zero.

Furthermore, because `num_bytes` represents the size of an object, it's
always a multiple of 8, regardless of the actual amount of data that it points to.

Having `num_bytes` and `num_handles` allows us to skip unknown envelope content.

### Tables

*   Record type consisting of the number of elements and a pointer.
*   Pointer points to an array of envelopes, each of which contains one element.
*   Each element is associated with an ordinal.
*   Ordinals are sequential, gaps incur an empty envelope cost and hence are discouraged.

Tables are denoted by their declared name (e.g., **Value**), and are not nullable:

*   `Value`: non-nullable `Value`

The following example shows how tables are laid out according to their fields.

```fidl
table Value {
    1: int16 command;
    2: Circle data;
    3: float64 offset;
};
```

![drawing](tables.png)

### Extensible Unions (xunions)

*   Record type consisting of an ordinal and an envelope.
*   Ordinal indicates member selection, and is represented with a **uint32**.
*   Ordinals are calculated by hashing the concatenated library name, xunion
    name, and member name, and retaining 31 bits.
    See [ordinal hashing] for further details.
*   Nullable xunions are represented with a `0` ordinal, and an empty envelope.
*   Empty xunions are not allowed.

xunions are denoted by their declared name (e.g. `Value`) and nullability:

*   `Value`: non-nullable `Value`
*   `Value?`: nullable `Value`

The following example shows how xunions are laid out according to their fields.

```fidl
xunion Value {
    int16 command;
    Circle data;
    float64 offset;
};
```

![drawing](xunion.png)

### Transactional Messages

In a transactional message, there is always a **header**, followed by
an optional **body**.

Both the header and body are FIDL messages, as defined above; that is,
a collection of data.

The header has the following form:

![drawing](transaction-header.png)

*   `zx_txid_t txid`, transaction ID (32 bits)
    * `txid`s with the high bit set are reserved for use by
      [**zx_channel_write()**][channel write]
    * `txid`s with the high bit unset are reserved for use by userspace
    * a value of `0` for `txid` is reserved for messages which do not
      require a response from the other side.
    * See [**zx_channel_call()**][channel call] for more details on
      `txid` allocation
*   `uint32 reserved0`, reserved for future use, must be zero
*   `uint32 flags`, all unused bits must be set to zero
*   `uint32 ordinal`
    *   The zero ordinal is invalid.
    *   Ordinals with the most significant bit set are reserved for
        control messages and future expansion.
    *   Ordinals without the most significant bit set indicate method calls
        and responses.

There are three kinds of transactional messages:

* method requests,
* method responses, and
* event requests.

We'll use the following interface for the next few examples:

```fidl
protocol Calculator {
    Add(int32 a, int32 b) -> (int32 sum);
    Divide(int32 dividend, int32 divisor) -> (int32 quotient, int32 remainder);
    Clear();
    -> OnError(uint32 status_code);
};
```

The **Add()** and **Divide()** methods illustrate both the method request
(sent from the client to the server), and a method reponse
(sent from the server back to the client).

The **Clear()** method is an example of a method request that does not
have a body.

> It's not correct to say it has an "empty" body: that would imply that
> there's a **body** following the **header**. In the case of **Clear()**,
> there is no **body**, there is only a **header**.

#### Method Request Messages

The client of an interface sends method request messages to the server
in order to invoke the method.

#### Method Response Messages

The server sends method reponse messages to the client to indicate completion
of a method invocation and to provide a (possibly empty) result.

Only two-way method requests which are defined to provide a (possibly empty) result
in the protocol declaration will elicit a method response.
One-way method requests must not produce a method response.

A method response message provides the result associated with a prior method request.
The body of the message contains the method results as if they were packed in a
**struct**.

Here we see that the answer to 912 / 43 is 21 with a remainder of 9.
Note the `txid` value of `1` &mdash; this identifies the transaction.
The `ordinal` value of `2` indicates the method &mdash; in this case, the
**Divide()** method.

![drawing](transaction-division.png)

Below, we see that `123 + 456` is `579`.
Here, the `txid` value is now `2` &mdash; this is simply the next transaction
number assigned to the transaction.
The `ordinal` is `1`, indicating **Add()**, and note that the result requires
4 bytes of padding in order to make the **body** object have a size that's
a multiple of 8 bytes.

![drawing](transaction-addition.png)

And finally, the **Clear()** method is different than the **Add()** and
**Divide()** in two important ways:
* it does not have a **body** (that is, it consists solely of the **header**), and
* it does not solicit a response from the interface (`txid` is zero).

![drawing](method-clear.png)

#### Event Requests

An example of an event is the **OnError()** event in our `Calculator`.

The server sends an unsolicited event request to the client
to indicate that an asynchronous event occurred, as specified by
the protocol declaration.

In the `Calculator` example, we can imagine that an attempt to divide by zero
would cause the **OnError()** event to be sent with a "divide by zero" status code
prior to the connection being closed. This allows the client to distinguish
between the connection being closed due to an error, as opposed to for other
reasons (such as the calculator process terminating abnormally).

![drawing](events.png)

Notice how the `txid` is zero (indicating this is not part of a transaction),
and `ordinal` is `4` (indicating the **OnError()** method).

The **body** contains the event arguments as if they were packed in a
**struct**, just as with method result messages.
Note that the body is padded to maintain 8-byte alignment.

#### Epitaph (Control Message Ordinal 0xFFFFFFFF)

An epitaph is a message with ordinal **0xFFFFFFFF**. A server may send an
epitaph as the last message prior to closing the connection, to provide an
indication of why the connection is being closed. No further messages may be
sent through the channel after the epitaph. Epitaphs are not sent from clients
to servers.

The epitaph contains an error status. The error status of the epitaph is stored
in the reserved `uint32` of the message header. The reserved word is treated as
being of type **zx_status_t**: negative numbers are reserved for system error
codes, positive numbers are reserved for application error codes, and `ZX_OK` is
used to indicate normal connection closure. The message is otherwise empty.

## Details

#### Size and Alignment

`sizeof(T)` denotes the size in bytes for an object of type **T**.

`alignof(T)` denotes the alignment factor in bytes to store an object of type **T**.

FIDL primitive types are stored at offsets in the message which are a multiple
of their size in bytes. Thus for primitives **T**, `alignof(T) ==
sizeof(T)`. This is called *natural alignment*. It has the
nice property of satisfying typical alignment requirements of modern CPU
architectures.

FIDL complex types, such as structs and arrays, are stored at offsets in the
message which are a multiple of the maximum alignment factor of all of their
fields. Thus for complex types **T**, `alignof(T) ==
max(alignof(F:T))` over all fields **F** in **T**. It has the nice
property of satisfying typical C structure packing requirements (which can be
enforced using packing attributes in the generated code). The size of a complex
type is the total number of bytes needed to store its members properly aligned
plus padding up to the type's alignment factor.

FIDL primary and secondary objects are aligned at 8-byte offsets within the
message, regardless of their contents. The primary object of a FIDL message
starts at offset 0. Secondary objects, which are the only possible referent of
pointers within the message, always start at offsets which are a multiple of 8.
(So all pointers within the message point at offsets which are a multiple of 8.)

FIDL in-line objects (complex types embedded within primary or secondary
objects) are aligned according to their type. They are not forced to 8 byte
alignment.

##### Types

Notes:

1. **N** indicates the number of elements, whether stated explicity (as in
   `array<T>:N`, an array with **N** elements of type **T**) or implictly (a `table`
   consisting of 7 elements would have `N=7`).
2. The out-of-line size is always padded to 8 bytes; we indicate the content size
   below, excluding the padding.
3. `sizeof(T)` in the `vector` entry below is `in_line_sizeof(T) + out_of_line_sizeof(T)`.
4. **M** in the `table` entry below is the maximum ordinal of present field.
5. In the `struct` entry below, the padding refers to the required padding to make the
   `struct` aligned to the widest element. For example, `struct{uint32;uint8}`
   has 3 bytes of padding, which is different than the padding to align to 8 bytes
   boundaries.

Type(s)                      | Size (in-line)                    | Size (out-of-line)                                              | Alignment
-----------------------------|-----------------------------------|-----------------------------------------------------------------|--------------------------------
`bool`                       | 1                                 | 0                                                               | 1
`int8`, `uint8`              | 1                                 | 0                                                               | 1
`int16`, `uint16`            | 2                                 | 0                                                               | 2
`int32`, `uint32`, `float32` | 4                                 | 0                                                               | 4
`int64`, `uint64`, `float64` | 8                                 | 0                                                               | 8
`enum`, `bits`               | (underlying type)                 | 0                                                               | (underlying type)
`handle`, et al.             | 4                                 | 0                                                               | 4
`array<T>:N`                 | sizeof(T) * N                     | 0                                                               | alignof(T)
`vector`, et al.             | 16                                | N * sizeof(T)                                                   | 8
`struct`                     | sum(sizeof(fields)) + padding     | 0                                                               | 8
`struct?`                    | 8                                 | sum(sizeof(fields)) + padding                                   | 8
`union`                      | 4 + max(sizeof(fields)) + padding |  0                                                              | max(all fields)
`union?`                     | 8                                 | 4 + max(sizeof(fields)) + padding                               | 8
`envelope`                   | 16                                | sizeof(field)                                                   | 8
`table`                      | 16                                | M * sizeof(envelope) + sum(aligned_to_8(sizeof(present fields)) | 8
`xunion`, `xunion?`          | 24                                | sizeof(selected variant)                                        | 8

The `handle` entry above refers to all flavors of handles, specifically
`handle`, `handle?`, `handle<H>`, `handle<H>?`, `Protocol`, `Protocol?`,
`request<Protocol>`, and `request<Protocol>?`.

Similarly, the `vector` entry above refers to all flavors of vectors,
specifically `vector<T>`, `vector<T>?`, `vector<T>:N`, `vector<T>:N?`,
`string`, `string?`, `string:N`, and `string:N?`.

#### Padding

The creator of a message must fill all alignment padding gaps with zeros.

The consumer of a message must verify that padding contains zeros (and generate
an error if not).

#### Maximum Recursion Depth

FIDL arrays, vectors, structures, tables, unions, and xunions enable the
construction of recursive messages.
Left unchecked, processing excessively deep messages could
lead to resource exhaustion of the consumer.

For safety, the maximum recursion depth for all FIDL messages is limited to
**32** levels of nested complex objects. The FIDL validator **must** enforce
this by keeping track of the current nesting level during message validation.

Complex objects are arrays, vectors, structures, tables, unions, or xunions
which contain pointers or handles which require fix-up.
These are precisely the kinds of
objects for which **encoding tables** must be generated. See [C
Language Bindings](/docs/development/languages/fidl/tutorial/tutorial-c.md)
for information about encoding
tables. Therefore, limiting the nesting depth of complex objects has the effect
of limiting the recursion depth for traversal of encoding tables.

Formal definition:

*   The message body is defined to be at nesting level **0**.
*   Each time the validator encounters a complex object, it increments the
    nesting level, recursively validates the object, then decrements the nesting
    level.
*   If at any time the nesting level exceeds **31**, a validation error is
    raised and validation terminates.

#### Validation

The purpose of message validation is to discover wire format errors early before
they have a chance to induce security or stability problems.

Message validation is **required** when decoding messages received from a peer
to prevent bad data from propagating beyond the service entry point.

Message validation is **optional but recommended** when encoding messages to
send to a peer in order to help localize violated integrity constraints.

To minimize runtime overhead, validation should generally be performed as part
of a single pass message encoding or decoding process, such that only a single
traversal is needed. Since messages are encoded in depth-first traversal order,
traversal exhibits good memory locality and should therefore be quite efficient.

For simple messages, validation may be very trivial, amounting to no more than a
few size checks. While programmers are encouraged to rely on their FIDL bindings
library to validate messages on their behalf, validation can also be done
manually if needed.

Conformant FIDL bindings must check all of the following integrity constraints:

*   The total size of the message including all of its out-of-line sub-objects
    exactly equals the total size of the buffer that contains it. All
    sub-objects are accounted for.
*   The total number of handles referenced by the message exactly equals the
    total size of the handle table. All handles are accounted for.
*   The maximum recursion depth for complex objects is not exceeded.
*   All enum values fall within their defined range.
*   All union and xunion tag values fall within their defined range.
*   Encoding only:
    *   All pointers to sub-objects encountered during traversal refer precisely
        to the next buffer position where a sub-object is expected to appear. As
        a corollary, pointers never refer to locations outside of the buffer.
*   Decoding only:
    *   All present / not-present flags for referenced sub-objects hold the
        value **0** or **UINTPTR_MAX** only.
    *   All present / not-present flags for referenced handles hold the value
        **0** or **UINT32_MAX** only.

--------------------------------------------------------------------------------------------------

#### Footnote 1

Defining the zero handle to mean "there is no handle" means it is safe to default-initialize
wire format structures to all zeros.
Zero is also the value of the `ZX_HANDLE_INVALID` constant.

#### Footnote 2

Read [The Lost Art of Structure Packing][lostart] for an in-depth treatise on the subject.

[channel call]: /docs/zircon/syscalls/channel_call.md
[channel write]: /docs/zircon/syscalls/channel_write.md
[ftp-030]: /docs/development/languages/fidl/reference/ftp/ftp-030.md
[ordinal hashing]: /docs/development/languages/fidl/reference/ftp/ftp-020.md
[lostart]: http://www.catb.org/esr/structure-packing/
