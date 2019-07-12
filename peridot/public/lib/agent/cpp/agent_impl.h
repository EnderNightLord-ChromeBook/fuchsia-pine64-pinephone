// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_AGENT_CPP_AGENT_IMPL_H_
#define LIB_AGENT_CPP_AGENT_IMPL_H_

#include <fbl/ref_ptr.h>
#include <fs/pseudo-dir.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/outgoing_directory.h>
#include <src/lib/fxl/macros.h>

#include <memory>

namespace modular {

// Use this class to talk to the modular framework as an
// fuchsia::modular::Agent.
class AgentImpl : public fuchsia::modular::Agent {
 public:
  // Users of AgentImpl register a delegate to receive messages from the
  // framework.
  class Delegate {
   public:
    virtual void Connect(fidl::InterfaceRequest<fuchsia::sys::ServiceProvider>
                             outgoing_services) = 0;
    virtual void RunTask(const fidl::StringPtr& task_id,
                         fit::function<void()> done) = 0;
  };

  AgentImpl(const std::shared_ptr<sys::OutgoingDirectory>& outgoing_services,
            Delegate* delegate);

  AgentImpl(fbl::RefPtr<fs::PseudoDir> directory, Delegate* delegate);

 private:
  // |fuchsia::modular::Agent|
  void Connect(std::string requestor_url,
               fidl::InterfaceRequest<fuchsia::sys::ServiceProvider>
                   services_request) override;
  // |fuchsia::modular::Agent|
  void RunTask(std::string task_id, RunTaskCallback callback) override;

  Delegate* const delegate_;
  fidl::Binding<fuchsia::modular::Agent> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AgentImpl);
};

}  // namespace modular

#endif  // LIB_AGENT_CPP_AGENT_IMPL_H_
