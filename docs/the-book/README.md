# Fuchsia is not Linux
_A modular, capability-based operating system_

This document is a collection of articles describing the Fuchsia operating system,
organized around particular subsystems. Sections will be populated over time.

[TOC]

## Zircon Kernel

Zircon is the microkernel underlying the rest of Fuchsia. Zircon
also provides core drivers and Fuchsia's libc implementation.

 - [Concepts][zircon-concepts]
 - [System Calls][zircon-syscalls]
 - [vDSO (libzircon)][zircon-vdso]

## Zircon Core

 - Device Manager & Device Hosts
 - [Device Driver Model (DDK)][zircon-ddk]
 - [C Library (libc)](libc.md)
 - [POSIX I/O (libfdio)](life_of_an_open.md)
 - [Process Creation](process_creation.md)

## Framework

 - [Overview][framework-overview]
 - [Core Libraries](core_libraries.md)
 - Application model
   - [Interface definition language (FIDL)][FIDL]
   - Services
   - Environments
 - [Boot sequence](boot_sequence.md)
 - Device, user, and story runners
 - Components
 - [Namespaces](namespaces.md)
 - [Sandboxing](sandboxing.md)
 - [Story][framework-story]
 - [Module][framework-module]
 - [Agent][framework-agent]

## Storage

 - [Block devices](block_devices.md)
 - [File systems](filesystems.md)
 - Directory hierarchy
 - [Ledger][ledger]
 - Document store
 - Application cache

## Networking

 - Ethernet
 - [Wireless](wireless_networking.md)
 - [Bluetooth](bluetooth_architecture.md)
 - [Telephony][telephony]
 - Sockets
 - HTTP

## Graphics

 - [UI Overview][ui-overview]
 - [Magma (vulkan driver)][magma]
 - [Escher (physically-based renderer)][escher]
 - [Scenic (compositor)][scenic]
 - [Input manager][input-manager]
 - [Flutter (UI toolkit)][flutter]

## Components

 - [Component framework](components/README.md)

## Media

 - Audio
 - Video
 - DRM

## Intelligence

 - Context
 - Agent Framework
 - Suggestions

## User interface

 - Device, user, and story shells
 - Stories and modules

## Backwards compatibility

 - POSIX lite (what subset of POSIX we support and why)
 - Web runtime

## Update and recovery

 - Verified boot
 - Updater

[zircon-concepts]: /docs/zircon/concepts.md
[zircon-syscalls]: /docs/zircon/syscalls.md
[zircon-vdso]: /docs/zircon/vdso.md
[zircon-ddk]: /docs/zircon/ddk/overview.md
[FIDL]: ../development/languages/fidl/README.md
[framework-overview]: modular/overview.md
[framework-story]: modular/story.md
[framework-module]: modular/module.md
[framework-agent]: modular/agent.md
[ledger]: /src/ledger/docs/README.md
[bluetooth]: /garnet/bin/bluetooth/README.md
[telephony]: /src/connectivity/telephony/
[magma]: magma/README.md
[escher]: /src/ui/lib/escher/README.md
[ui-overview]: ui/README.md
[scenic]: ui/scenic.md
[input-manager]: ui/input.md
[flutter]: https://flutter.dev/
