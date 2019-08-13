// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "managed_environment.h"

#include <random>

#include <fuchsia/logger/cpp/fidl.h>
#include <sdk/lib/sys/cpp/termination_reason.h>
#include <src/lib/fxl/strings/string_printf.h>

namespace netemul {

// Start the Log and LogSink service (the same component publishses both
// services))
static const char* kLogSinkServiceURL = "fuchsia-pkg://fuchsia.com/logger#meta/logger.cmx";
static const char* kLogServiceURL = "fuchsia-pkg://fuchsia.com/logger#meta/logger.cmx";
static const char* kLogServiceNoKLogOption = "--disable-klog";

using sys::testing::EnclosingEnvironment;
using sys::testing::EnvironmentServices;

ManagedEnvironment::Ptr ManagedEnvironment::CreateRoot(const fuchsia::sys::EnvironmentPtr& parent,
                                                       const SandboxEnv::Ptr& sandbox_env,
                                                       Options options) {
  auto ret = ManagedEnvironment::Ptr(new ManagedEnvironment(sandbox_env));
  ret->Create(parent, std::move(options));
  return ret;
}

ManagedEnvironment::ManagedEnvironment(const SandboxEnv::Ptr& sandbox_env)
    : sandbox_env_(sandbox_env), ready_(false) {}

sys::testing::EnclosingEnvironment& ManagedEnvironment::environment() { return *env_; }

void ManagedEnvironment::GetLauncher(::fidl::InterfaceRequest<::fuchsia::sys::Launcher> launcher) {
  launcher_->Bind(std::move(launcher));
}

void ManagedEnvironment::CreateChildEnvironment(fidl::InterfaceRequest<FManagedEnvironment> me,
                                                Options options) {
  ManagedEnvironment::Ptr np(new ManagedEnvironment(sandbox_env_));
  fuchsia::sys::EnvironmentPtr env;
  env_->ConnectToService(env.NewRequest());
  np->Create(env, std::move(options), this);
  np->bindings_.AddBinding(np.get(), std::move(me));

  children_.emplace_back(std::move(np));
}

void ManagedEnvironment::Create(const fuchsia::sys::EnvironmentPtr& parent,
                                ManagedEnvironment::Options options,
                                const ManagedEnvironment* managed_parent) {
  // Nested environments without a name are not allowed, if empty name is
  // provided, replace it with a default *randomized* value.
  // Randomness there is necessary due to appmgr rules for environments with
  // same name.
  if (!options.has_name() || options.name().empty()) {
    std::random_device rnd;
    options.set_name(fxl::StringPrintf("netemul-env-%08x", rnd()));
  }

  // Start LogListener for this environment
  log_listener_ =
      LogListener::Create(std::move(*options.mutable_logger_options()), options.name(), NULL);

  auto services = EnvironmentServices::Create(parent);

  services->SetServiceTerminatedCallback([this, name = options.name()](
                                             const std::string& service, int64_t exit_code,
                                             fuchsia::sys::TerminationReason reason) {
    FXL_LOG(WARNING) << "Service " << service << " exited on environment " << name << " with ("
                     << exit_code << ") reason: " << sys::HumanReadableTerminationReason(reason);
    if (sandbox_env_->events().service_terminated) {
      sandbox_env_->events().service_terminated(service, exit_code, reason);
    }
  });

  if (log_listener_) {
    loggers_ = std::make_unique<ManagedLoggerCollection>(options.name(),
                                                         log_listener_->GetLogListenerImpl());
  } else {
    loggers_ = std::make_unique<ManagedLoggerCollection>(options.name(), nullptr);
  }

  // add network context service:
  services->AddService(sandbox_env_->network_context().GetHandler());

  // add Bus service:
  services->AddService(sandbox_env_->sync_manager().GetHandler());

  // add managed environment itself as a handler
  services->AddService(bindings_.GetHandler(this));

  bool disable_klog = !LogListener::IsKlogsEnabled(options);

  // Inject LogSink service
  services->AddServiceWithLaunchInfo(
      kLogSinkServiceURL,
      [this, disable_klog]() {
        fuchsia::sys::LaunchInfo linfo;
        linfo.url = kLogSinkServiceURL;
        if (disable_klog) {
          linfo.arguments.emplace({kLogServiceNoKLogOption});
        }
        linfo.out = loggers_->CreateLogger(kLogSinkServiceURL, false);
        linfo.err = loggers_->CreateLogger(kLogSinkServiceURL, true);
        loggers_->IncrementCounter();
        return linfo;
      },
      fuchsia::logger::LogSink::Name_);

  // Inject Log service
  services->AddServiceWithLaunchInfo(
      kLogServiceURL,
      [this, disable_klog]() {
        fuchsia::sys::LaunchInfo linfo;
        linfo.url = kLogServiceURL;
        if (disable_klog) {
          linfo.arguments.emplace({kLogServiceNoKLogOption});
        }
        linfo.out = loggers_->CreateLogger(kLogServiceURL, false);
        linfo.err = loggers_->CreateLogger(kLogServiceURL, true);
        loggers_->IncrementCounter();
        return linfo;
      },
      fuchsia::logger::Log::Name_);

  // prepare service configurations:
  service_config_.clear();
  if (options.has_inherit_parent_launch_services() && options.inherit_parent_launch_services() &&
      managed_parent != nullptr) {
    for (const auto& a : managed_parent->service_config_) {
      LaunchService clone;
      a.Clone(&clone);
      service_config_.push_back(std::move(clone));
    }
  }

  if (options.has_services()) {
    std::move(options.mutable_services()->begin(), options.mutable_services()->end(),
              std::back_inserter(service_config_));
  }

  // push all the allowable launch services:
  for (const auto& svc : service_config_) {
    LaunchService copy;
    ZX_ASSERT(svc.Clone(&copy) == ZX_OK);
    services->AddServiceWithLaunchInfo(
        svc.url,
        [this, svc = std::move(copy)]() {
          fuchsia::sys::LaunchInfo linfo;
          linfo.url = svc.url;
          linfo.arguments->insert(linfo.arguments->begin(), svc.arguments->begin(),
                                  svc.arguments->end());

          if (!launcher_->MakeServiceLaunchInfo(&linfo)) {
            // NOTE: we can just log an return code of MakeServiceLaunchInfo here, since those are
            // caused by fuchsia::sys::Loader errors that will happen again once we return the
            // launch info. That failure, in turn, will be caught by the service termination
            // callback installed in the services instance.
            FXL_LOG(ERROR) << "Make service launch info failed";
          }
          return linfo;
        },
        svc.name);
  }

  if (options.has_devices()) {
    // save all handles for virtual devices
    for (auto& dev : *options.mutable_devices()) {
      virtual_devices_.AddEntry(dev.path, dev.device.Bind());
    }
  }

  fuchsia::sys::EnvironmentOptions sub_options = {
      .kill_on_oom = true, .allow_parent_runners = false, .inherit_parent_services = false};

  env_ = EnclosingEnvironment::Create(options.name(), parent, std::move(services), sub_options);

  env_->SetRunningChangedCallback([this](bool running) {
    ready_ = true;
    if (running) {
      for (auto& r : pending_requests_) {
        Bind(std::move(r));
      }
      pending_requests_.clear();
      if (running_callback_) {
        running_callback_();
      }
    } else {
      FXL_LOG(ERROR) << "Underlying enclosed Environment stopped running";
      running_callback_ = nullptr;
      children_.clear();
      pending_requests_.clear();
      env_ = nullptr;
      launcher_ = nullptr;
      bindings_.CloseAll();
    }
  });

  launcher_ = std::make_unique<ManagedLauncher>(this);

  // If we have one, bind our log listener to this environment.
  // We do this after creation of log listener because
  // we need to make sure the environment is created first,
  // but managed logger needs our implementation of LogListenerImpl.
  if (log_listener_) {
    ZX_ASSERT(log_listener_->Bindable());
    log_listener_->BindToLogService(this);
  }
}

zx::channel ManagedEnvironment::OpenVdevDirectory() { return virtual_devices_.OpenAsDirectory(); }

zx::channel ManagedEnvironment::OpenVdataDirectory() {
  if (!virtual_data_) {
    virtual_data_ = std::make_unique<VirtualData>();
  }
  return virtual_data_->GetDirectory();
}

void ManagedEnvironment::Bind(fidl::InterfaceRequest<ManagedEnvironment::FManagedEnvironment> req) {
  if (ready_) {
    bindings_.AddBinding(this, std::move(req));
  } else if (env_) {
    pending_requests_.push_back(std::move(req));
  } else {
    req.Close(ZX_ERR_INTERNAL);
  }
}

ManagedLoggerCollection& ManagedEnvironment::loggers() {
  ZX_ASSERT(loggers_);
  return *loggers_;
}

void ManagedEnvironment::ConnectToService(std::string name, zx::channel req) {
  env_->ConnectToService(name, std::move(req));
}

void ManagedEnvironment::AddDevice(fuchsia::netemul::environment::VirtualDevice device) {
  virtual_devices_.AddEntry(device.path, device.device.Bind());
}

void ManagedEnvironment::RemoveDevice(::std::string path) { virtual_devices_.RemoveEntry(path); }

}  // namespace netemul
