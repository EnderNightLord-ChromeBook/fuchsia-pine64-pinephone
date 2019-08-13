# zxdb: Fuchsia native debugger user guide

This is the command usage guide for zxdb. Please see also:

  * The [setup and troubleshooting guide](README.md).

## Quick start

### Connecting in-tree

In-tree developers should use the `fx debug` command to start the debugger. The
system must already be running and reachable via networking from your computer:

```
$ scripts/fx debug
Attempting to start the Debug Agent.
Waiting for the Debug Agent to start.
Connecting (use "disconnect" to cancel)...
Connected successfully.
[zxdb]
```

The `status` command will give the current state of the debugger. Be aware if
the remote system dies the debugger won't always notice the connection is gone.

### Debugging a process or component.

Running a process on Fuchsia is more complicated than in other systems because
there are different loader environments (see "A note about launcher
environments" below).

The only want to reliably debug all types of processes is to create a filter on
the process name via "attach" and start it the normal way you would start that
process. The process name is usually the name of the build target that
generates it. To check what this is, use "ps" (either in the debugger or from a
system shell) with it running.

> Note: only the first 32 bytes of the name are included in the Zircon process
> description. Sometimes the number of path components can cause the name to be
> truncated. If the filter isn't working, check the actual name in "ps". We hope
> to have a better way to match this in the future.

This example sets a pending breakpoint on `main` (Rust users: use "@main") to
stop at the beginning of execution, and waits for a process called "my_app" to
start:

```
[zxdb] attach my_app
Waiting for process matching "my_app"

[zxdb] break main
Breakpoint 1 (Software) on Global, Enabled, stop=All, @ main
Pending: No matches for location, it will be pending library loads.
```

Then run the process the way you would in normal use (direcly on the command
line, via `fx run-test`, via the shell's `run fuchsia-pkg://...`, or another
way. The debugger should then immediately break on `main` (it may take some
time to load symbols so you may see a delay before showing the source code):

```
Attached Process 1 [Running] koid=51590 my_app.cmx
🛑 on bp 1 main(…) • main.cc:222
   220 }
   221
 ▶ 222 int main(int argc, const char* argv[]) {
   223   foo::CommandLineOptions options;
   224   cmdline::Status status = ParseCommandLine(argc, argv, &options);
```

You can then do basic commands that are similar to GDB:

```
next
step
print argv[1]
continue
quit
```

#### A note about launcher environments

The following loader environments all have different capabilities (in order
from least capable to most capable):

  * The debugger's `run <file name>` command (base system process stuff).
  * The system console or `fx shell` (adds some libraries).
  * The base component environment via the shell's `run` and the debugger's
    `run -c <package url>` (adds component capabilities).
  * The test environment via `fx run-test`.
  * The user environment when launched from a "story" (adds high-level
    services like scenic).

This panoply of environments is why the debugger can't have a simple "run"
command that always works.

### Launching simple command-line processes

Minimal console apps including some unit tests can be launched directly from
within the debugger which avoids the "attach" dance:

```
[zxdb] break @main
Breakpoint 1 (Software) on Global, Enabled, stop=All, @ @main
Pending: No matches for location, it will be pending library loads.

[zxdb] run /bin/cowsay
```

> The "@main" symbol will match the process' entrypoint which is particularly
> useful for Rust where things live in a namespace. For C/C++, it's equivalent
> to "main".

If you get a shared library load error or errors about files or services not
being found, it means the app can't be run from within the debugger's launcher
environment. This is true even for things that may seem relatively simple.

### Directly launching components

Components that can be executed with the console comand `run fuchsia-pkg://...`
can be loaded in the debugger with the following command, substituting your
component's URL:

```
[zxdb] run -c fuchsia-pkg://fuchsia.com/your_app#meta/your_app.cmx
```

Not all components can be launched this way since most higher-level services
won't be accessible: if you can't do `run ...` from the system console, it
won't work from the debugger either. Note also that `fx run-test` is a
different environment. According to your test's dependencies, it may or may not
work from the debugger's `run` command. 

### Attaching to an existing process

You can attach to most running processes given the process’ KOID. You can get
the KOID by running `ps` on the target Fuchsia system. zxdb also has a built-in
`ps` command:

```
[zxdb] ps
j: 1030 root
  j: 1079 zircon-drivers
    p: 1926 devhost:sys
...
```

Then to attach:

```
[zxdb] attach 3517
Process 1 Running koid=1249 pwrbtn-monitor
```

When you’re done, you can choose to `detach` (keep running) or `kill`
(terminate) the process.

## Interaction model

Most command-line debuggers use an exclusive model for input: you’re either
interacting with the debugged process’ stdin and stdout, or you’re interacting
with the debugger. In contrast, zxdb has an asynchronous model similar to most
GUI debuggers. In this model, the user is exclusively interacting with the
debugger while arbitrary processes or threads are running or stopped.

When the debugger itself launches a program it will print the program's stdout
and stderr to the console. When you attach (either with a filter or with the
`attach` command) they will go to the original place. Currently there is no way
to interact with a process’ stdin.

zxdb has a regular noun/verb model for typed commands. The rest of this section
gives an overview of the syntax that applies to all commands. Specific commands
will be covered in the “Task guide” section below.

### Nouns

The possible nouns (and their abbreviations) are:

  * `process` (`pr`)
  * `job` (`j`)
  * `thread` (`t`)
  * `frame` (`f`)
  * `breakpoint` (`bp`)

#### Listing nouns

If you type a noun by itself, it lists the available objects of that type:

  * List attached processes

    ```
    [zxdb] process
      # State       Koid Name
    ▶ 1 Not running 3471 debug_agent_unit_tests.cmx
    ```

  * List attached jobs

    ```
    [zxdb] job
      # State   Koid Name
    ▶ 1 running 3471 sys
    ```

  * List threads in the current process:

    ```
    [zxdb] thread
      # State   Koid Name
    ▶ 1 Blocked 1348 initial-thread
      2 Blocked 1356 some-other-thread
    ```

  * List stack frames in the current thread (the thread must be stopped—see
    `pause` below):

    ```
    [zxdb] frame
    ▶ 0 fxl::CommandLineFromIterators<const char *const *>() • command_line.h:203
      1 fxl::CommandLineFromArgcArgv() • command_line.h:224
      2 main() • main.cc:174
    ```

#### Selecting defaults

If you type a noun and its index, you select that as the default for subsequent
commands. It also tells you the stats about the new default.

  * Select thread 3 to be the default for future commands:

    ```
    [zxdb] thread 3
    Thread 3 Blocked koid=9940 worker-thread
    ```

  * Select breakpoint 2 to be the default:

    ```
    [zxdb] breakpoint 2
    Breakpoint 2 (Software) on Global, Enabled, stop=All, @ MyFunction
    ```

### Verbs

By default, a verb (`run`, `next`, `print`, etc.) applies to the current
defaults. So to evaluate an expression in the context of the current stack
frame, just type `print` by itself:

```
[zxdb] print argv[1]
"--foo=bar"
```

You can override the default context by prefixing the verb with a noun and its
index. So to evaluate an expression in the context of a specific stack frame
(in this case, frame 2 of the current thread):

```
[zxdb] frame 2 print argv[1]
"--foo=bar"
```

You can keep adding different types of context. This specifies the process,
thread, and frame for the print command:

```
[zxdb] process 1 thread 1 frame 2 print argv[1]
"--foo=bar"
```

# Attaching and running

### Debugging drivers

It's not currently possible to set up the debugger early enough in system
startup to debug most driver initialization. And since zxdb itself uses the
network, no drivers associated with network communication can be debugged.

Driver debugging support is tracked in
[DX-598](https://fuchsia.atlassian.net/browse/DX-598).

You can debug running drivers by attaching like any other process (see
“Attaching to an existing process” below). You can delay initialization to
allow time to attach by adding a busyloop at the beginning of your code:

```
volatile bool stop = false;
while (!stop) {}
```

To break out of the loop after attaching, either set the variable to true:

```
[zxdb] print stop = true
true
[zxdb] continue
```

Or jump to the line after the loop:

```
[zxdb] jump <line #>
[zxdb] continue
```

### Debugging crash dumps

You can load a minidump generated by a crash report. Use the "opendump" verb
and supply the local file name of the dump. The debugger must not be attached
to another dump or a running system (use "disconnect" first if so).

```
[zxdb] opendump upload_file_minidump-e71256ba30163a0.dmp
Opening dump file
Dump loaded successfully.
```

Now the thread, stack, and memory commands can be used to inspect the state of
the program. Use "disconnect" to close the dump.

For in-tree users, the `fx debug` command can take the path to a core file as an argument.

```
fx debug -c upload_file_minidump-e71256ba30163a0.dmp
```

#### Downloading symbols

You can tell `zxdb` to look for debug symbols for your core dump in a GCS bucket and download them
automatically. You'll need to run with a few command-line options:

```
zxdb --symbol-cache $HOME --symbol-server gs://my-bucket-name
```

In-tree users will automatically have these options set, with the server pointed to a bucket
containing symbols for all release builds.

The first time you use the symbol server, you will have to authenticate using the `auth` command.
The authentication flow will require you to complete part of the authentication in your browser.

```
[zxdb] auth
To authenticate, please supply an authentication token. You can retrieve a token from:

https://accounts.google.com/o/oauth2/v2/< very long URL omitted >

Once you've retrieved a token, run 'auth <token>'

[zxdb] auth 4/hAF-pASODIFUASDIFUASODIUFSADF329827349872V6
Successfully authenticated with gs://fuchsia-infra-debug-symbols
```

### Debugging multiple processes

You can debug many arbitrary processes at the same time. When you start, one
“process context” (the container that may or may not have a running process)
 is created for you to use. When you run or attach, that process because
associated with that context.

To debug a second program, create a new context with:

```
[zxdb] process new
```

This will clone the current process’ settings into a new context but not run
anything yet. You can then run or attach as normal.

Recall from the “Interaction model” section you can list the current processes
with:

```
[zxdb] process
  # State       Koid Name
▶ 1 Running     1249 pwrbtn-monitor
  2 Not running 7235 pwrbtn-monitor
```

Select one of those as the default by providing its index (not KOID):

```
[zxdb] process 2
```

Or apply commands to a specific process (even if it’s not the default) with:

```
[zxdb] process 2 pause
```

# Running

### Working with breakpoints

Breakpoints stop execution when some code is executed. To create a breakpoint,
use the `break` command (`b` for short) and give it a location:

```
[zxdb] break main
Breakpoint 3 (Software) on Global, Enabled, stop=All, @ main
   180
 ◉ 181 int main(int argc, char**argv) {
   182     fbl::unique_fd dirfd;
```

A location can be expressed in many different ways.

  * Plain function name:

    ```
    break main
    ```

    There is a special symbol "@main" that matches the entrypoint of the process.
    This is useful for Rust where the entrypoint is usually in a namespace:

    ```
    break @main
    ```

  * Member function or functions inside namespaces:

    ```
    break my_namespace::MyClass::MyFunction
    ```

  * Source file + line number (separate with a colon):

    ```
    break mymain.cc:22
    ```

  * Line number within the current frame’s current source file (useful when
    stepping):

    ```
    break 23
    ```

  * Memory address:

    ```
    break 0xf72419a01
    ```

To list all breakpoints:

```
[zxdb] breakpoint
```

> Note: this is the “breakpoint” noun (a noun by itself lists the things
> associated with it). It is not plural.

To clear a specific breakpoint, give that breakpoint index as the context for
the clear command (see “Interaction model” above). Here’s we’re using the
abbreviation for `breakpoint` (`bp`):

```
[zxdb] bp 2 clear
```

Or you can clear the current breakpoint:

```
[zxdb] clear
```

Whenever you create or stop on a breakpoint, that breakpoint becomes the
default automatically so clear always clears the one you just hit. Note that
unlike GDB, “clear” takes a breakpoint context before the verb and there are
never any arguments after it. Support for GDB-like “clear <location>” is
[DX-594](https://fuchsia.atlassian.net/browse/DX-594).

### Programatic breakpoints

You can insert a hardcoded breakpoint in your code if you want to catch some
specific condition. Clang has a builtin (it won't work in GCC Zircon builds):

```
__builtin_debugtrap();
```

If the debugger is already attached to the process, it will stop as if a normal
breakpoint was hit. You can step or continue from there. If the debugger is
not already attached, this will cause a crash.

### Working with threads

To list the current process’ threads (see “Interaction model” above for more):

```
[zxdb] thread
  # State   Koid Name
▶ 1 Blocked 1323 initial-thread
  2 Running 3462 worker-thread
```

Often when you attach to a process the thread will be “blocked”, meaning it is
stopped on a system call. For asynchronous programs this will typically be some
kind of wait.

Most thread control and introspection commands only work when a thread is
suspended (not blocked or running). A thread will be suspended when it is
stopped at a breakpoint or crashes. You can explicitly suspend a thread with
the `pause` command:

```
[zxdb] thread 2 pause
🛑 syscalls-x86-64.S:67
   65 m_syscall zx_port_create 60 2 1
   66 m_syscall zx_port_queue 61 2 1
 ▶ 67 m_syscall zx_port_wait 62 3 0
   68 m_syscall zx_port_cancel 63 3 1
   69 m_syscall zx_timer_create 64 3 1
```

> When a thread is paused the debugger will show the current source code
> location. Often threads will be in a system call which will resolve to the
> location in the assembly-language macro file that generated the system call
> as shown in the above example.

Running `pause` by itself with no context will pause all threads of all
processes currently attached:

```
[zxdb] pause
```

Unpause a thread with `continue`. As before, `continue` with no context will
resume all threads:

```
[zxdb] continue
```

Or continue a specific thread:

```
[zxdb] thread 1 continue
```

### Working with stack frames

A stack frame is a function call. When a function calls another function, a new
nested frame is created. So listing the frames of a thread tells you the call
stack. You can only see the stack frames when a thread is suspended (see
“Working with threads” above).

To list the current thread’s stack frames (the `f` abbreviation will also
work).

```
[zxdb] frame
▶ 0 fxl::CommandLineFromIterators<const char *const *>() • command_line.h:203
  1 fxl::CommandLineFromArgcArgv() • command_line.h:224
  2 main() • main.cc:174
```

And to select a given frame as the default:

```
[zxdb] frame 2
```

Frames are numbered with “0” being the top of the stack. Increasing numbers go
backwards in time.

For more context, you can use the `backtrace` command. This is identical
to `frame` but gives more detailed address information as well as function
parameters. This command can be abbreviated `bt`:

```
[zxdb] bt
▶ 0 fxl::CommandLineFromIteratorsFindFirstPositionalArg<const char *const *>() • command_line.h:185
      IP = 0x10f982cf2ad0, BP = 0x66b45a01af50, SP = 0x66b45a01af38
      first = (const char* const*) 0x59f4e1268dc0
      last = (const char* const*) 0x59f4e1268dc8
      first_positional_arg = (const char* const**) 0x0
  1 fxl::CommandLineFromIterators<const char *const *>() • command_line.h:204
      IP = 0x10f982cf2ac0, BP = 0x66b45a01af50, SP = 0x66b45a01af40
      first = <'first' is not available at this address. >
      last = <'last' is not available at this address. >
...
```

Each stack frame has a code location. Use the `list` command to look at source
code. By itself, it lists the source code around the current stack frame’s
instruction pointer:

```
[zxdb] list
   183 inline CommandLine CommandLineFromIteratorsFindFirstPositionalArg(
   184     InputIterator first, InputIterator last,
 ▶ 185     InputIterator* first_positional_arg) {
   186   if (first_positional_arg)
   187     *first_positional_arg = last;
```

You can list code around the current instruction pointer of other stack frames,
too:

```
[zxdb] frame 3 list
```

Or you can list specific things like functions:

```
[zxdb] list MyClass::MyFunc
```

File/line numbers:

```
[zxdb] list foo.cc:43
```

Or whole files:

```
[zxdb] list --all myfile.cc:1
```

### Printing values

The `print` command can evaluate simple C/C++ expressions in the context of a
stack frame. When a thread is suspended (see “Working with threads” above) just
type:

```
[zxdb] print i
34
```

Expressions can use most simple C/C++ syntax:

```
[zxdb] print &foo->bar[baz]
(const MyStruct*) 0x59f4e1268f70

```

You can also evaluate expressions in the context of other stack frames without
switching to them (see “Interaction model” above for more):

```
[zxdb] frame 2 print argv[0]
"/bin/cowsay"
```

Often you will want to see all local variables:

```
[zxdb] locals
argc = 1
argv = (const char* const*) 0x59999ec02dc0
```

You can also set variables to integer and boolean values (as long as those
variables are in memory and not registers):

```
[zxdb] print done_flag = true
true
[zddb] print i = 56
56
```

Things that don’t currently work are:

  * Math ([DX-600](https://fuchsia.atlassian.net/browse/DX-600))
  * Function calls ([DX-599](https://fuchsia.atlassian.net/browse/DX-599))
  * Pretty-printing (especially for STL) ([DX-601](https://fuchsia.atlassian.net/browse/DX-601))
  * Various Rust-isms (please file feature requests!).

### Controlling execution (stepping, etc.)

When a thread is suspended (see “Working with threads” above) you can control
its execution:

`next` / `n`: Advances to the next line, stepping over function calls.

```
[zxdb] n
```

`step` / `s`: Advances to the next line. If a function call happens before the
next line, that function will be stepped into and execution will stop at the
beginning of it. You can also supply an argument which is a substring to match
of a specific function call. Function names not containing this substring will
be skipped and only matching ones will be stepped into:

```
[zxdb] s
[zxdb] s MyFunction
```

`finish` / `fi`: Exits the function and stops right after the call.

```
[zxdb] finish
```

`until` / `u`: Given a location (the same as breakpoints, see above), continues
the thread until execution gets there. For example, to run until line 45 of the
current file:

```
[zxdb] u 45
```

`jump`: Move the instruction pointer to a new address.

```
[zxdb] jump 22  // Line number
[zxdb] jump 0x87534123  // Address
```

There different things you can do with context. For example, to run until
execution gets back to a given stack frame:

```
[zxdb] frame 2 until
```

### Assembly language

There are commands that deal with assembly language:

  * `disassemble` / `di`: Disassemble at the current location (or a given
    location)

  * `nexti` / `ni`: Step to the next instruction, stepping over function calls.

  * `stepi` / `si`: Step the next instruction, following function calls.

  * `regs`: Get the CPU registers.

zxdb maintains information about whether the last command was an assembly
command or a source-code and will show that information on stepping or
breakpoint hits. To switch to assembly-language mode, type `disassemble`, and
to switch back to source-code mode, type `list`.

### Low-level memory

  * `mem-read` / `x`: Dumps memory

  * `stack`: Provides a low-level analysis of the stack. This is a handy
    command for low-level debugging.
