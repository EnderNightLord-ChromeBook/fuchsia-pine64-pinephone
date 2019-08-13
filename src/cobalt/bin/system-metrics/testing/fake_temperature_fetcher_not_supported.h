// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_SYSTEM_METRICS_TESTING_FAKE_TEMPERATURE_FETCHER_NOT_SUPPORTED_H_
#define SRC_COBALT_BIN_SYSTEM_METRICS_TESTING_FAKE_TEMPERATURE_FETCHER_NOT_SUPPORTED_H_

#include <stdint.h>

#include "src/cobalt/bin/system-metrics/temperature_fetcher.h"

namespace cobalt {

class FakeTemperatureFetcherNotSupported : public cobalt::TemperatureFetcher {
 public:
  FakeTemperatureFetcherNotSupported();
  cobalt::TemperatureFetchStatus FetchTemperature(int32_t *temperature) override;
};

}  // namespace cobalt

#endif  // SRC_COBALT_BIN_SYSTEM_METRICS_TESTING_FAKE_TEMPERATURE_FETCHER_NOT_SUPPORTED_H_
