# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os
import sys
import subprocess

ROOT_PATH = os.environ["FUCHSIA_DIR"]
FX_PATH = os.path.join(ROOT_PATH, "scripts", "fx")
FUCHSIA_BUILD_DIR = os.environ["FUCHSIA_BUILD_DIR"]
BUILDTOOLS_DIR = os.path.join(ROOT_PATH, "buildtools")

def _walk_up_path(path):
    res = set([path])
    while True:
        path, basename = os.path.split(path)
        if not path:
            break
        res.add(path)
    return res

def _find_cargo_target(path, target_filter=None):
    match_paths = _walk_up_path(path)
    all_targets = subprocess.check_output([FX_PATH, "build", "-t", "targets"])
    for gn_target in all_targets.split("\n"):
        target_parts = gn_target.split(":")
        if len(target_parts) < 2:
            continue
        target_path, gn_target = target_parts[0], target_parts[1]
        if target_path in match_paths and gn_target.endswith("_cargo"):
            gn_target=gn_target[:gn_target.rindex("_")]
            if target_filter and target_filter != gn_target:
                continue
            yield "{path}:{target}".format(
                    path=target_path,
                    target=gn_target,
            )

class GnTarget:
    def __init__(self, gn_target):
        gn_target = gn_target.lstrip("/")
        gn_target_parts = gn_target.split(":", 1)

        if gn_target_parts[0] == ".":
            cwd_rel_path = os.path.relpath(os.path.abspath("."), ROOT_PATH)
            target_filter = None if len(gn_target_parts) == 1 else gn_target_parts[1]
            gn_targets = list(_find_cargo_target(cwd_rel_path, target_filter))
            if not gn_targets:
                print "No cargo targets found at '{}'".format(cwd_rel_path)
                raise ValueError(gn_target)
            elif len(gn_targets) > 1:
                print "Multiple cargo targets found at '{}'".format(cwd_rel_path)
                for gn_target in gn_targets:
                    print "- {}".format(gn_target)
                raise ValueError(gn_target)
            else:
                gn_target, = gn_targets
                gn_target_parts = gn_target.split(":", 1)

        self.gn_target = gn_target
        self.parts = gn_target_parts

    def __str__(self):
        return self.gn_target

    @property
    def path(self):
        return os.path.join(ROOT_PATH, self.parts[0])

    def manifest_path(self, build_dir=None, prefer_host=False):
        if build_dir is None:
            build_dir = os.environ["FUCHSIA_BUILD_DIR"]

        if len(self.parts) == 1:
            # Turn foo/bar into foo/bar/bar
            path = os.path.join(self.gn_target, os.path.basename(self.gn_target))
        else:
            # Turn foo/bar:baz into foo/bar/baz
            path = self.gn_target.replace(":", os.sep)

        manifest_path_prefix = os.path.join(ROOT_PATH, build_dir)
        manifest_path_suffix = os.path.join("gen", path, "Cargo.toml")

        # By default, return the Fuchsia target Cargo.toml.
        # Fall back to the
        target_manifest_path = os.path.join(manifest_path_prefix, manifest_path_suffix)
        host_manifest_path = os.path.join(manifest_path_prefix, "host_x64", manifest_path_suffix)
        if not os.path.isfile(target_manifest_path) or prefer_host:
            return host_manifest_path
        else:
            return target_manifest_path

def get_rust_target_from_file(file):
    """Given a Rust file, return a GN target that references it. Raises ValueError if the file
    cannot be converted to a target."""
    if not file.endswith(".rs"):
        return None, "Not a Rust file."
    # Query ninja to find the output file.
    ninja_query_args = [
        os.path.join(BUILDTOOLS_DIR, "ninja"),
        "-C",
        FUCHSIA_BUILD_DIR,
        "-t",
        "query",
        os.path.relpath(file, FUCHSIA_BUILD_DIR),
    ]

    p = subprocess.Popen(
        ninja_query_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = p.communicate()
    if p.returncode:
        print err
        raise None

    # Expected Ninja query output is:
    # ../../filename.rs:
    #   outputs:
    #     rust_crates/binary
    lines = out.splitlines()
    if len(lines) < 3:
        print "Unexpected Ninja output: %s" % out
        return None

    output_files = [
        os.path.join(FUCHSIA_BUILD_DIR, l.strip()) for l in lines[2:]
    ]

    # For each output file in Ninja, check to see if it's produced by a Rust build
    # target. If so, return the base target name.
    for output_file in output_files:
        # Query GN to get the target that produced that output.
        gn_refs_args = [
            os.path.join(BUILDTOOLS_DIR, "gn"),
            "refs",
            FUCHSIA_BUILD_DIR,
            output_file,
        ]

        p = subprocess.Popen(
            gn_refs_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out, err = p.communicate()
        if p.returncode:
            print err
            raise None

        # Expected GN refs output is:
        # //path/to/target:bin_build
        # //path/to/target:bin_copy
        lines = out.splitlines()
        for line in lines:
            line = line.strip()
            if line.endswith("_build"):
                return GnTarget(line.rstrip("_build"))

    print "Unable to find Rust build target for %s" % file
    return None
