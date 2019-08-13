# Testing

## Types of tests

Multiple Dart test targets are available:

- [dart_fuchsia_test] runs the test as a package on a fuchsia device. This must
  be used if there is anything fuchsia specific being used like fidl. It is run
  with `fx run-test`.
- [dart_test] runs unit tests that can be run on the host or on a fuchsia
  device. The dart:ui package is not made available to these tests. The test can
  be run with `fx run-host-tests`.
- [flutter_test] is just like dart_test except the dart:ui package is made
  available to it, so it can test widget code.

For information on integration testing [Flutter mods](mods.md), see [mod
integration testing](mod_integration_testing.md).

Note that in order to be built and run on bots, the test targets need to be
included in the packages that are configured to run there. For example, in
topaz this can be achieved by adding those tests to `//topaz/packages/tests`.

## Code coverage

To generate an HTML coverage report from all `dart_test`s, first build them
with `fx build` and then run:

```sh
scripts/dart/report_coverage.py --report-dir ...
```

This script runs all of the dart tests in your `<out>/host_tests/` dir with
coverage enabled. Under the hood, each test uses the coverage collection support
from [flutter](
https://github.com/flutter/flutter/wiki/Test-coverage-for-package:flutter).

The basic logic is:

```
for test in host_tests:
  covered_lines += lines in test's package that were covered by test
  total_lines += all lines in test's package
```

So if there are packages that have no tests at all, they won't be considered in
the denominator of the report, which can give you a misleadingly high coverage
number.

[dart_fuchsia_test]: https://fuchsia.googlesource.com/topaz/+/master/runtime/dart/dart_fuchsia_test.gni
[dart_test]: /build/dart/test.gni
[flutter_test]: https://fuchsia.googlesource.com/topaz/+/master/runtime/dart/flutter_test.gni