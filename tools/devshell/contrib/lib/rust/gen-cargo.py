#!/usr/bin/env python2.7
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

### generates symlinks to Rust Cargo.toml output files

import argparse
import os
import subprocess
import sys

import rust
from rust import ROOT_PATH

def main():
    parser = argparse.ArgumentParser(
            "Generate symlinks to Rust Cargo.toml output files")
    parser.add_argument("gn_target",
                        type=rust.GnTarget,
                        help="GN target to generate a symlink for. \
                              Use '.[:target]' to discover the cargo target \
                              for the current directory or use the \
                              absolute path to the target \
                              (relative to $FUCHSIA_DIR). \
                              For example: //garnet/bin/foo/bar:baz")
    parser.add_argument("--output",
                        help="Path to Cargo.toml to generate",
                        required=False)
    parser.add_argument("--out-dir",
                        help="Path to the Fuchsia build directory",
                        required=False)
    args = parser.parse_args()

    if args.out_dir:
        build_dir = args.out_dir
    else:
        build_dir = None

    path = args.gn_target.manifest_path(build_dir)

    if args.output:
        output = args.output
    else:
        output = os.path.join(args.gn_target.path, "Cargo.toml")

    if os.path.exists(path):
        try:
            os.remove(output)
        except OSError:
            pass
        print "Creating '{}' pointing to '{}'".format(output, path)
        os.symlink(path, output)
    else:
        print "Internal error: path '{}' does not point to a Cargo.toml file".format(path)

if __name__ == '__main__':
    sys.exit(main())

