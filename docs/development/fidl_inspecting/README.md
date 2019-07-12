# fidlcat: Monitor and debug your fidl calls

## Overview

fidlcat is a tool that allows users to monitor FIDL connections. Currently, it
can attach to or launch a process on a Fuchsia device, and will report its FIDL
traffic.

## Enabling it

To run fidlcat in-tree, you first build it, which you can do the following way:

```sh
fx set <whatever> --with //bundles:tools
fx build
```

If you want to add it to your existing gn args, you can do so by adding this
stanza to the bottom of your <build_dir>/args.gn.

```
universe_package_labels += [ "//bundles:tools" ]
```

To run fidlcat, you must boot with networking enabled.

For QEMU networking support, you need to setup your system with a TUN/TAP
interface. Then, run:

```sh
fx run -N
```

In a separate console, you need to ensure your target is able to fetch updates:

```sh
fx serve
```

## Running it

When your environment is properly set up, and fidlcat is built, you should be
able to use it to monitor FIDL messages from processes on the target. There are
several ways to do this.

### Attaching to a running process

If you run the `ps` command in the shell, you can get a pid you want to monitor,
and run:

```sh
fx fidlcat --remote-pid <pid>
```

If your code is executed by a runner, you are likely to want to attach to the
runner. For Dart JIT-executed code, run `ps` on the target, and look for the process named `dart_jit_runner`:

```sh
host$ fx shell ps
[...]
        j:21102           17.6M   17.6M
          p:21107         17.6M   17.6M     32k         dart_jit_runner.cmx
```

You can then attach directly to that process, and view all FIDL messages sent by
Dart programs:

```sh
host$ fx fidlcat --remote-pid 21107
```

You can use the `--remote-pid` flag multiple times to connect to multiple processes:

```sh
fx fidlcat --remote-pid <pid1> --remote-pid <pid2>
```

### Launching a component with fidlcat

Alternatively, you can launch a component directly using its URL:

```sh
fx fidlcat run fuchsia-pkg://fuchsia.com/echo_client_rust#meta/echo_client_rust.cmx
```

### Attaching to a program on startup

You can also attach to programs that have not started yet by passing a regex to
match their names. If you issue the following command, fidlcat will connect to
the system, wait for a program with the substring "echo_client" to start, and
automatically attach to it.

```sh
fx fidlcat --remote-name echo_client
```

## Running without the fx tool

Note that fidlcat needs two sources of information to work:

 * First, it needs the symbols for the executable. In practice, if you are
   running in-tree, the symbols should be provided to fidlcat automatically.
   Otherwise, you can provide fidlcat a symbol path, which can be a text file
   that maps build ids to debug symbols, an explicit ELF file path, or a
   directory it will scan for ELF files and index. This can be provided to
   fidlcat with the `-s` flag.

 * Second, it needs the intermediate representation for the FIDL it ingests, so
   it can produce readable output. If you are running in-tree, the IR should be
   provided to fidlcat automatically. Otherwise, you can provide fidlcat an IR
   path, which can be an explicit IR file path, a directory it will scan for IR
   files, or an argument file containing explicit paths. This can be provided
   to fidlcat with the `--fidl-ir-path` flag. The argument files need to be
   prepended with a `@` character: `--fidl-ir-path @argfile`.

Finally, if you are running fidlcat without the fx tool, the debug agent needs
to be running on the target. Connect to the target and run:

```sh
run fuchsia-pkg://fuchsia.com/debug_agent#meta/debug_agent.cmx --port=8080
```

And, when you run fidlcat on the host, make sure you connect to that agent:

```sh
tools/fidlcat --connect [$(fx netaddr --fuchsia)]:8080 <other args>
```

## Where is the code?

The code is located in `//tools/fidlcat`.
