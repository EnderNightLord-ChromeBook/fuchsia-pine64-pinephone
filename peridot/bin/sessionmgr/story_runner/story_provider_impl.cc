// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/sessionmgr/story_runner/story_provider_impl.h"

#include <fuchsia/scenic/snapshot/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/function.h>
#include <lib/fsl/handles/object_info.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/zx/time.h>

#include <memory>
#include <utility>
#include <vector>

#include "peridot/bin/basemgr/cobalt/cobalt.h"
#include "peridot/bin/sessionmgr/focus.h"
#include "peridot/bin/sessionmgr/presentation_provider.h"
#include "peridot/bin/sessionmgr/storage/constants_and_utils.h"
#include "peridot/bin/sessionmgr/storage/session_storage.h"
#include "peridot/bin/sessionmgr/storage/story_storage.h"
#include "peridot/bin/sessionmgr/story/systems/story_visibility_system.h"
#include "peridot/bin/sessionmgr/story_runner/link_impl.h"
#include "peridot/bin/sessionmgr/story_runner/story_controller_impl.h"
#include "peridot/lib/common/teardown.h"
#include "peridot/lib/fidl/array_to_string.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/fidl/proxy.h"
#include "peridot/lib/rapidjson/rapidjson.h"
#include "src/lib/uuid/uuid.h"

// In tests prefetching mondrian saved ~30ms in story start up time.
#define PREFETCH_MONDRIAN 1

namespace modular {

constexpr char kSnapshotLoaderUrl[] = "snapshot";

class StoryProviderImpl::StopStoryCall : public Operation<> {
 public:
  using StoryRuntimesMap = std::map<std::string, struct StoryRuntimeContainer>;

  StopStoryCall(fidl::StringPtr story_id, const bool bulk,
                StoryRuntimesMap* const story_runtime_containers,
                MessageQueueManager* const message_queue_manager,
                ResultCall result_call)
      : Operation("StoryProviderImpl::DeleteStoryCall", std::move(result_call)),
        story_id_(story_id),
        bulk_(bulk),
        story_runtime_containers_(story_runtime_containers),
        message_queue_manager_(message_queue_manager) {}

 private:
  void Run() override {
    FlowToken flow{this};

    auto i = story_runtime_containers_->find(story_id_);
    if (i == story_runtime_containers_->end()) {
      FXL_LOG(WARNING) << "I was told to teardown story " << story_id_
                       << ", but I can't find it.";
      return;
    }

    FXL_DCHECK(i->second.controller_impl != nullptr);
    i->second.controller_impl->StopBulk(bulk_,
                                        [this, flow] { CleanupRuntime(flow); });
  }

  void CleanupRuntime(FlowToken flow) {
    // Here we delete the instance from whose operation a result callback was
    // received. Thus we must assume that the callback returns to a method of
    // the instance. If we delete the instance right here, |this| would be
    // deleted not just for the remainder of this function here, but also for
    // the remainder of all functions above us in the callstack, including
    // functions that run as methods of other objects owned by |this| or
    // provided to |this|. To avoid such problems, the delete is invoked
    // through the run loop.
    //
    // TODO(thatguy); Understand the above comment, and rewrite it.
    async::PostTask(async_get_default_dispatcher(), [this, flow] {
      story_runtime_containers_->erase(story_id_);
      message_queue_manager_->DeleteNamespace(
          EncodeModuleComponentNamespace(story_id_), [flow] {});
    });
  }

 private:
  const fidl::StringPtr story_id_;
  const bool bulk_;
  StoryRuntimesMap* const story_runtime_containers_;
  MessageQueueManager* const message_queue_manager_;
};

// Loads a StoryRuntimeContainer object and stores it in
// |story_provider_impl.story_runtime_containers_| so that the story is ready
// to be run.
class StoryProviderImpl::LoadStoryRuntimeCall
    : public Operation<StoryRuntimeContainer*> {
 public:
  LoadStoryRuntimeCall(StoryProviderImpl* const story_provider_impl,
                       SessionStorage* const session_storage,
                       fidl::StringPtr story_id, ResultCall result_call)
      : Operation("StoryProviderImpl::LoadStoryRuntimeCall",
                  std::move(result_call)),
        story_provider_impl_(story_provider_impl),
        session_storage_(session_storage),
        story_id_(story_id) {}

 private:
  void Run() override {
    FlowToken flow{this, &story_runtime_container_};

    // Use the existing controller, if possible.
    // This won't race against itself because it's managed by an operation
    // queue.
    auto i = story_provider_impl_->story_runtime_containers_.find(story_id_);
    if (i != story_provider_impl_->story_runtime_containers_.end()) {
      story_runtime_container_ = &i->second;
      return;
    }

    session_storage_->GetStoryData(story_id_)->WeakThen(
        GetWeakPtr(),
        [this, flow](fuchsia::modular::internal::StoryDataPtr story_data) {
          if (!story_data) {
            return;
            // Operation finishes since |flow| goes out of scope.
          }
          Cont(std::move(story_data), flow);
        });
  }

  void Cont(fuchsia::modular::internal::StoryDataPtr story_data,
            FlowToken flow) {
    session_storage_->GetStoryStorage(story_id_)->WeakThen(
        GetWeakPtr(), [this, story_data = std::move(story_data), flow](
                          std::unique_ptr<StoryStorage> story_storage) mutable {
          struct StoryRuntimeContainer container {
            .executor = std::make_unique<async::Executor>(
                async_get_default_dispatcher()),
            .storage = std::move(story_storage),
            .current_data = std::move(story_data),
          };

          container.model_owner = std::make_unique<StoryModelOwner>(
              story_id_, container.executor.get(),
              std::make_unique<NoopStoryModelStorage>());
          container.model_observer = container.model_owner->NewObserver();

          // Create systems that are part of this story.
          auto story_visibility_system =
              std::make_unique<StoryVisibilitySystem>(
                  container.model_owner->NewMutator());

          container.controller_impl = std::make_unique<StoryControllerImpl>(
              session_storage_, container.storage.get(),
              container.model_owner->NewMutator(),
              container.model_owner->NewObserver(),
              story_visibility_system.get(), story_provider_impl_);
          container.entity_provider =
              std::make_unique<StoryEntityProvider>(container.storage.get());

          // Hand ownership of systems over to |container|.
          container.systems.push_back(std::move(story_visibility_system));

          // Register a listener on the StoryModel so that we can signal
          // our watchers when relevant data changes.
          container.model_observer->RegisterListener(
              [id = story_id_, story_provider = story_provider_impl_](
                  const fuchsia::modular::storymodel::StoryModel& model) {
                story_provider->NotifyStoryStateChange(id);
              });

          auto it = story_provider_impl_->story_runtime_containers_.emplace(
              story_id_, std::move(container));
          story_runtime_container_ = &it.first->second;
        });
  }

  StoryProviderImpl* const story_provider_impl_;  // not owned
  SessionStorage* const session_storage_;         // not owned
  const fidl::StringPtr story_id_;

  // Return value.
  StoryRuntimeContainer* story_runtime_container_ = nullptr;

  // Sub operations run in this queue.
  OperationQueue operation_queue_;
};

class StoryProviderImpl::StopAllStoriesCall : public Operation<> {
 public:
  StopAllStoriesCall(StoryProviderImpl* const story_provider_impl,
                     ResultCall result_call)
      : Operation("StoryProviderImpl::StopAllStoriesCall",
                  std::move(result_call)),
        story_provider_impl_(story_provider_impl) {}

 private:
  void Run() override {
    FlowToken flow{this};

    for (auto& it : story_provider_impl_->story_runtime_containers_) {
      // Each callback has a copy of |flow| which only goes out-of-scope
      // once the story corresponding to |it| stops.
      //
      // TODO(thatguy): If the StoryControllerImpl is deleted before it can
      // complete StopWithoutNotifying(), we will never be called back and the
      // OperationQueue on which we're running will block.  Moving over to
      // fit::promise will allow us to observe cancellation.
      operations_.Add(std::make_unique<StopStoryCall>(
          it.first, true /* bulk */,
          &story_provider_impl_->story_runtime_containers_,
          story_provider_impl_->component_context_info_.message_queue_manager,
          [flow] {}));
    }
  }

  OperationCollection operations_;

  StoryProviderImpl* const story_provider_impl_;  // not owned
};

class StoryProviderImpl::StopStoryShellCall : public Operation<> {
 public:
  StopStoryShellCall(StoryProviderImpl* const story_provider_impl,
                     ResultCall result_call)
      : Operation("StoryProviderImpl::StopStoryShellCall",
                  std::move(result_call)),
        story_provider_impl_(story_provider_impl) {}

 private:
  void Run() override {
    FlowToken flow{this};
    if (story_provider_impl_->preloaded_story_shell_app_) {
      // Calling Teardown() below will branch |flow| into normal and timeout
      // paths. |flow| must go out of scope when either of the paths
      // finishes.
      FlowTokenHolder branch{flow};
      story_provider_impl_->preloaded_story_shell_app_->Teardown(
          kBasicTimeout,
          [branch] { std::unique_ptr<FlowToken> flow = branch.Continue(); });
    }
  }

  StoryProviderImpl* const story_provider_impl_;  // not owned
};

class StoryProviderImpl::GetStoryEntityProviderCall
    : public Operation<StoryEntityProvider*> {
 public:
  GetStoryEntityProviderCall(StoryProviderImpl* const story_provider_impl,
                             const std::string& story_id,
                             ResultCall result_call)
      : Operation("StoryProviderImpl::GetStoryEntityProviderCall",
                  std::move(result_call)),
        story_provider_impl_(story_provider_impl),
        story_id_(story_id) {}

 private:
  void Run() override {
    FlowToken flow{this, &story_entity_provider_};

    operation_queue_.Add(std::make_unique<LoadStoryRuntimeCall>(
        story_provider_impl_, story_provider_impl_->session_storage_, story_id_,
        [this, flow](StoryRuntimeContainer* story_controller_container) {
          if (story_controller_container) {
            story_entity_provider_ =
                story_controller_container->entity_provider.get();
          }
        }));
  }

  StoryProviderImpl* const story_provider_impl_;  // not owned

  // The returned story entity provider.
  StoryEntityProvider* story_entity_provider_ = nullptr;

  fuchsia::modular::StoryInfoPtr story_info_;

  OperationQueue operation_queue_;

  std::string story_id_;
};

StoryProviderImpl::StoryProviderImpl(
    Environment* const user_environment, std::string device_id,
    SessionStorage* const session_storage,
    fuchsia::modular::AppConfig story_shell_config,
    fuchsia::modular::StoryShellFactoryPtr story_shell_factory,
    const ComponentContextInfo& component_context_info,
    fuchsia::modular::FocusProviderPtr focus_provider,
    fuchsia::modular::UserIntelligenceProvider* const
        user_intelligence_provider,
    fuchsia::app::discover::DiscoverRegistry* const discover_registry,
    fuchsia::modular::ModuleResolver* const module_resolver,
    EntityProviderRunner* const entity_provider_runner,
    modular::ModuleFacetReader* const module_facet_reader,
    PresentationProvider* const presentation_provider,
    fuchsia::ui::viewsv1::ViewSnapshotPtr view_snapshot,
    const bool enable_story_shell_preload)
    : user_environment_(user_environment),
      session_storage_(session_storage),
      device_id_(std::move(device_id)),
      story_shell_config_(std::move(story_shell_config)),
      story_shell_factory_(std::move(story_shell_factory)),
      enable_story_shell_preload_(enable_story_shell_preload),
      component_context_info_(component_context_info),
      user_intelligence_provider_(user_intelligence_provider),
      discover_registry_(discover_registry),
      module_resolver_(module_resolver),
      entity_provider_runner_(entity_provider_runner),
      module_facet_reader_(module_facet_reader),
      presentation_provider_(presentation_provider),
      focus_provider_(std::move(focus_provider)),
      focus_watcher_binding_(this),
      view_snapshot_(std::move(view_snapshot)),
      weak_factory_(this) {
  session_storage_->set_on_story_deleted(
      [weak_ptr = weak_factory_.GetWeakPtr()](fidl::StringPtr story_id) {
        if (!weak_ptr)
          return;
        weak_ptr->OnStoryStorageDeleted(std::move(story_id));
      });
  session_storage_->set_on_story_updated(
      [weak_ptr = weak_factory_.GetWeakPtr()](
          fidl::StringPtr story_id,
          fuchsia::modular::internal::StoryData story_data) {
        if (!weak_ptr)
          return;
        weak_ptr->OnStoryStorageUpdated(std::move(story_id),
                                        std::move(story_data));
      });

  focus_provider_->Watch(focus_watcher_binding_.NewBinding());
  // As an optimization, since app startup time is long, we optimistically
  // load a story shell instance even if there are no stories that need it
  // yet. This can reduce the time to first frame.
  MaybeLoadStoryShellDelayed();
}

StoryProviderImpl::~StoryProviderImpl() = default;

void StoryProviderImpl::Connect(
    fidl::InterfaceRequest<fuchsia::modular::StoryProvider> request) {
  bindings_.AddBinding(this, std::move(request));
}

void StoryProviderImpl::StopAllStories(fit::function<void()> callback) {
  operation_queue_.Add(
      std::make_unique<StopAllStoriesCall>(this, std::move(callback)));
}

void StoryProviderImpl::SetSessionShell(
    fuchsia::modular::SessionShellPtr session_shell) {
  // Not on operation queue, because it's called only after all stories have
  // been stopped or none are running yet, i.e. when no Operations that would
  // call this interface are scheduled. If there is an operation pending here,
  // then it would pertain to a story running in the new session shell started
  // by puppet master or an agent, so we must assign this now.
  //
  // TODO(mesch): It may well be that we need to revisit this when we support
  // starting stories, or swapping session shells, through puppet master, i.e.
  // from outside the session shell.
  //
  // TODO(mesch): Add a WARNING log if the operation is not empty.
  session_shell_ = std::move(session_shell);
}

void StoryProviderImpl::Teardown(fit::function<void()> callback) {
  // Closing all binding to this instance ensures that no new messages come
  // in, though previous messages need to be processed. The stopping of
  // stories is done on |operation_queue_| since that must strictly happen
  // after all pending messgages have been processed.
  bindings_.CloseAll();
  operation_queue_.Add(std::make_unique<StopAllStoriesCall>(this, [] {}));
  operation_queue_.Add(
      std::make_unique<StopStoryShellCall>(this, std::move(callback)));
}

// |fuchsia::modular::StoryProvider|
void StoryProviderImpl::Watch(
    fidl::InterfaceHandle<fuchsia::modular::StoryProviderWatcher> watcher) {
  auto watcher_ptr = watcher.Bind();
  for (const auto& item : story_runtime_containers_) {
    const auto& container = item.second;
    FXL_CHECK(container.current_data->has_story_info());
    watcher_ptr->OnChange(CloneStruct(container.current_data->story_info()),
                          container.model_observer->model().runtime_state(),
                          container.model_observer->model().visibility_state());
  }
  watchers_.AddInterfacePtr(std::move(watcher_ptr));
}

// |fuchsia::modular::StoryProvider|
void StoryProviderImpl::WatchActivity(
    fidl::InterfaceHandle<fuchsia::modular::StoryActivityWatcher> watcher) {
  auto watcher_ptr = watcher.Bind();
  for (const auto& item : story_runtime_containers_) {
    const auto& container = item.second;
    watcher_ptr->OnStoryActivityChange(
        container.model_observer->model().name(),
        container.controller_impl->GetOngoingActivities());
  }
  activity_watchers_.AddInterfacePtr(std::move(watcher_ptr));
}

std::unique_ptr<AsyncHolderBase> StoryProviderImpl::StartStoryShell(
    fidl::StringPtr story_id, fuchsia::ui::views::ViewToken view_token,
    fidl::InterfaceRequest<fuchsia::modular::StoryShell> story_shell_request) {
  // When we're supplied a StoryShellFactory, use it to get StoryShells instead
  // of launching the story shell as a separate component. In this case, there
  // is also nothing to preload, so ignore |preloaded_story_shell_app_|.
  if (story_shell_factory_) {
    story_shell_factory_->AttachStory(story_id, std::move(story_shell_request));

    auto on_teardown =
        [this, story_id = std::move(story_id)](fit::function<void()> done) {
          story_shell_factory_->DetachStory(story_id, std::move(done));
        };

    return std::make_unique<ClosureAsyncHolder>(story_id /* name */,
                                                std::move(on_teardown));
  }

  MaybeLoadStoryShell();

  // TODO(SCN-1019): This is a temporary hack to cache the endpoint ID of the
  // view so that framework can make snapshot requests.
  view_endpoints_[story_id] = fsl::GetKoid(view_token.value.get());

  fuchsia::ui::app::ViewProviderPtr view_provider;
  preloaded_story_shell_app_->services().ConnectToService(
      view_provider.NewRequest());
  view_provider->CreateView(std::move(view_token.value), nullptr, nullptr);

  preloaded_story_shell_app_->services().ConnectToService(
      std::move(story_shell_request));

  auto story_shell_holder = std::move(preloaded_story_shell_app_);

  // Kickoff another fuchsia::modular::StoryShell, to make it faster for next
  // story. We optimize even further by delaying the loading of the next story
  // shell instance by waiting a few seconds.
  MaybeLoadStoryShellDelayed();

  return story_shell_holder;
}

void StoryProviderImpl::MaybeLoadStoryShellDelayed() {
#if PREFETCH_MONDRIAN
  // In tests, we don't care about story shell launch latency as much, and
  // don't want the test to wait for the delayed task to finish.
  //
  // When using a StoryShellFactory, the |preloaded_story_shell_app_| is never
  // used, so it should not be loaded.
  if (!enable_story_shell_preload_ || story_shell_factory_) {
    return;
  }

  async::PostDelayedTask(
      async_get_default_dispatcher(),
      [weak_this = weak_factory_.GetWeakPtr()] {
        if (weak_this) {
          weak_this->operation_queue_.Add(
              std::make_unique<SyncCall>([weak_this] {
                if (weak_this) {
                  weak_this->MaybeLoadStoryShell();
                }
              }));
        }
      },
      zx::sec(5));
#endif
}

void StoryProviderImpl::MaybeLoadStoryShell() {
  if (preloaded_story_shell_app_) {
    return;
  }

  preloaded_story_shell_app_ =
      std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
          user_environment_->GetLauncher(), CloneStruct(story_shell_config_));
}

fuchsia::modular::StoryInfoPtr StoryProviderImpl::GetCachedStoryInfo(
    std::string story_id) {
  auto it = story_runtime_containers_.find(story_id);
  if (it == story_runtime_containers_.end()) {
    return nullptr;
  }
  FXL_CHECK(it->second.current_data->has_story_info());
  return CloneOptional(it->second.current_data->story_info());
}

// |fuchsia::modular::StoryProvider|
void StoryProviderImpl::GetStoryInfo(std::string story_id,
                                     GetStoryInfoCallback callback) {
  auto on_run = Future<>::Create("StoryProviderImpl.GetStoryInfo.on_run");
  auto done = on_run
                  ->AsyncMap([this, story_id] {
                    return session_storage_->GetStoryData(story_id);
                  })
                  ->Map([](fuchsia::modular::internal::StoryDataPtr story_data)
                            -> fuchsia::modular::StoryInfoPtr {
                    if (!story_data) {
                      return nullptr;
                    }
                    if (!story_data->has_story_info()) {
                      return nullptr;
                    }
                    return fidl::MakeOptional(
                        std::move(*story_data->mutable_story_info()));
                  });
  operation_queue_.Add(WrapFutureAsOperation(
      "StoryProviderImpl::GetStoryInfo", on_run, done, std::move(callback)));
}

// Called by StoryControllerImpl on behalf of ModuleContextImpl
void StoryProviderImpl::RequestStoryFocus(fidl::StringPtr story_id) {
  FXL_LOG(INFO) << "RequestStoryFocus() " << story_id;
  focus_provider_->Request(story_id);
}

void StoryProviderImpl::AttachView(
    fidl::StringPtr story_id,
    fuchsia::ui::views::ViewHolderToken view_holder_token) {
  FXL_CHECK(session_shell_);
  fuchsia::modular::ViewIdentifier view_id;
  view_id.story_id = std::move(story_id);
  session_shell_->AttachView2(std::move(view_id), std::move(view_holder_token));
}

void StoryProviderImpl::DetachView(fidl::StringPtr story_id,
                                   fit::function<void()> done) {
  FXL_CHECK(session_shell_);
  fuchsia::modular::ViewIdentifier view_id;
  view_id.story_id = std::move(story_id);
  session_shell_->DetachView(std::move(view_id), std::move(done));
}

void StoryProviderImpl::NotifyStoryStateChange(fidl::StringPtr story_id) {
  auto it = story_runtime_containers_.find(story_id);
  if (it == story_runtime_containers_.end()) {
    // If this call arrives while DeleteStory() is in
    // progress, the story controller might already be gone
    // from here.
    return;
  }
  NotifyStoryWatchers(it->second.current_data.get(),
                      it->second.model_observer->model().runtime_state(),
                      it->second.model_observer->model().visibility_state());
}

void StoryProviderImpl::NotifyStoryActivityChange(
    fidl::StringPtr story_id,
    fidl::VectorPtr<fuchsia::modular::OngoingActivityType> ongoing_activities) {
  for (const auto& i : activity_watchers_.ptrs()) {
    (*i)->OnStoryActivityChange(story_id, ongoing_activities.Clone());
  }
}

// |fuchsia::modular::StoryProvider|
void StoryProviderImpl::GetController(
    std::string story_id,
    fidl::InterfaceRequest<fuchsia::modular::StoryController> request) {
  operation_queue_.Add(std::make_unique<LoadStoryRuntimeCall>(
      this, session_storage_, story_id,
      [request = std::move(request)](
          StoryRuntimeContainer* story_controller_container) mutable {
        if (story_controller_container) {
          story_controller_container->controller_impl->Connect(
              std::move(request));
        }
      }));
}

// |fuchsia::modular::StoryProvider|
void StoryProviderImpl::GetStories(
    fidl::InterfaceHandle<fuchsia::modular::StoryProviderWatcher> watcher,
    PreviousStoriesCallback callback) {
  auto watcher_ptr = watcher.Bind();
  auto on_run = Future<>::Create("StoryProviderImpl.GetStories.on_run");
  auto done =
      on_run->AsyncMap([this] { return session_storage_->GetAllStoryData(); })
          ->Map([this, watcher_ptr = std::move(watcher_ptr)](
                    std::vector<fuchsia::modular::internal::StoryData>
                        all_story_data) mutable {
            std::vector<fuchsia::modular::StoryInfo> result;

            for (auto& story_data : all_story_data) {
              if (!story_data.story_options().kind_of_proto_story) {
                if (!story_data.has_story_info()) {
                  continue;
                }
                result.push_back(std::move(*story_data.mutable_story_info()));
              }
            }

            if (watcher_ptr) {
              watchers_.AddInterfacePtr(std::move(watcher_ptr));
            }
            return result;
          });

  operation_queue_.Add(WrapFutureAsOperation(
      "StoryProviderImpl::GetStories", on_run, done, std::move(callback)));
}

// |fuchsia::modular::StoryProvider|
void StoryProviderImpl::PreviousStories(PreviousStoriesCallback callback) {
  auto on_run = Future<>::Create("StoryProviderImpl.PreviousStories.on_run");
  auto done =
      on_run->AsyncMap([this] { return session_storage_->GetAllStoryData(); })
          ->Map([](std::vector<fuchsia::modular::internal::StoryData>
                       all_story_data) {
            std::vector<fuchsia::modular::StoryInfo> result;

            for (auto& story_data : all_story_data) {
              if (!story_data.story_options().kind_of_proto_story) {
                if (!story_data.has_story_info()) {
                  continue;
                }
                result.push_back(std::move(*story_data.mutable_story_info()));
              }
            }
            return result;
          });
  operation_queue_.Add(WrapFutureAsOperation(
      "StoryProviderImpl::PreviousStories", on_run, done, std::move(callback)));
}

void StoryProviderImpl::OnStoryStorageUpdated(
    fidl::StringPtr story_id,
    fuchsia::modular::internal::StoryData story_data) {
  // If we have a StoryRuntimeContainer for this story id, update our cached
  // StoryData and get runtime state available from it.
  //
  // Otherwise, use defaults for an unloaded story and send a request for the
  // story to start running (stories should start running by default).
  fuchsia::modular::StoryState runtime_state =
      fuchsia::modular::StoryState::STOPPED;
  fuchsia::modular::StoryVisibilityState visibility_state =
      fuchsia::modular::StoryVisibilityState::DEFAULT;
  auto i = story_runtime_containers_.find(story_data.story_info().id);
  if (i != story_runtime_containers_.end()) {
    runtime_state = i->second.model_observer->model().runtime_state();
    visibility_state = i->second.model_observer->model().visibility_state();
    i->second.current_data = CloneOptional(story_data);
  } else {
    fuchsia::modular::StoryControllerPtr story_controller;
    GetController(story_id, story_controller.NewRequest());
    story_controller->RequestStart();
  }
  NotifyStoryWatchers(&story_data, runtime_state, visibility_state);
}

void StoryProviderImpl::OnStoryStorageDeleted(fidl::StringPtr story_id) {
  operation_queue_.Add(std::make_unique<StopStoryCall>(
      story_id, false /* bulk */, &story_runtime_containers_,
      component_context_info_.message_queue_manager, [this, story_id] {
        for (const auto& i : watchers_.ptrs()) {
          (*i)->OnDelete(story_id);
        }
      }));
}

// |fuchsia::modular::FocusWatcher|
void StoryProviderImpl::OnFocusChange(fuchsia::modular::FocusInfoPtr info) {
  operation_queue_.Add(std::make_unique<SyncCall>([this,
                                                   info = std::move(info)]() {
    if (info->device_id != device_id_) {
      return;
    }

    if (info->focused_story_id.is_null()) {
      return;
    }

    auto i = story_runtime_containers_.find(info->focused_story_id.get());
    if (i == story_runtime_containers_.end()) {
      FXL_LOG(ERROR) << "Story controller not found for focused story "
                     << info->focused_story_id;
      return;
    }

    // Last focus time is recorded in the ledger, and story provider
    // watchers are notified through watching SessionStorage.
    auto on_run = Future<>::Create("StoryProviderImpl.OnFocusChange.on_run");
    auto done = on_run->AsyncMap([this, story_id = info->focused_story_id] {
      zx_time_t now = 0;
      zx_clock_get_new(ZX_CLOCK_UTC, &now);
      return session_storage_->UpdateLastFocusedTimestamp(story_id, now);
    });
    fit::function<void()> callback = [] {};
    operation_queue_.Add(WrapFutureAsOperation(
        "StoryProviderImpl::OnFocusChange", on_run, done, std::move(callback)));
  }));
}

void StoryProviderImpl::NotifyStoryWatchers(
    const fuchsia::modular::internal::StoryData* story_data,
    const fuchsia::modular::StoryState story_state,
    const fuchsia::modular::StoryVisibilityState story_visibility_state) {
  if (!story_data || story_data->story_options().kind_of_proto_story) {
    return;
  }
  for (const auto& i : watchers_.ptrs()) {
    if (!story_data->has_story_info()) {
      continue;
    }
    (*i)->OnChange(CloneStruct(story_data->story_info()), story_state,
                   story_visibility_state);
  }
}

void StoryProviderImpl::CreateEntity(
    const std::string& story_id, fidl::StringPtr type,
    fuchsia::mem::Buffer data,
    fidl::InterfaceRequest<fuchsia::modular::Entity> entity_request,
    fit::function<void(std::string /* entity_reference */)> callback) {
  operation_queue_.Add(std::make_unique<GetStoryEntityProviderCall>(
      this, story_id,
      [this, type, story_id, data = std::move(data),
       callback = std::move(callback),
       entity_request = std::move(entity_request)](
          StoryEntityProvider* entity_provider) mutable {
        // Once the entity provider for the given story is available, create
        // the entity.
        entity_provider->CreateEntity(
            type, std::move(data),
            [this, story_id, callback = std::move(callback),
             entity_request =
                 std::move(entity_request)](std::string cookie) mutable {
              if (cookie.empty()) {
                // Return nullptr to indicate the entity creation failed.
                callback(nullptr);
                return;
              }

              std::string entity_reference =
                  entity_provider_runner_->CreateStoryEntityReference(story_id,
                                                                      cookie);

              // Once the entity reference has been created, it can be
              // used to connect the entity request.
              fuchsia::modular::EntityResolverPtr resolver;
              entity_provider_runner_->ConnectEntityResolver(
                  resolver.NewRequest());
              resolver->ResolveEntity(entity_reference,
                                      std::move(entity_request));

              callback(entity_reference);
            });
      }));
}

void StoryProviderImpl::ConnectToStoryEntityProvider(
    const std::string& story_id,
    fidl::InterfaceRequest<fuchsia::modular::EntityProvider>
        entity_provider_request) {
  operation_queue_.Add(std::make_unique<GetStoryEntityProviderCall>(
      this, story_id,
      [entity_provider_request = std::move(entity_provider_request)](
          StoryEntityProvider* entity_provider) mutable {
        entity_provider->Connect(std::move(entity_provider_request));
      }));
}

void StoryProviderImpl::GetPresentation(
    fidl::StringPtr story_id,
    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> request) {
  presentation_provider_->GetPresentation(std::move(story_id),
                                          std::move(request));
}

void StoryProviderImpl::WatchVisualState(
    fidl::StringPtr story_id,
    fidl::InterfaceHandle<fuchsia::modular::StoryVisualStateWatcher> watcher) {
  presentation_provider_->WatchVisualState(std::move(story_id),
                                           std::move(watcher));
}

void StoryProviderImpl::TakeSnapshot(
    fidl::StringPtr story_id,
    fit::function<void(fuchsia::mem::Buffer)> callback) {
  auto it = view_endpoints_.find(story_id);
  if (it != view_endpoints_.end()) {
    view_snapshot_->TakeSnapshot(it->second, [callback = std::move(callback)](
                                                 fuchsia::mem::Buffer buffer) {
      callback(std::move(buffer));
    });
  } else {
    callback(fuchsia::mem::Buffer{});
  }
}

void StoryProviderImpl::StartSnapshotLoader(
    fuchsia::ui::views::ViewToken view_token,
    fidl::InterfaceRequest<fuchsia::scenic::snapshot::Loader> loader_request) {
  if (!snapshot_loader_app_) {
    fuchsia::modular::AppConfig snapshot_loader_config;
    snapshot_loader_config.url = kSnapshotLoaderUrl;

    snapshot_loader_app_ =
        std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
            user_environment_->GetLauncher(),
            std::move(snapshot_loader_config));
  }

  fuchsia::sys::ServiceProviderPtr service_provider;
  fuchsia::ui::app::ViewProviderPtr view_provider;
  snapshot_loader_app_->services().ConnectToService(view_provider.NewRequest());
  view_provider->CreateView(std::move(view_token.value),
                            service_provider.NewRequest(), nullptr);

  service_provider->ConnectToService(fuchsia::scenic::snapshot::Loader::Name_,
                                     loader_request.TakeChannel());
}

}  // namespace modular
