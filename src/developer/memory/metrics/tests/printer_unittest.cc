// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/memory/metrics/printer.h"

#include <gtest/gtest.h>
#include <src/lib/fxl/strings/split_string.h>
#include <src/lib/fxl/strings/string_view.h>

#include "src/developer/memory/metrics/capture.h"
#include "src/developer/memory/metrics/summary.h"
#include "src/developer/memory/metrics/tests/test_utils.h"

namespace memory {
namespace test {

using PrinterUnitTest = testing::Test;

void ConfirmLines(std::ostringstream& oss, std::vector<std::string> expected_lines) {
  SCOPED_TRACE("");
  auto lines = fxl::SplitStringCopy(oss.str(), "\n", fxl::kKeepWhitespace, fxl::kSplitWantNonEmpty);
  ASSERT_EQ(expected_lines.size(), lines.size());
  for (size_t li = 0; li < expected_lines.size(); li++) {
    SCOPED_TRACE(li);
    std::string expected_line = expected_lines.at(li);
    fxl::StringView line = lines.at(li);
    EXPECT_STREQ(expected_line.c_str(), line.data());
  }
}

TEST_F(PrinterUnitTest, PrintCaptureKMEM) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234,
                                  .kmem = {.total_bytes = 300,
                                           .free_bytes = 100,
                                           .wired_bytes = 10,
                                           .total_heap_bytes = 20,
                                           .free_heap_bytes = 30,
                                           .vmo_bytes = 40,
                                           .mmu_overhead_bytes = 50,
                                           .ipc_bytes = 60,
                                           .other_bytes = 70},
                                  .vmos =
                                      {
                                          {.koid = 1, .name = "v1", .committed_bytes = 100},
                                      },
                                  .processes =
                                      {
                                          {.koid = 100, .name = "p1", .vmos = {1}},
                                      },
                              });
  std::ostringstream oss;
  Printer p(oss);

  p.PrintCapture(c, KMEM, SORTED);
  ConfirmLines(oss, {"K,1234,300,100,10,20,30,40,50,60,70"});
}

TEST_F(PrinterUnitTest, PrintCapturePROCESS) {
  Capture c;
  TestUtils::CreateCapture(
      c, {
             .time = 1234,
             .kmem = {.total_bytes = 300,
                      .free_bytes = 100,
                      .wired_bytes = 10,
                      .total_heap_bytes = 20,
                      .free_heap_bytes = 30,
                      .vmo_bytes = 40,
                      .mmu_overhead_bytes = 50,
                      .ipc_bytes = 60,
                      .other_bytes = 70},
             .vmos =
                 {
                     {.koid = 1, .name = "v1", .committed_bytes = 100},
                 },
             .processes =
                 {
                     {.koid = 100, .name = "p1", .vmos = {1}, .stats = {10, 20, 30, 40}},
                 },
         });
  std::ostringstream oss;
  Printer p(oss);

  p.PrintCapture(c, PROCESS, SORTED);
  ConfirmLines(oss, {"K,1234,300,100,10,20,30,40,50,60,70", "P,100,p1,10,20,30,40,1"});
}

TEST_F(PrinterUnitTest, PrintCaptureVMO) {
  Capture c;
  TestUtils::CreateCapture(
      c, {
             .time = 1234,
             .kmem =
                 {
                     .total_bytes = 300,
                     .free_bytes = 100,
                     .wired_bytes = 10,
                     .total_heap_bytes = 20,
                     .free_heap_bytes = 30,
                     .vmo_bytes = 40,
                     .mmu_overhead_bytes = 50,
                     .ipc_bytes = 60,
                     .other_bytes = 70,
                 },
             .vmos =
                 {
                     {
                         .koid = 1,
                         .name = "v1",
                         .size_bytes = 100,
                         .parent_koid = 200,
                         .committed_bytes = 300,
                     },
                 },
             .processes =
                 {
                     {.koid = 100, .name = "p1", .vmos = {1}, .stats = {10, 20, 30, 40}},
                 },
         });
  std::ostringstream oss;
  Printer p(oss);

  p.PrintCapture(c, VMO, SORTED);
  ConfirmLines(oss, {
                        "K,1234,300,100,10,20,30,40,50,60,70",
                        "P,100,p1,10,20,30,40,1",
                        "V,1,v1,100,200,300",
                    });
}

TEST_F(PrinterUnitTest, PrintSummaryKMEM) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234,
                                  .kmem =
                                      {
                                          .total_bytes = 1024 * 1024,
                                          .free_bytes = 1024,
                                          .wired_bytes = 2 * 1024,
                                          .total_heap_bytes = 3 * 1024,
                                          .free_heap_bytes = 2 * 1024,
                                          .vmo_bytes = 5 * 1024,
                                          .mmu_overhead_bytes = 6 * 1024,
                                          .ipc_bytes = 7 * 1024,
                                          .other_bytes = 8 * 1024,
                                      },
                              });

  std::ostringstream oss;
  Printer p(oss);
  p.PrintSummary(c, KMEM, SORTED);

  ConfirmLines(oss, {
                        "Time: 1234 VMO: 5K Free: 1K",
                    });
}

TEST_F(PrinterUnitTest, PrintSummaryPROCESS) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234,
                                  .kmem =
                                      {
                                          .total_bytes = 1024 * 1024,
                                          .free_bytes = 1024,
                                          .wired_bytes = 2 * 1024,
                                          .total_heap_bytes = 3 * 1024,
                                          .free_heap_bytes = 2 * 1024,
                                          .vmo_bytes = 5 * 1024,
                                          .mmu_overhead_bytes = 6 * 1024,
                                          .ipc_bytes = 7 * 1024,
                                          .other_bytes = 8 * 1024,
                                      },
                                  .vmos = {{.koid = 1, .name = "v1", .committed_bytes = 1024}},
                                  .processes = {{.koid = 100, .name = "p1", .vmos = {1}}},
                              });

  std::ostringstream oss;
  Printer p(oss);
  p.PrintSummary(c, PROCESS, SORTED);

  ConfirmLines(oss, {
                        "Time: 1234 VMO: 5K Free: 1K",
                        "kernel<1> 30K",
                        "p1<100> 1K",
                    });
}

TEST_F(PrinterUnitTest, PrintSummaryVMO) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234,
                                  .kmem =
                                      {
                                          .total_bytes = 1024 * 1024,
                                          .free_bytes = 1024,
                                          .wired_bytes = 2 * 1024,
                                          .total_heap_bytes = 3 * 1024,
                                          .free_heap_bytes = 2 * 1024,
                                          .vmo_bytes = 5 * 1024,
                                          .mmu_overhead_bytes = 6 * 1024,
                                          .ipc_bytes = 7 * 1024,
                                          .other_bytes = 8 * 1024,
                                      },
                                  .vmos = {{.koid = 1, .name = "v1", .committed_bytes = 1024}},
                                  .processes = {{.koid = 100, .name = "p1", .vmos = {1}}},
                              });

  std::ostringstream oss;
  Printer p(oss);
  p.PrintSummary(c, VMO, SORTED);

  ConfirmLines(oss, {
                        "Time: 1234 VMO: 5K Free: 1K",
                        "kernel<1> 30K",
                        " other 8K",
                        " ipc 7K",
                        " mmu 6K",
                        " vmo 4K",
                        " heap 3K",
                        " wired 2K",
                        "p1<100> 1K",
                        " v1 1K",
                    });
}

TEST_F(PrinterUnitTest, PrintSummaryVMOShared) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234,
                                  .kmem = {.vmo_bytes = 6 * 1024},
                                  .vmos =
                                      {
                                          {.koid = 1, .name = "v1", .committed_bytes = 1024},
                                          {.koid = 2, .name = "v2", .committed_bytes = 2 * 1024},
                                          {.koid = 3, .name = "v3", .committed_bytes = 3 * 1024},
                                      },
                                  .processes =
                                      {
                                          {.koid = 100, .name = "p1", .vmos = {1, 2}},
                                          {.koid = 200, .name = "p2", .vmos = {2, 3}},
                                      },
                              });

  std::ostringstream oss;
  Printer p(oss);
  p.PrintSummary(c, VMO, SORTED);

  ConfirmLines(oss, {
                        "Time: 1234 VMO: 6K Free: 0B",
                        "p2<200> 3K 4K 5K",
                        " v3 3K",
                        " v2 0B 1K 2K",
                        "p1<100> 1K 2K 3K",
                        " v1 1K",
                        " v2 0B 1K 2K",
                        "kernel<1> 0B",
                    });
}

TEST_F(PrinterUnitTest, OutputSummarySingle) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234L * 1000000000L,
                                  .vmos =
                                      {
                                          {.koid = 1, .name = "v1", .committed_bytes = 100},
                                      },
                                  .processes =
                                      {
                                          {.koid = 100, .name = "p1", .vmos = {1}},
                                      },
                              });
  Summary s(c);

  std::ostringstream oss;
  Printer p(oss);

  p.OutputSummary(s, SORTED, ZX_KOID_INVALID);
  ConfirmLines(oss, {
                        "1234,100,p1,100,100,100",
                        "1234,1,kernel,0,0,0",
                    });

  oss.str("");
  p.OutputSummary(s, SORTED, 100);
  ConfirmLines(oss, {
                        "1234,100,v1,100,100,100",
                    });
}

TEST_F(PrinterUnitTest, OutputSummaryKernel) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234L * 1000000000L,
                                  .kmem =
                                      {
                                          .wired_bytes = 10,
                                          .total_heap_bytes = 20,
                                          .mmu_overhead_bytes = 30,
                                          .ipc_bytes = 40,
                                          .other_bytes = 50,
                                          .vmo_bytes = 60,
                                      },
                              });
  Summary s(c);

  std::ostringstream oss;
  Printer p(oss);

  p.OutputSummary(s, SORTED, ZX_KOID_INVALID);
  ConfirmLines(oss, {
                        "1234,1,kernel,210,210,210",
                    });

  oss.str("");
  p.OutputSummary(s, SORTED, ProcessSummary::kKernelKoid);
  ConfirmLines(oss, {
                        "1234,1,vmo,60,60,60",
                        "1234,1,other,50,50,50",
                        "1234,1,ipc,40,40,40",
                        "1234,1,mmu,30,30,30",
                        "1234,1,heap,20,20,20",
                        "1234,1,wired,10,10,10",
                    });
}

TEST_F(PrinterUnitTest, OutputSummaryDouble) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234L * 1000000000L,
                                  .vmos =
                                      {
                                          {.koid = 1, .name = "v1", .committed_bytes = 100},
                                          {.koid = 2, .name = "v2", .committed_bytes = 200},
                                      },
                                  .processes =
                                      {
                                          {.koid = 100, .name = "p1", .vmos = {1}},
                                          {.koid = 200, .name = "p2", .vmos = {2}},
                                      },
                              });
  Summary s(c);

  std::ostringstream oss;
  Printer p(oss);

  p.OutputSummary(s, SORTED, ZX_KOID_INVALID);
  ConfirmLines(oss, {
                        "1234,200,p2,200,200,200",
                        "1234,100,p1,100,100,100",
                        "1234,1,kernel,0,0,0",
                    });

  oss.str("");
  p.OutputSummary(s, SORTED, 100);
  ConfirmLines(oss, {
                        "1234,100,v1,100,100,100",
                    });

  oss.str("");
  p.OutputSummary(s, SORTED, 200);
  ConfirmLines(oss, {
                        "1234,200,v2,200,200,200",
                    });
}

TEST_F(PrinterUnitTest, OutputSummaryShared) {
  Capture c;
  TestUtils::CreateCapture(c, {
                                  .time = 1234L * 1000000000L,
                                  .vmos =
                                      {
                                          {.koid = 1, .name = "v1", .committed_bytes = 100},
                                          {.koid = 2, .name = "v1", .committed_bytes = 100},
                                          {.koid = 3, .name = "v1", .committed_bytes = 100},
                                          {.koid = 4, .name = "v2", .committed_bytes = 100},
                                          {.koid = 5, .name = "v3", .committed_bytes = 200},
                                      },
                                  .processes =
                                      {
                                          {.koid = 100, .name = "p1", .vmos = {1, 2, 4}},
                                          {.koid = 200, .name = "p2", .vmos = {2, 3, 5}},
                                      },
                              });
  Summary s(c);

  std::ostringstream oss;
  Printer p(oss);

  p.OutputSummary(s, SORTED, ZX_KOID_INVALID);
  ConfirmLines(oss, {
                        "1234,200,p2,300,350,400",
                        "1234,100,p1,200,250,300",
                        "1234,1,kernel,0,0,0",
                    });

  oss.str("");
  p.OutputSummary(s, SORTED, 100);
  ConfirmLines(oss, {
                        "1234,100,v1,100,150,200",
                        "1234,100,v2,100,100,100",
                    });

  oss.str("");
  p.OutputSummary(s, SORTED, 200);
  ConfirmLines(oss, {
                        "1234,200,v3,200,200,200",
                        "1234,200,v1,100,150,200",
                    });
}

TEST_F(PrinterUnitTest, FormatSize) {
  struct TestCase {
    uint64_t bytes;
    const char* val;
  };
  std::vector<TestCase> tests = {
      {0, "0B"},
      {1, "1B"},
      {1023, "1023B"},
      {1024, "1K"},
      {1025, "1K"},
      {1029, "1K"},
      {1124, "1.1K"},
      {1536, "1.5K"},
      {2047, "2K"},
      {1024 * 1024, "1M"},
      {1024 * 1024 * 1024, "1G"},
      {1024UL * 1024 * 1024 * 1024, "1T"},
      {1024UL * 1024 * 1024 * 1024 * 1024, "1P"},
      {1024UL * 1024 * 1024 * 1024 * 1024 * 1024, "1E"},
      {1024UL * 1024 * 1024 * 1024 * 1024 * 1024 * 1024, "0B"},
  };
  for (auto const& test : tests) {
    EXPECT_STREQ(test.val, FormatSize(test.bytes).c_str());
  }
}

}  // namespace test
}  // namespace memory
