# Development

This document is a top-level entry point to all of Fuchsia documentation related
to **developing** Fuchsia and software running on Fuchsia.

## Developer workflow

This sections describes the workflows and tools for building, running, testing
and debugging Fuchsia and programs running on Fuchsia.

 - [Getting started](/docs/getting_started.md) - **start here**. This document
   covers getting the source, building and running Fuchsia.
 - [Source code](source_code/README.md)
 - [fx workflows](workflows/fx.md)
 - [Multiple device setup](workflows/multi_device.md)
 - [Pushing a package](workflows/package_update.md)
 - [Changes that span layers](workflows/multilayer_changes.md)
 - [Debugging](workflows/debugging.md)
 - [LibFuzzer-based fuzzing](workflows/libfuzzer.md)
 - [Build system](build/README.md)
 - [Workflow tips and FAQ](workflows/workflow_tips_and_faq.md)
 - [Testing FAQ](workflows/testing_faq.md)

## Languages

 - [README](languages/README.md) - Language usage in Fuchsia
 - [C/C++](languages/c-cpp/README.md)
 - [Dart](languages/dart/README.md)
 - [FIDL](languages/fidl/README.md)
 - [Go](languages/go/README.md)
 - [Rust](languages/rust/README.md)
 - [Python](languages/python/README.md)
 - [Flutter modules](languages/dart/mods.md) - how to write a graphical module
   using Flutter
 - [New language](languages/new/README.md) - how to bring a new language to Fuchsia

## API

 - [README](api/README.md) - Developing APIs for Fuchsia
 - [Council](api/council.md) - Definition of the API council
 - [System](api/system.md) - Rubric for designing the Zircon System Interface
 - [FIDL API][fidl-api] - Rubric for designing FIDL protocols
 - [FIDL style][fidl-style] - FIDL style rubric
 - [C](api/c.md) - Rubric for designing C library interfaces
 - [Tools](api/tools.md) - Rubrics for designing developer tools
 - [Devices](api/device_interfaces.md) - Rubric for designing device interfaces

## ABI

 - [System](abi/system.md) - Describes scope of the binary-stable Fuchsia System Interface

## SDK

 - [SDK](sdk/README.md) - information about developing the Fuchsia SDK

## Hardware

This section covers Fuchsia development hardware targets.

 - [Acer Switch Alpha 12][acer_12]
 - [Intel NUC][intel_nuc] (also [this](hardware/developing_on_nuc.md))
 - [Pixelbook](hardware/pixelbook.md)

## Testing

 - [Test components](testing/test_component.md)
 - [Test environments](testing/environments.md)
 - [Testability rubrics](testing/testability_rubric.md)
 - [Test flake policy](/docs/best-practices/test_flake_policy.md)
 - [Testing Isolated Cache Storage](testing/testing_isolated_cache_storage.md)

## Conventions

This section covers Fuchsia-wide conventions and best practices.

 - [Documentation standards](/docs/best-practices/documentation_standards.md)
 - [Endian Issues](source_code/endian.md) and recommendations

## Tracing

 - [Tracing homepage](tracing/README.md)
 - [Tracing Quick-Start Guide](tracing/quick-start/README.md)
 - [Tracing tutorial](tracing/tutorial.md)
 - [Tracing usage guide](tracing/usage-guide.md)
 - [Trace based benchmarking](benchmarking/trace_based_benchmarking.md)
 - [Tracing booting Fuchsia](tracing/tracing-boot.md)
 - [CPU Performance Monitor](tracing/cpuperf-provider.md)

## Miscellaneous

 - [CTU analysis in Zircon](workflows/ctu_analysis.md)
 - [Component Inspection](inspect/README.md)


[acer_12]: /zircon/docs/targets/acer12.md "Acer 12"
[intel_nuc]: /zircon/docs/targets/nuc.md "Intel NUC"
[pixelbook]: hardware/pixelbook.md "Pixelbook"
[fidl-style]: /docs/development/languages/fidl/style.md
[fidl-api]: /docs/development/api/fidl.md
