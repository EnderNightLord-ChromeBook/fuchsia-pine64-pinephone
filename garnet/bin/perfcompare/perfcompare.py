# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This tool is for invoking by the performance comparison trybots.  It
# is intended for comparing the performance of two versions of
# Fuchsia.  It can also compare binary sizes.

import argparse
import json
import math
import os
import sys
import tarfile

import scipy.stats


# For comparing results from a performance test, we calculate
# confidence intervals for the mean running times of the test.  If the
# confidence intervals are non-overlapping, we conclude that the
# performance has improved or regressed for this test.
#
# Data is gathered from a 3-level sampling process:
#
#  1) Boot Fuchsia multiple times.
#  2) For each boot, launch the perf test process one or more times.
#  3) For each process launch, instantiate the performance test and
#     run the body of the test some number of times.
#
# This is intended to account for variation across boots and across process
# launches.
#
# Currently we use t-test confidence intervals.  This assumes that the
# values we apply the t-test to are normally distributed, or approximately
# normally distributed.  In future we could instead use bootstrap
# confidence intervals, which would avoid that assumption.


# ALPHA is a parameter for calculating confidence intervals.  It is
# the probability that the true value for the statistic we're
# estimating (here, the mean running time) lies outside the confidence
# interval.
ALPHA = 0.01


def Mean(values):
    if len(values) == 0:
        raise AssertionError('Mean is not defined for an empty sample')
    return float(sum(values)) / len(values)


# Returns the mean and standard deviation of a sample.  This applies
# Bessel's correction to the calculation of the standard deviation.
def MeanAndStddev(values):
    if len(values) <= 1:
        raise AssertionError(
            "Sample size of %d is too small to calculate standard deviation "
            "with Bessel's correction" % len(values))
    mean_val = Mean(values)
    sum_of_squares = 0.0
    for val in values:
        diff = val - mean_val
        sum_of_squares += diff * diff
    stddev_val = math.sqrt(sum_of_squares / (len(values) - 1))
    return mean_val, stddev_val


class Stats(object):

    def __init__(self, values):
        sample_size = len(values)
        mean, stddev = MeanAndStddev(values)
        offset = (-scipy.stats.t.ppf(ALPHA / 2, sample_size - 1)
                  * stddev / math.sqrt(sample_size))
        self._mean = mean
        self._offset = offset
        # Confidence interval for the mean.
        self.interval = (mean - offset, mean + offset)

    def FormatConfidenceInterval(self):
        return '%d +/- %d' % (self._mean, self._offset)

    # Returns the relative CI width, which is the width of the confidence
    # interval divided by the mean.
    def RelativeConfidenceIntervalWidth(self):
        return self._offset * 2 / self._mean


def ReadJsonFile(filename):
    with open(filename, 'r') as fh:
        return json.load(fh)


def IsResultsFilename(name):
    return name.endswith('.json') and name != 'summary.json'


# Read the raw perf test results from a directory or a tar file that
# contains results from a single boot of Fuchsia.  Returns a sequence
# (iterator) of JSON trees.
#
# Accepting tar files here is a convenience for when doing local testing of
# the statistics.  The Swarming system used for the bots produces "out.tar"
# files as results.
#
# The directory (or tar file) is expected to contain files with names
# of the following forms:
#
#   <name-of-test-executable>_process<number>.json - results that are read here
#   <name-of-test-executable>_process<number>.catapult_json - ignored here
#   summary.json - ignored here
#
# Each *.json file (except for summary.json) contains results from a
# separate launch of a performance test process.
def RawResultsFromDir(filename):
    # Note that sorting the filename listing (from os.listdir() or from
    # tarfile) is not essential, but it helps to make any later processing
    # more deterministic.
    if os.path.isfile(filename):
        # Read from tar file.
        with tarfile.open(filename) as tar:
            for member in sorted(tar.getmembers(),
                                 key=lambda member: member.name):
                if IsResultsFilename(member.name):
                    yield json.load(tar.extractfile(member))
    else:
        # Read from directory.
        for name in sorted(os.listdir(filename)):
            if IsResultsFilename(name):
                yield ReadJsonFile(os.path.join(filename, name))


# Takes a list of filenames of perf test results, each representing the
# results from one boot of Fuchsia, and each in the format accepted by
# RawResultsFromDir().
#
# Returns a dict mapping test names to Stats objects.
def ResultsFromDirs(filenames):
    results_map = {}
    for boot_results_path in filenames:
        results_for_boot = {}
        for process_run_results in RawResultsFromDir(boot_results_path):
            for test_case in process_run_results:
                # Skip the running time from the test's initial run within
                # the process; treat it as a warmup run.  The initial run
                # is often slower than later runs, so it would skew the
                # mean if we included it.  The RoundTrip_*_MultiProcess
                # tests are an extreme case, because the first run waits
                # for a subprocess to start up.  See PT-244.
                new_value = Mean(test_case['values'][1:])
                results_for_boot.setdefault(test_case['label'], []).append(
                    new_value)
        for label, values in results_for_boot.iteritems():
            results_map.setdefault(label, []).append(Mean(values))
    return {name: Stats(values) for name, values in results_map.iteritems()}


# This takes a directory representing perf test results from multiple boots
# of Fuchsia.  It contains a "by_boot" subdir, which contains directories
# (or tar files) of the format read by RawResultsFromDir().
#
# This returns a dict mapping test names to Stats objects.
def ResultsFromDir(filename):
    assert os.path.exists(filename)
    by_boot_dir = os.path.join(filename, 'by_boot')
    assert os.path.exists(by_boot_dir), by_boot_dir
    filenames = [os.path.join(by_boot_dir, name)
                 for name in sorted(os.listdir(by_boot_dir))]
    return ResultsFromDirs(filenames)


def FormatTable(heading_row, rows, out_fh):
    column_count = len(heading_row)
    for row in rows:
        assert len(row) == column_count
    rows = [heading_row] + rows
    widths = [2 + max(len(row[col_number]) for row in rows)
              for col_number in xrange(column_count)]
    # Underline the heading row.
    rows.insert(1, ['-' * (width - 2) for width in widths])
    for row in rows:
        for col_number, value in enumerate(row):
            out_fh.write(value)
            if col_number < column_count - 1:
                out_fh.write(' ' * (widths[col_number] - len(value)))
        out_fh.write('\n')


def ComparePerf(args, out_fh):
    results_maps = [ResultsFromDir(args.results_dir_before),
                    ResultsFromDir(args.results_dir_after)]

    # Set of all test case names, including those added or removed.
    labels = set(results_maps[0].iterkeys())
    labels.update(results_maps[1].iterkeys())

    counts = {
        'added': 0,
        'removed': 0,
        'faster': 0,
        'slower': 0,
        'no_sig_diff': 0,
    }
    heading_row = ['Test case', 'Improve/regress?', 'Factor change',
                   'Mean before', 'Mean after']
    all_rows = []
    diff_rows = []
    for label in sorted(labels):
        if label not in results_maps[0]:
            result = 'added'
            factor_range = '-'
            before_range = '-'
            after_range = results_maps[1][label].FormatConfidenceInterval()
        elif label not in results_maps[1]:
            result = 'removed'
            factor_range = '-'
            before_range = results_maps[0][label].FormatConfidenceInterval()
            after_range = '-'
        else:
            stats = [results_map[label] for results_map in results_maps]
            interval_before = stats[0].interval
            interval_after = stats[1].interval
            factor_min = interval_after[0] / interval_before[1]
            factor_max = interval_after[1] / interval_before[0]
            if interval_after[0] >= interval_before[1]:
                result = 'slower'
            elif interval_after[1] <= interval_before[0]:
                result = 'faster'
            else:
                result = 'no_sig_diff'
            before_range = stats[0].FormatConfidenceInterval()
            after_range = stats[1].FormatConfidenceInterval()
            factor_range = '%.3f-%.3f' % (factor_min, factor_max)
        counts[result] += 1
        row = [label, result, factor_range, before_range, after_range]
        all_rows.append(row)
        if result != 'no_sig_diff':
            diff_rows.append(row)

    def FormatCount(count, text):
        noun = 'test case' if count == 1 else 'test cases'
        out_fh.write('  %d %s %s\n' % (count, noun, text))

    out_fh.write('Summary counts:\n')
    FormatCount(len(labels), 'in total')
    FormatCount(counts['no_sig_diff'],
                'had no significant difference (no_sig_diff)')
    FormatCount(counts['faster'], 'got faster')
    FormatCount(counts['slower'], 'got slower')
    FormatCount(counts['added'], 'added')
    FormatCount(counts['removed'], 'removed')
    out_fh.write('\n\n')

    if len(diff_rows) != 0:
        out_fh.write('Results from test cases with differences:\n\n')
        FormatTable(heading_row, diff_rows, out_fh)
        out_fh.write('\n\n')

    out_fh.write('Results from all test cases:\n\n')
    FormatTable(heading_row, all_rows, out_fh)


def IntervalsIntersect(interval1, interval2):
    return not (interval2[0] >= interval1[1] or
                interval2[1] <= interval1[0])


# Calculate the rate at which two intervals drawn (without replacement)
# from the given set of intervals will be non-intersecting.
def MismatchRate(intervals):
    mismatch_count = sum(int(not IntervalsIntersect(intervals[i], intervals[j]))
                         for i in xrange(len(intervals))
                         for j in xrange(i))
    comparisons_count = len(intervals) * (len(intervals) - 1) / 2
    return float(mismatch_count) / comparisons_count


def ValidatePerfCompare(args, out_fh):
    boot_count = len(args.results_dirs)
    group_size = args.group_size
    group_count = boot_count / group_size

    results_maps = [
        ResultsFromDirs(
            args.results_dirs[i * group_size : (i + 1) * group_size])
        for i in xrange(group_count)]

    # Group by test name (label).
    by_test = {}
    for results_map in results_maps:
        for label, stats in results_map.iteritems():
            by_test.setdefault(label, []).append(stats)

    out_fh.write('Rate of mismatches (non-intersections) '
                 'of confidence intervals for each test:\n')
    mismatch_rates = []
    for label, stats_list in sorted(by_test.iteritems()):
        mismatch_rate = MismatchRate([stats.interval for stats in stats_list])
        out_fh.write('%f %s\n' % (mismatch_rate, label))
        mismatch_rates.append(mismatch_rate)

    mean_relative_ci_width = Mean([
        stats.RelativeConfidenceIntervalWidth()
        for results_map in results_maps
        for stats in results_map.itervalues()])

    out_fh.write('\n')
    mean_val = Mean(mismatch_rates)
    out_fh.write('Mean mismatch rate: %f\n' % mean_val)
    out_fh.write('Mean relative confidence interval width: %f\n'
                 % mean_relative_ci_width)
    out_fh.write('Number of test cases: %d\n' % len(mismatch_rates))
    out_fh.write('Number of result sets: %d groups of %d boots each'
                 ' (ignoring %d leftover boots)\n'
                 % (group_count, group_size,
                    boot_count - group_size * group_count))
    out_fh.write('Expected number of test cases with mismatches: %f\n'
                 % (mean_val * len(mismatch_rates)))


def TotalSize(snapshot_file):
    with open(snapshot_file) as fh:
        data = json.load(fh)
    return sum(info['size'] for info in data['blobs'].itervalues())


def CompareSizes(args):
    filenames = [args.snapshot_before, args.snapshot_after]
    sizes = [TotalSize(filename) for filename in filenames]
    print 'Size before:  %d bytes' % sizes[0]
    print 'Size after:   %d bytes' % sizes[1]
    print 'Difference:   %d bytes' % (sizes[1] - sizes[0])
    if sizes[0] != 0:
        print 'Factor of:    %f' % (float(sizes[1]) / sizes[0])


def Main(argv, out_fh):
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers()

    parser_compare_perf = subparsers.add_parser(
        'compare_perf',
        help='Compare two sets of perf test results')
    parser_compare_perf.add_argument('results_dir_before')
    parser_compare_perf.add_argument('results_dir_after')
    parser_compare_perf.set_defaults(
        func=lambda args: ComparePerf(args, out_fh))

    parser_validate_perfcompare = subparsers.add_parser(
        'validate_perfcompare',
        help='Outputs statistics given multiple sets of perf test results'
        ' that come from the same build.  This is for validating the'
        ' statistics used by the perfcompare tool.  It can be used to check'
        ' the rate at which the tool will falsely indicate that performance'
        ' of a test case has regressed or improved.')
    parser_validate_perfcompare.add_argument(
        '-g', '--group_size', type=int, required=True,
        help='Number of boots to put in each group.  To get realistic'
        ' results that reflect how the perfcompare trybots would behave,'
        ' this should match the boots_per_revision setting in the'
        ' fuchsia_perfcompare.py recipe.  (Since that code is currently'
        ' not part of the Fuchsia checkout, we cannot make the settings'
        ' match automatically.)')
    parser_validate_perfcompare.add_argument('results_dirs', nargs='+')
    parser_validate_perfcompare.set_defaults(
        func=lambda args: ValidatePerfCompare(args, out_fh))

    parser_compare_sizes = subparsers.add_parser(
        'compare_sizes',
        help='Compare file sizes specified by two system.snapshot files')
    parser_compare_sizes.add_argument('snapshot_before')
    parser_compare_sizes.add_argument('snapshot_after')
    parser_compare_sizes.set_defaults(func=CompareSizes)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == '__main__':
    Main(sys.argv[1:], sys.stdout)
