// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/bin/a11y_manager/settings/settings_provider_impl.h"

#include <lib/syslog/cpp/logger.h>
#include <src/lib/fxl/logging.h>

#include "src/ui/a11y/bin/a11y_manager/util.h"

namespace a11y_manager {

// clang-format off
const std::array<float, 9> kIdentityMatrix = {
    1, 0, 0,
    0, 1, 0,
    0, 0, 1};

// To read more information about following Matrix, please refer to Jira ticket
// MI4-2420 to get a link to document which explains them in more detail.
const std::array<float, 9> kColorInversionMatrix = {
    0.402,  -0.598, -0.599,
    -1.174, -0.174, -1.175,
    -0.228, -0.228, 0.772};
const std::array<float, 9> kCorrectProtanomaly = {
    0.622774, 0.264275,  0.216821,
    0.377226, 0.735725,  -0.216821,
    0.000000, -0.000000, 1.000000};
const std::array<float, 9> kCorrectDeuteranomaly = {
    0.288299f, 0.052709f,  -0.257912f,
    0.711701f, 0.947291f,  0.257912f,
    0.000000f, -0.000000f, 1.000000f};
const std::array<float, 9> kCorrectTritanomaly = {
    1.000000f,  0.000000f, -0.000000f,
    -0.805712f, 0.378838f, 0.104823f,
    0.805712f,  0.621162f, 0.895177f};
// clang-format on

SettingsProviderImpl::SettingsProviderImpl() : binding_(this), settings_() {
  settings_.set_magnification_enabled(false);
  settings_.set_magnification_zoom_factor(1.0);
  settings_.set_screen_reader_enabled(false);
  settings_.set_color_inversion_enabled(false);
  settings_.set_color_correction(fuchsia::accessibility::ColorCorrection::DISABLED);
  settings_.set_color_adjustment_matrix(kIdentityMatrix);
}

void SettingsProviderImpl::Bind(
    fidl::InterfaceRequest<fuchsia::accessibility::SettingsProvider> settings_provider_request) {
  binding_.Close(ZX_ERR_PEER_CLOSED);
  binding_.Bind(std::move(settings_provider_request));
}

std::string SettingsProviderImpl::BoolToString(bool value) { return value ? "true" : "false"; }

void SettingsProviderImpl::SetMagnificationEnabled(bool magnification_enabled,
                                                   SetMagnificationEnabledCallback callback) {
  // Attempting to enable magnification when it's already enabled OR disable
  // magnification when it's already disabled has no effect.
  if (settings_.has_magnification_enabled() &&
      settings_.magnification_enabled() == magnification_enabled) {
    callback(fuchsia::accessibility::SettingsManagerStatus::OK);
    return;
  }

  if (magnification_enabled) {
    // Case: enable magnification (currently disabled).
    settings_.set_magnification_enabled(true);
  } else {
    // Case: disable magnification (currently enabled).
    settings_.set_magnification_enabled(false);
  }

  // In either case, set zoom factor to default value of 1.0.
  settings_.set_magnification_zoom_factor(1.0);

  NotifyWatchers(settings_);

  FX_LOGS(INFO) << "magnification_enabled = " << BoolToString(settings_.magnification_enabled());

  callback(fuchsia::accessibility::SettingsManagerStatus::OK);
}

void SettingsProviderImpl::SetMagnificationZoomFactor(float magnification_zoom_factor,
                                                      SetMagnificationZoomFactorCallback callback) {
  if (!settings_.has_magnification_enabled() || !(settings_.magnification_enabled())) {
    callback(fuchsia::accessibility::SettingsManagerStatus::ERROR);
    return;
  }

  if (magnification_zoom_factor < 1.0) {
    FX_LOGS(ERROR) << "Magnification zoom factor must be > 1.0.";

    callback(fuchsia::accessibility::SettingsManagerStatus::ERROR);
    return;
  }

  settings_.set_magnification_zoom_factor(magnification_zoom_factor);

  NotifyWatchers(settings_);

  FX_LOGS(INFO) << "magnification_zoom_factor = " << settings_.magnification_zoom_factor();

  callback(fuchsia::accessibility::SettingsManagerStatus::OK);
}

void SettingsProviderImpl::SetScreenReaderEnabled(bool screen_reader_enabled,
                                                  SetScreenReaderEnabledCallback callback) {
  settings_.set_screen_reader_enabled(screen_reader_enabled);

  NotifyWatchers(settings_);

  FX_LOGS(INFO) << "screen_reader_enabled = " << BoolToString(settings_.screen_reader_enabled());

  callback(fuchsia::accessibility::SettingsManagerStatus::OK);
}

void SettingsProviderImpl::SetColorInversionEnabled(bool color_inversion_enabled,
                                                    SetColorInversionEnabledCallback callback) {
  settings_.set_color_inversion_enabled(color_inversion_enabled);
  settings_.set_color_adjustment_matrix(GetColorAdjustmentMatrix());

  NotifyWatchers(settings_);

  FX_LOGS(INFO) << "color_inversion_enabled = "
                << BoolToString(settings_.color_inversion_enabled());

  callback(fuchsia::accessibility::SettingsManagerStatus::OK);
}

void SettingsProviderImpl::SetColorCorrection(
    fuchsia::accessibility::ColorCorrection color_correction, SetColorCorrectionCallback callback) {
  settings_.set_color_correction(color_correction);
  settings_.set_color_adjustment_matrix(GetColorAdjustmentMatrix());

  NotifyWatchers(settings_);

  callback(fuchsia::accessibility::SettingsManagerStatus::OK);
}

void SettingsProviderImpl::NotifyWatchers(const fuchsia::accessibility::Settings& new_settings) {
  for (auto& watcher : watchers_) {
    auto setting = fidl::Clone(new_settings);
    watcher->OnSettingsChange(std::move(setting));
  }
}

void SettingsProviderImpl::ReleaseWatcher(fuchsia::accessibility::SettingsWatcher* watcher) {
  auto predicate = [watcher](const auto& target) { return target.get() == watcher; };

  watchers_.erase(std::remove_if(watchers_.begin(), watchers_.end(), predicate));
}

void SettingsProviderImpl::AddWatcher(
    fidl::InterfaceHandle<fuchsia::accessibility::SettingsWatcher> watcher) {
  fuchsia::accessibility::SettingsWatcherPtr watcher_proxy = watcher.Bind();
  fuchsia::accessibility::SettingsWatcher* proxy_raw_ptr = watcher_proxy.get();

  watcher_proxy.set_error_handler(
      [this, proxy_raw_ptr](zx_status_t status) { ReleaseWatcher(proxy_raw_ptr); });
  // Send Current Settings to watcher, so that they have the initial copy of the
  // Settings.
  auto setting = fidl::Clone(settings_);
  watcher_proxy->OnSettingsChange(std::move(setting));
  watchers_.push_back(std::move(watcher_proxy));
}

std::array<float, 9> SettingsProviderImpl::GetColorAdjustmentMatrix() {
  std::array<float, 9> color_inversion_matrix = kIdentityMatrix;
  std::array<float, 9> color_correction_matrix = kIdentityMatrix;

  if (settings_.color_inversion_enabled()) {
    color_inversion_matrix = kColorInversionMatrix;
  }

  switch (settings_.color_correction()) {
    case fuchsia::accessibility::ColorCorrection::CORRECT_PROTANOMALY:
      color_correction_matrix = kCorrectProtanomaly;
      break;
    case fuchsia::accessibility::ColorCorrection::CORRECT_DEUTERANOMALY:
      color_correction_matrix = kCorrectDeuteranomaly;
      break;
    case fuchsia::accessibility::ColorCorrection::CORRECT_TRITANOMALY:
      color_correction_matrix = kCorrectTritanomaly;
      break;
    case fuchsia::accessibility::ColorCorrection::DISABLED:
      color_correction_matrix = kIdentityMatrix;
      break;
  }

  return Multiply3x3MatrixRowMajor(color_inversion_matrix, color_correction_matrix);
}

}  // namespace a11y_manager
