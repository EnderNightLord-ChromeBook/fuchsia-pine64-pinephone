#!/usr/bin/env python2.7
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import StringIO
import argparse
import functools
import json
import operator
import os
import sys
import tarfile
import time
import zipfile


def generate_script(images, board_name, type, additional_bootserver_arguments):
    # The bootserver must be in there or we lose.
    # TODO(mcgrathr): Multiple bootservers for different platforms
    # and switch in the script.
    [bootserver
    ] = [image['path'] for image in images if image['name'] == 'bootserver']
    script = '''\
#!/bin/sh
dir="$(dirname "$0")"
set -x
'''
    switches = dict(
        (switch, '"$dir/%s"' % image['path']) for image in images
        if type in image for switch in image[type])
    cmd = ['exec', '"$dir/%s"' % bootserver]
    if board_name:
        cmd += ['--board_name', '"%s"' % board_name]

    if additional_bootserver_arguments:
        cmd += [additional_bootserver_arguments]

    for switch, path in sorted(switches.iteritems()):
        cmd += [switch, path]
    cmd.append('"$@"')
    script += ' '.join(cmd) + '\n'
    return script


# Produces a gzip compressed tarball archive.
class CompressedTarArchiver(object):
    """Public interface needs to match {Nil,Zip}Archiver."""

    def __init__(self, outfile, compress=True):
        mode = 'w'
        # A mode of 'w:gz' adds gzip compression to the output file.
        self._archive = tarfile.open(outfile, 'w:gz', dereference=True)

    def __enter__(self):
        return self

    def __exit__(self, unused_type, unused_value, unused_traceback):
        self._archive.close()

    @staticmethod
    def _sanitize_tarinfo(executable, info):
        assert info.isfile()
        info.mode = 0o555 if executable else 0o444
        info.uid = 0
        info.gid = 0
        info.uname = ''
        info.gname = ''
        return info

    def add_path(self, path, name, executable):
        self._archive.add(
            path,
            name,
            filter=functools.partial(self._sanitize_tarinfo, executable))

    def add_contents(self, contents, name, executable):
        info = self._sanitize_tarinfo(executable, tarfile.TarInfo(name))
        info.size = len(contents)
        info.mtime = time.time()
        self._archive.addfile(info, StringIO.StringIO(contents))


def write_archive(outfile, images, board_name, additional_bootserver_arguments):
    # Synthesize a sanitized form of the input.
    path_images = []
    for image in images:
        path = image['path']
        if 'archive' in image:
            del image['archive']
        image['path'] = image['name'] + '.' + image['type']
        path_images.append((path, image))

    # Generate scripts that use the sanitized file names.
    content_images = [
        (
            generate_script(
                [image for path, image in path_images], board_name,
                'bootserver_pave', additional_bootserver_arguments), {
                    'name': 'pave',
                    'type': 'sh',
                    'path': 'pave.sh'
                }),
        (
            generate_script(
                [image for path, image in path_images], board_name,
                'bootserver_pave_zedboot', additional_bootserver_arguments +
                " --allow-zedboot-version-mismatch"), {
                    'name': 'pave-zedboot',
                    'type': 'sh',
                    'path': 'pave-zedboot.sh'
                }),
        (
            generate_script(
                [image for path, image in path_images], board_name,
                'bootserver_netboot', additional_bootserver_arguments), {
                    'name': 'netboot',
                    'type': 'sh',
                    'path': 'netboot.sh'
                })
    ]

    # Self-reference.
    content_images.append(
        (
            json.dumps(
                [image for _, image in (path_images + content_images)],
                indent=2,
                sort_keys=True), {
                    'name': 'images',
                    'type': 'json',
                    'path': 'images.json',
                }))

    # Canonicalize the order of the files in the archive.
    path_images = sorted(path_images, key=lambda pair: pair[1]['path'])
    content_images = sorted(content_images, key=lambda pair: pair[1]['path'])

    def is_executable(image):
        return image['type'] == 'sh' or image['type'].startswith('exe')

    with CompressedTarArchiver(outfile) as archiver:
        for path, image in path_images:
            archiver.add_path(path, image['path'], is_executable(image))
        for contents, image in content_images:
            archiver.add_contents(contents, image['path'], is_executable(image))


def main():
    parser = argparse.ArgumentParser(description='Pack Fuchsia build images.')
    parser.add_argument(
        '--depfile', metavar='FILE', help='Write Ninja dependencies file')
    parser.add_argument(
        'json',
        nargs='+',
        metavar='FILE',
        help='Read JSON image list from FILE')
    parser.add_argument(
        '--pave', metavar='FILE', help='Write paving bootserver script to FILE')
    parser.add_argument(
        '--pave_zedboot',
        metavar='FILE',
        help='Write zedboot paving bootserver script to FILE')
    parser.add_argument(
        '--netboot',
        metavar='FILE',
        help='Write netboot bootserver script to FILE')
    parser.add_argument(
        '--archive', metavar='FILE', help='Write archive to FILE')
    parser.add_argument('--board_name', help='Board name images were built for')
    parser.add_argument(
        '--additional_bootserver_arguments',
        action='append',
        default=[],
        help='additional arguments to pass to bootserver in generated scripts')
    args = parser.parse_args()

    # Keep track of every input file for the depfile.
    files_read = set()

    def read_json_file(filename):
        files_read.add(filename)
        with open(filename, 'r') as f:
            return json.load(f)

    images = reduce(
        operator.add, (read_json_file(file) for file in args.json), [])

    outfile = None

    # Write an executable script into outfile for the given bootserver mode.
    def write_script_for(outfile, mode):
        with os.fdopen(os.open(outfile, os.O_CREAT | os.O_TRUNC | os.O_WRONLY,
                               0o777), 'w') as script_file:
            script_file.write(
                generate_script(
                    images, args.board_name, mode,
                    ' '.join(args.additional_bootserver_arguments)))

    # First write the local scripts that work relative to the build directory.
    if args.pave:
        outfile = args.pave
        write_script_for(args.pave, 'bootserver_pave')
    if args.pave_zedboot:
        outfile = args.pave_zedboot
        write_script_for(args.pave_zedboot, 'bootserver_pave_zedboot')
    if args.netboot:
        outfile = args.netboot
        write_script_for(args.netboot, 'bootserver_netboot')

    if args.archive:
        outfile = args.archive
        archive_images = [
            image for image in images if image.get('archive', False)
        ]
        files_read |= set(image['path'] for image in archive_images)
        write_archive(
            outfile, archive_images, args.board_name,
            ' '.join(args.additional_bootserver_arguments))

    if outfile and args.depfile:
        with open(args.depfile, 'w') as depfile:
            depfile.write('%s: %s\n' % (outfile, ' '.join(sorted(files_read))))


if __name__ == "__main__":
    main()
