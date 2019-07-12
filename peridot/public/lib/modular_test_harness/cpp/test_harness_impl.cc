// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/modular_test_harness/cpp/test_harness_impl.h"

#include <dirent.h>
#include <lib/fdio/directory.h>
#include <lib/fsl/io/fd.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/vfs/cpp/pseudo_dir.h>
#include <lib/vfs/cpp/pseudo_file.h>
#include <peridot/lib/modular_config/modular_config.h>
#include <peridot/lib/modular_config/modular_config_constants.h>
#include <peridot/lib/modular_config/modular_config_xdr.h>
#include <peridot/lib/util/pseudo_dir_utils.h>
#include <src/lib/files/path.h>
#include <src/lib/files/unique_fd.h>
#include <src/lib/fxl/logging.h>
#include <src/lib/fxl/strings/join_strings.h>
#include <src/lib/fxl/strings/split_string.h>
#include <src/lib/fxl/strings/substitute.h>
#include <zircon/status.h>

namespace modular::testing {
namespace {

constexpr char kBasemgrUrl[] =
    "fuchsia-pkg://fuchsia.com/basemgr#meta/basemgr.cmx";

// Defaut shell URLs which are used if not specified.
constexpr char kBaseShellDefaultUrl[] =
    "fuchsia-pkg://fuchsia.com/modular_test_harness#meta/test_base_shell.cmx";
constexpr char kSessionShellDefaultUrl[] =
    "fuchsia-pkg://fuchsia.com/modular_test_harness#meta/"
    "test_session_shell.cmx";
constexpr char kStoryShellDefaultUrl[] =
    "fuchsia-pkg://fuchsia.com/modular_test_harness#meta/test_story_shell.cmx";

constexpr char kSessionAgentFakeInterceptionUrl[] =
    "fuchsia-pkg://example.com/FAKE_SESSION_AGENT_PKG/fake_session_agent.cmx";
constexpr char kSessionAgentFakeInterceptionCmx[] = R"(
{
  "sandbox": {
    "services": [
      "fuchsia.modular.PuppetMaster",
      "fuchsia.modular.AgentContext",
      "fuchsia.modular.ComponentContext"
    ]
  }
}
)";

};  // namespace

class TestHarnessImpl::InterceptedComponentImpl
    : public fuchsia::modular::testing::InterceptedComponent {
 public:
  using RemoveHandler = fit::function<void()>;
  InterceptedComponentImpl(
      std::unique_ptr<sys::testing::InterceptedComponent> impl,
      fidl::InterfaceRequest<fuchsia::modular::testing::InterceptedComponent>
          request)
      : impl_(std::move(impl)), binding_(this, std::move(request)) {
    impl_->set_on_kill([this] {
      binding_.events().OnKill();
    });
  }

  virtual ~InterceptedComponentImpl() = default;

  void set_remove_handler(RemoveHandler remove_handler) {
    remove_handler_ = std::move(remove_handler);
  }

 private:
  // |fuchsia::modular::testing::InterceptedComponent|
  void Exit(int64_t exit_code, fuchsia::sys::TerminationReason reason) {
    impl_->Exit(exit_code, reason);
    remove_handler_();
  }

  std::unique_ptr<sys::testing::InterceptedComponent> impl_;
  fidl::Binding<fuchsia::modular::testing::InterceptedComponent> binding_;
  RemoveHandler remove_handler_;
};

// This class implements a session agent using AgentDriver.
class TestHarnessImpl::InterceptedSessionAgent final {
 public:
  InterceptedSessionAgent(::modular::AgentHost* host) {}

  // Called by AgentDriver.
  void Connect(
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> outgoing_services) {
  }

  // Called by AgentDriver.
  void RunTask(const fidl::StringPtr& task_id,
               const fit::function<void()>& done) {
    FXL_DLOG(WARNING) << "This session agent does not run tasks";
    done();
  }

  // Called by AgentDriver.
  void Terminate(const fit::function<void()>& done) { done(); }
};

TestHarnessImpl::TestHarnessImpl(
    const fuchsia::sys::EnvironmentPtr& parent_env,
    fidl::InterfaceRequest<fuchsia::modular::testing::TestHarness> request,
    fit::function<void()> on_disconnected)
    : parent_env_(parent_env),
      binding_(this, std::move(request)),
      on_disconnected_(std::move(on_disconnected)),
      interceptor_(
          sys::testing::ComponentInterceptor::CreateWithEnvironmentLoader(
              parent_env_)) {
  binding_.set_error_handler(
      [this](zx_status_t status) { CloseBindingIfError(status); });
}

TestHarnessImpl::~TestHarnessImpl() = default;

void TestHarnessImpl::ConnectToModularService(
    fuchsia::modular::testing::ModularService service) {
  switch (service.Which()) {
    case fuchsia::modular::testing::ModularService::Tag::kPuppetMaster: {
      BufferSessionAgentService(std::move(service.puppet_master()));
    } break;

    case fuchsia::modular::testing::ModularService::Tag::kComponentContext: {
      BufferSessionAgentService(std::move(service.component_context()));
    } break;

    case fuchsia::modular::testing::ModularService::Tag::kAgentContext: {
      BufferSessionAgentService(std::move(service.agent_context()));
    } break;

    case fuchsia::modular::testing::ModularService::Tag::Empty: {
      FXL_LOG(ERROR) << "The given ModularService is empty.";
      CloseBindingIfError(ZX_ERR_INVALID_ARGS);
      return;
    } break;
  }
}

void TestHarnessImpl::ConnectToEnvironmentService(std::string service_name,
                                                  zx::channel request) {
  enclosing_env_->ConnectToService(service_name, std::move(request));
}

bool TestHarnessImpl::CloseBindingIfError(zx_status_t status) {
  if (status != ZX_OK) {
    FXL_LOG(ERROR) << "Destroying TestHarness because of error: "
                   << zx_status_get_string(status);
    binding_.Close(status);
    // destory |enclosing_env_| should kill all processes.
    enclosing_env_.reset();
    on_disconnected_();
    return true;
  }
  return false;
}

std::string MakeTestHarnessEnvironmentName() {
  // Apply a random suffix to the environment name so that multiple hermetic
  // test harness environments may coexist under the same parent env.
  uint32_t random_env_suffix = 0;
  zx_cprng_draw(&random_env_suffix, sizeof random_env_suffix);
  return fxl::Substitute("modular_test_harness_$0",
                         std::to_string(random_env_suffix));
}

zx_status_t TestHarnessImpl::PopulateEnvServices(
    sys::testing::EnvironmentServices* env_services) {
  // The default set of component-provided services are all basemgr's hard
  // dependencies. A map of service name => component URL providing the service.
  std::map<std::string, std::string> default_svcs = {
      {fuchsia::auth::account::AccountManager::Name_,
       "fuchsia-pkg://fuchsia.com/account_manager#meta/account_manager.cmx"},
      {fuchsia::devicesettings::DeviceSettingsManager::Name_,
       "fuchsia-pkg://fuchsia.com/device_settings_manager#meta/"
       "device_settings_manager.cmx"}};

  std::set<std::string> added_svcs;

  // 1. Allow services to be inherited from parent environment.
  if (spec_.has_env_services_to_inherit()) {
    for (auto& svc_name : spec_.env_services_to_inherit()) {
      added_svcs.insert(svc_name);
      env_services->AllowParentService(svc_name);
    }
  }

  // 2. Inject component-provided services.
  if (auto retval = PopulateEnvServicesWithComponents(env_services,
                                                      &added_svcs) != ZX_OK) {
    return retval;
  }

  // 3. Inject service_dir services.
  if (auto retval = PopulateEnvServicesWithServiceDir(env_services,
                                                      &added_svcs) != ZX_OK) {
    return retval;
  }

  // 4. Inject the remaining default component-provided services.
  for (const auto& svc_component : default_svcs) {
    if (added_svcs.find(svc_component.first) != added_svcs.end()) {
      continue;
    }
    fuchsia::sys::LaunchInfo info;
    info.url = svc_component.second;
    env_services->AddServiceWithLaunchInfo(std::move(info),
                                           svc_component.first);
  }

  return ZX_OK;
}

zx_status_t TestHarnessImpl::PopulateEnvServicesWithComponents(
    sys::testing::EnvironmentServices* env_services,
    std::set<std::string>* added_svcs) {
  // Wire up client-specified injected services, and remove them from the
  // default injected services.
  if (!spec_.has_env_services() ||
      !spec_.env_services().has_services_from_components()) {
    return ZX_OK;
  }
  for (const auto& svc : spec_.env_services().services_from_components()) {
    if (added_svcs->find(svc.name) != added_svcs->end()) {
      FXL_LOG(ERROR) << svc.name
                     << " has already been injected into the environment, "
                        "cannot add twice.";
      return ZX_ERR_ALREADY_EXISTS;
    }
    added_svcs->insert(svc.name);

    fuchsia::sys::LaunchInfo info;
    info.url = svc.url;
    env_services->AddServiceWithLaunchInfo(std::move(info), svc.name);
  }

  return ZX_OK;
}  // namespace modular::testing

std::vector<std::string> GetDirListing(fuchsia::io::Directory* dir) {
  // Make a clone of |dir| since translating to a POSIX fd is destructive.
  fuchsia::io::NodePtr dir_copy;
  dir->Clone(fuchsia::io::OPEN_RIGHT_READABLE, dir_copy.NewRequest());

  std::vector<std::string> svcs;
  DIR* fd = fdopendir(
      fsl::OpenChannelAsFileDescriptor(dir_copy.Unbind().TakeChannel())
          .release());
  FXL_CHECK(fd != nullptr);

  struct dirent* dp = nullptr;
  while ((dp = readdir(fd)) != nullptr) {
    if (dp->d_name[0] != '.') {
      svcs.push_back(dp->d_name);
    }
  }

  closedir(fd);
  return svcs;
}

zx_status_t TestHarnessImpl::PopulateEnvServicesWithServiceDir(
    sys::testing::EnvironmentServices* env_services,
    std::set<std::string>* added_svcs) {
  if (!spec_.has_env_services() || !spec_.env_services().has_service_dir() ||
      !spec_.env_services().service_dir()) {
    return ZX_OK;
  }

  fuchsia::io::DirectoryPtr dir;
  dir.Bind(std::move(*spec_.mutable_env_services()->mutable_service_dir()));
  for (auto& svc_name : GetDirListing(dir.get())) {
    if (added_svcs->find(svc_name) != added_svcs->end()) {
      FXL_LOG(ERROR)
          << svc_name
          << " is already injected into the environment, cannot add twice.";
      return ZX_ERR_ALREADY_EXISTS;
    }
    env_services->AddService(
        std::make_unique<vfs::Service>([this, svc_name](
                                           zx::channel request,
                                           async_dispatcher_t* dispatcher) {
          FXL_CHECK(env_service_dir_->Connect(svc_name, std::move(request)) ==
                    ZX_OK);
        }),
        svc_name);
    added_svcs->insert(svc_name);
  }

  env_service_dir_ =
      std::make_unique<sys::ServiceDirectory>(dir.Unbind().TakeChannel());

  return ZX_OK;
}

void TestHarnessImpl::ParseConfig(std::string config, std::string config_path,
                                  ParseConfigCallback callback) {
  auto config_reader = modular::ModularConfigReader(config, config_path);
  callback(config_reader.GetBasemgrConfig(),
           config_reader.GetSessionmgrConfig());
}

void TestHarnessImpl::Run(fuchsia::modular::testing::TestHarnessSpec spec) {
  // Run() can only be called once.
  if (enclosing_env_) {
    CloseBindingIfError(ZX_ERR_ALREADY_BOUND);
    return;
  }

  spec_ = std::move(spec);

  if (CloseBindingIfError(SetupComponentInterception())) {
    return;
  }
  if (CloseBindingIfError(SetupFakeSessionAgent())) {
    return;
  }

  std::unique_ptr<sys::testing::EnvironmentServices> env_services =
      interceptor_.MakeEnvironmentServices(parent_env_);

  if (CloseBindingIfError(PopulateEnvServices(env_services.get()))) {
    return;
  }

  // Ledger configuration for tests by default:
  // * use a memory-backed FS for ledger.
  // * doesn't sync with a cloudprovider.
  auto* sessionmgr_config =
      spec_.mutable_sessionmgr_config();  // auto initialize.
  if (!sessionmgr_config->has_use_memfs_for_ledger()) {
    sessionmgr_config->set_use_memfs_for_ledger(true);
  }
  if (!sessionmgr_config->has_cloud_provider()) {
    sessionmgr_config->set_cloud_provider(
        fuchsia::modular::session::CloudProvider::NONE);
  }

  fuchsia::sys::EnvironmentOptions env_options;
  env_options.delete_storage_on_death = true;

  enclosing_env_ = sys::testing::EnclosingEnvironment::Create(
      MakeTestHarnessEnvironmentName(), parent_env_, std::move(env_services),
      env_options);

  zx::channel client;
  zx::channel request;
  FXL_CHECK(zx::channel::create(0u, &client, &request) == ZX_OK);
  basemgr_config_dir_ = MakeBasemgrConfigDir(spec_);
  basemgr_config_dir_->Serve(fuchsia::io::OPEN_RIGHT_READABLE,
                             std::move(request));

  fuchsia::sys::LaunchInfo info;
  info.url = kBasemgrUrl;
  info.flat_namespace = fuchsia::sys::FlatNamespace::New();
  info.flat_namespace->paths.push_back(modular_config::kOverriddenConfigDir);
  info.flat_namespace->directories.push_back(std::move(client));

  basemgr_ctrl_ = enclosing_env_->CreateComponent(std::move(info));
}

zx::channel TakeSvcFromFlatNamespace(
    fuchsia::sys::FlatNamespace* flat_namespace) {
  for (size_t i = 0; i < flat_namespace->paths.size(); i++) {
    if (flat_namespace->paths[i] == "/svc") {
      return std::move(flat_namespace->directories[i]);
    }
  }
  FXL_CHECK(false) << "Could not find /svc in component namespace.";
  return zx::channel();
}

zx_status_t TestHarnessImpl::SetupFakeSessionAgent() {
  auto interception_retval = interceptor_.InterceptURL(
      kSessionAgentFakeInterceptionUrl, kSessionAgentFakeInterceptionCmx,
      [this](fuchsia::sys::StartupInfo startup_info,
             std::unique_ptr<sys::testing::InterceptedComponent>
                 intercepted_component) {
        intercepted_session_agent_info_.component_context =
            std::make_unique<sys::ComponentContext>(
                std::make_shared<sys::ServiceDirectory>(
                    TakeSvcFromFlatNamespace(&startup_info.flat_namespace)),
                std::move(startup_info.launch_info.directory_request));
        intercepted_session_agent_info_.agent_driver.reset(
            new ::modular::AgentDriver<InterceptedSessionAgent>(
                intercepted_session_agent_info_.component_context.get(),
                [] {}));
        intercepted_session_agent_info_.intercepted_component =
            std::move(intercepted_component);

        FlushBufferedSessionAgentServices();
      });

  if (!interception_retval) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_OK;
}

fuchsia::modular::session::AppConfig MakeAppConfigWithUrl(std::string url) {
  fuchsia::modular::session::AppConfig app_config;
  app_config.set_url(url);
  return app_config;
}

fuchsia::modular::session::SessionShellMapEntry
MakeDefaultSessionShellMapEntry() {
  fuchsia::modular::session::SessionShellConfig config;
  config.mutable_app_config()->set_url(kSessionShellDefaultUrl);

  fuchsia::modular::session::SessionShellMapEntry entry;
  entry.set_name("");
  entry.set_config(std::move(config));
  return entry;
}

// static
std::unique_ptr<vfs::PseudoDir> TestHarnessImpl::MakeBasemgrConfigDir(
    const fuchsia::modular::testing::TestHarnessSpec& const_spec) {
  fuchsia::modular::testing::TestHarnessSpec spec;
  const_spec.Clone(&spec);

  auto* basemgr_config = spec.mutable_basemgr_config();
  // 1. Give base & story shell a default.
  if (!basemgr_config->has_base_shell() ||
      !basemgr_config->mutable_base_shell()->has_app_config()) {
    basemgr_config->mutable_base_shell()->set_app_config(
        MakeAppConfigWithUrl(kBaseShellDefaultUrl));
  }

  if (!basemgr_config->has_story_shell() ||
      !basemgr_config->mutable_story_shell()->has_app_config()) {
    basemgr_config->mutable_story_shell()->set_app_config(
        MakeAppConfigWithUrl(kStoryShellDefaultUrl));
  }

  // 1.1. Give session shell a default if not specified.
  if (!basemgr_config->has_session_shell_map() ||
      basemgr_config->session_shell_map().size() == 0) {
    basemgr_config->mutable_session_shell_map()->push_back(
        MakeDefaultSessionShellMapEntry());
  }

  auto* first_session_shell_entry =
      &basemgr_config->mutable_session_shell_map()->at(0);
  if (!first_session_shell_entry->has_config() ||
      !first_session_shell_entry->config().has_app_config() ||
      !first_session_shell_entry->config().app_config().has_url()) {
    first_session_shell_entry->mutable_config()->mutable_app_config()->set_url(
        kSessionShellDefaultUrl);
  }

  // 2. Configure a session agent and intercept/mock it for its capabilities.
  std::vector<std::string> sessionmgr_args;
  auto* sessionmgr_config =
      spec.mutable_sessionmgr_config();  // initialize if empty.
  auto* session_agents =
      sessionmgr_config->mutable_session_agents();  // initialize if empty.
  session_agents->push_back(kSessionAgentFakeInterceptionUrl);

  // 3. Write sessionmgr and basemgr configs into a single modular config
  // json object, as described in //peridot/docs/modular/guide/config.md
  std::string basemgr_json;
  std::string sessionmgr_json;
  XdrWrite(&basemgr_json, basemgr_config, XdrBasemgrConfig);
  XdrWrite(&sessionmgr_json, sessionmgr_config, XdrSessionmgrConfig);

  std::string modular_config_json =
      fxl::Substitute(R"({
      "$0": $1,
      "$2": $3
    })",
                      modular_config::kBasemgrConfigName, basemgr_json,
                      modular_config::kSessionmgrConfigName, sessionmgr_json);

  return MakeFilePathWithContents(modular_config::kStartupConfigFilePath,
                                  modular_config_json);
}

fuchsia::modular::testing::InterceptedComponentPtr
TestHarnessImpl::AddInterceptedComponentBinding(
    std::unique_ptr<sys::testing::InterceptedComponent> intercepted_component) {
  fuchsia::modular::testing::InterceptedComponentPtr ptr;
  auto impl = std::make_unique<InterceptedComponentImpl>(
      std::move(intercepted_component), ptr.NewRequest());

  // Hold on to |impl|.
  // Automatically remove/destroy |impl| if its associated binding closes.
  auto key = impl.get();
  impl->set_remove_handler(
      [this, key] { intercepted_component_impls_.erase(key); });
  intercepted_component_impls_[key] = std::move(impl);

  return ptr;
}

std::string GetCmxAsString(
    const fuchsia::modular::testing::InterceptSpec& intercept_spec) {
  std::string cmx_str = "";
  if (intercept_spec.has_extra_cmx_contents()) {
    if (!fsl::StringFromVmo(intercept_spec.extra_cmx_contents(), &cmx_str)) {
      // Not returning |cmx_str| since fsl::StringFromVmo doesn't guarantee that
      // |cmx_str| will be untouched on failure.
      return "";
    }
  }

  return cmx_str;
}

zx_status_t TestHarnessImpl::SetupComponentInterception() {
  if (!spec_.has_components_to_intercept()) {
    return ZX_OK;
  }
  for (const auto& intercept_spec : spec_.components_to_intercept()) {
    if (!interceptor_.InterceptURL(
            intercept_spec.component_url(), GetCmxAsString(intercept_spec),
            [this](fuchsia::sys::StartupInfo startup_info,
                   std::unique_ptr<sys::testing::InterceptedComponent>
                       intercepted_component) {
              binding_.events().OnNewComponent(
                  std::move(startup_info),
                  AddInterceptedComponentBinding(
                      std::move(intercepted_component)));
            })) {
      return ZX_ERR_INVALID_ARGS;
    }
  }
  return ZX_OK;
}

void TestHarnessImpl::FlushBufferedSessionAgentServices() {
  if (!intercepted_session_agent_info_.component_context) {
    return;
  }

  for (auto&& req : intercepted_session_agent_info_.buffered_service_requests) {
    intercepted_session_agent_info_.component_context->svc()->Connect(
        req.service_name, std::move(req.service_request));
  }
  intercepted_session_agent_info_.buffered_service_requests.clear();
}

}  // namespace modular::testing
