// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ledger/cpp/fidl.h>
#include <fuchsia/ledger/internal/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fsl/io/fd.h>
#include <lib/svc/cpp/services.h>
#include <lib/sys/cpp/component_context.h>

#include <cstdlib>

#include "peridot/lib/convert/convert.h"
#include "peridot/lib/scoped_tmpfs/scoped_tmpfs.h"
#include "src/lib/fxl/strings/string_view.h"

namespace {

constexpr char kLedgerBinaryPath[] = "fuchsia-pkg://fuchsia.com/ledger#meta/ledger.cmx";
constexpr fxl::StringView kLedgerName = "test ledger instance";

}  // namespace

// Exposes a public service that serves an in-memory Ledger.
int main(int argc, char const *argv[]) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  std::unique_ptr<sys::ComponentContext> context(sys::ComponentContext::Create());

  // Get a repository factory.
  component::Services services;
  fuchsia::sys::LaunchInfo launch_info;
  launch_info.url = kLedgerBinaryPath;
  launch_info.directory_request = services.NewRequest();
  launch_info.arguments.push_back("--disable_reporting");
  fuchsia::sys::ComponentControllerPtr controller;
  fuchsia::sys::LauncherPtr launcher;
  context->svc()->Connect(launcher.NewRequest());
  launcher->CreateComponent(std::move(launch_info), controller.NewRequest());
  fuchsia::ledger::internal::LedgerRepositoryFactoryPtr repository_factory;
  services.ConnectToService(repository_factory.NewRequest());

  // Create memfs.
  auto memfs = std::make_unique<scoped_tmpfs::ScopedTmpFS>();
  zx::channel memfs_channel = fsl::CloneChannelFromFileDescriptor(memfs->root_fd());

  // Get a repository.
  fuchsia::ledger::internal::LedgerRepositorySyncPtr repository;
  repository_factory->GetRepository(std::move(memfs_channel), nullptr, "", repository.NewRequest());

  // Serve the repository.
  context->outgoing()->AddPublicService<fuchsia::ledger::Ledger>(
      [&repository](fidl::InterfaceRequest<fuchsia::ledger::Ledger> request) {
        repository->GetLedger(convert::ToArray(kLedgerName), std::move(request));
      });
  loop.Run();
  return EXIT_SUCCESS;
}
