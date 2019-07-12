# Storage capabilities

[Storage capabilities][glossary-storage] are a way for components to define,
[offer][offer], and [use][use] directories, but they have different semantics
than [directory capabilities][directory-capabilities].

Directories provided by storage capabilities are guaranteed to be unique and
non-overlapping for each [component instance][component-instance], preventing
any component instances from accessing files belonging to any other component
instance (including their own children).

## Directory vs storage capabilities

As an example, if component instance `a` receives a _directory_ capability from
its [realm][realm] and both [uses][use] it and [offers][offer] it to `b`, which
also uses the directory, both component instances can see and interact with the
same directory.

```
<a's realm>
    |
    a
    |
    b

a.cml:
{
    "use": [ {"directory": "/example_dir" } ],
    "offer": [
        {
            "directory": "/example_dir",
            "from": "realm",
            "to": [ { "dest": "#b" } ],
        },
    ],
}

b.cml:
{
    "use": [ {"directory": "/example_dir" } ],
}
```

In this example if component instance `a` creates a file named `hippos` inside
`/example_dir` then `b` will be able to see and read this file.

If the component instances use storage capabilities instead of directory
capabilities, then component instance `b` cannot see and read the `hippos` file.

```
<a's realm>
    |
    a
    |
    b

a.cml:
{
    "use": [ { "storage": "data", "as": "/example_dir" } ],
    "offer": [
        {
            "storage": "data",
            "from": "realm",
            "to": [ { "dest": "#b" } ],
        },
    ],
}

b.cml:
{
    "use": [ { "storage": "data", "as": "/example_dir" } ],
}
```

In this example any files that `a` creates are not be visible to `b`, as
storage capabilities provide unique non-overlapping directories to each
component instance.

## Creating storage capabilities

Storage capabilities can be created with a [`storage`
declaration][storage-syntax] in a [component manifest][manifests]. Once storage
capabilities have been declared, they can then be offered to other component
instances by referencing the declaration by name.

A `storage` declaration must include a reference to a directory capability,
which is the directory from which the component manager will create isolated
directories for each component instance using the storage capability.

For example, the following manifest describes new storage capabilities backed
by the `/memfs` directory exposed by the child named `memfs`. From this storage
declaration a data storage capability is offered to the child named
`storage_user`.

```

{
    "storage": [
        {
            "name": "mystorage",
            "from": "#memfs",
            "path": "/memfs",
        },
    ],
    "offer": [
        {
            "storage": "data",
            "from": "#mystorage",
            "to": [ { "dest": "#storage_user" } ],
        },
    ],
    "children": [
        { "name": "memfs", "url": "fuchsia-pkg://..." },
        { "name": "storage_user", "url": "fuchsia-pkg://...", },
    ],
}
```

## Storage capability semantics

A directory capability that backs storage capabilities can be used to access the
files of any component that uses the resulting storage capabilities. This type
of directory capability should be routed carefully to avoid exposing this
capability to too many component instances.

When a component instance attempts to access the directory provided to it
through a storage capability, the framework binds to and generates
sub-directories in the component instance that provides the backing directory
capability. Then, the framework provides the component instance access to a
unique sub-directory.

The sub-directory to which a component instance is provided access is determined
by the type of storage and its location in the component topology. This means
that if a component instance is renamed in its parent manifest or moved to a
different parent then it will receive a different sub-directory than it did
before the change.

[component-instance]: ../../glossary.md#component-instance
[directory-capabilities]: ../../glossary.md#directory-capability
[glossary-storage]: ../../glossary.md#storage-capability
[manifests]: component_manifests.md
[offer]: ../../glossary.md#offer
[realm]: ../../glossary.md#realm
[storage-syntax]: component_manifests.md#storage
[use-syntax]: component_manifests.md#use
[use]: ../../glossary.md#use
