// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/tests/mocks/mock_settings_watcher.h"

#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/logger.h>
#include <src/lib/fxl/logging.h>
#include <zircon/status.h>

#include <cmath>

namespace accessibility_test {

MockSettingsWatcher::MockSettingsWatcher(sys::testing::ComponentContextProvider* context)
    : context_provider_(context) {
  context_provider_->context()->svc()->Connect(manager_.NewRequest());
  manager_.set_error_handler([](zx_status_t status) {
    FX_LOGS(ERROR) << "Cannot connect to SettingsManager with status:"
                   << zx_status_get_string(status);
  });
  fidl::InterfaceHandle<fuchsia::accessibility::SettingsWatcher> watcher_handle;
  bindings_.AddBinding(this, watcher_handle.NewRequest());
  manager_->Watch(std::move(watcher_handle));
}

void MockSettingsWatcher::OnSettingsChange(fuchsia::accessibility::Settings new_settings) {
  SaveSettings(std::move(new_settings));
}

void MockSettingsWatcher::SaveSettings(fuchsia::accessibility::Settings provided_settings) {
  settings_.set_magnification_enabled(provided_settings.magnification_enabled());
  if (provided_settings.has_magnification_zoom_factor()) {
    settings_.set_magnification_zoom_factor(provided_settings.magnification_zoom_factor());
  }
  settings_.set_screen_reader_enabled(provided_settings.screen_reader_enabled());
  settings_.set_color_inversion_enabled(provided_settings.color_inversion_enabled());
  settings_.set_color_correction(provided_settings.color_correction());
  if (provided_settings.has_color_adjustment_matrix()) {
    settings_.set_color_adjustment_matrix(provided_settings.color_adjustment_matrix());
  }
}

bool MockSettingsWatcher::CompareFloatArray(std::array<float, 9> first_array,
                                            std::array<float, 9> second_array) const {
  const float float_comparison_epsilon = 0.00001;
  for (int i = 0; i < 9; i++) {
    if ((std::fabs(first_array[i] - second_array[i]) > float_comparison_epsilon)) {
      return false;
    }
  }
  return true;
}

bool MockSettingsWatcher::IsSame(fuchsia::accessibility::SettingsPtr provided_settings) {
  return settings_.magnification_enabled() == provided_settings->magnification_enabled() &&
         settings_.magnification_zoom_factor() == provided_settings->magnification_zoom_factor() &&
         settings_.screen_reader_enabled() == provided_settings->screen_reader_enabled() &&
         settings_.color_inversion_enabled() == provided_settings->color_inversion_enabled() &&
         settings_.color_correction() == provided_settings->color_correction() &&
         settings_.has_color_adjustment_matrix() ==
             provided_settings->has_color_adjustment_matrix() &&
         (settings_.has_color_adjustment_matrix()
              ? CompareFloatArray(settings_.color_adjustment_matrix(),
                                  provided_settings->color_adjustment_matrix())
              : true);
}

}  // namespace accessibility_test
