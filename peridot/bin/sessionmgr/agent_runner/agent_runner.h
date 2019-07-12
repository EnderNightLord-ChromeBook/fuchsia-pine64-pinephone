// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SESSIONMGR_AGENT_RUNNER_AGENT_RUNNER_H_
#define PERIDOT_BIN_SESSIONMGR_AGENT_RUNNER_AGENT_RUNNER_H_

#include <fuchsia/auth/cpp/fidl.h>
#include <fuchsia/ledger/cpp/fidl.h>
#include <fuchsia/ledger/internal/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async/cpp/operation.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_ptr_set.h>
#include <src/lib/fxl/macros.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "peridot/bin/sessionmgr/agent_runner/agent_runner_storage.h"
#include "peridot/bin/sessionmgr/agent_runner/agent_service_index.h"

namespace modular {

// This is the component namespace we give to all agents; used for namespacing
// storage between different component types.
constexpr char kAgentComponentNamespace[] = "agents";

class AgentContextImpl;
class EntityProviderRunner;
class MessageQueueManager;

// This class provides a way for components to connect to agents and
// manages the life time of a running agent.
class AgentRunner : AgentRunnerStorage::NotificationDelegate {
 public:
  AgentRunner(
      fuchsia::sys::Launcher* launcher,
      MessageQueueManager* message_queue_manager,
      fuchsia::ledger::internal::LedgerRepository* ledger_repository,
      AgentRunnerStorage* agent_runner_storage,
      fuchsia::auth::TokenManager* token_manager,
      fuchsia::modular::UserIntelligenceProvider* user_intelligence_provider,
      EntityProviderRunner* entity_provider_runner,
      std::unique_ptr<AgentServiceIndex> agent_service_index = nullptr);
  ~AgentRunner() override;

  // |callback| is called after - (1) all agents have been shutdown and (2)
  // no new tasks are scheduled to run.
  void Teardown(fit::function<void()> callback);

  // Connects to an agent (and starts it up if it doesn't exist) through
  // |fuchsia::modular::Agent.Connect|. Called using
  // fuchsia::modular::ComponentContext.
  void ConnectToAgent(const std::string& requestor_url,
                      const std::string& agent_url,
                      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider>
                          incoming_services_request,
                      fidl::InterfaceRequest<fuchsia::modular::AgentController>
                          agent_controller_request);

  // Supports implementation of ComponentContext/ConnectToAgentService().
  void ConnectToAgentService(const std::string& requestor_url,
                             fuchsia::modular::AgentServiceRequest request);

  // Connects to an agent (and starts it up if it doesn't exist) through its
  // |fuchsia::modular::EntityProvider| service.
  void ConnectToEntityProvider(
      const std::string& agent_url,
      fidl::InterfaceRequest<fuchsia::modular::EntityProvider>
          entity_provider_request,
      fidl::InterfaceRequest<fuchsia::modular::AgentController>
          agent_controller_request);

  // Removes an agent. Called by AgentContextImpl when it is done.
  // NOTE: This should NOT take a const reference, since |agent_url| will die
  // the moment we delete |AgentContextImpl|.
  void RemoveAgent(std::string agent_url);

  // fuchsia::modular::Agent at |agent_url| is run (if not already running)
  // and fuchsia::modular::Agent.RunTask() is called with |task_id| as the
  // agent specified identifier for the task when a trigger condition
  // specified in |task_info| is satisfied. The trigger condition is also
  // replicated to the ledger and the task my get scheduled on other user
  // devices too.
  void ScheduleTask(
      const std::string& agent_url, fuchsia::modular::TaskInfo task_info,
      fit::function<void(bool)> done = [](bool) {});

  // Deletes a task for |agent_url| that is identified by agent provided
  // |task_id|. The trigger condition is removed from the ledger.
  void DeleteTask(const std::string& agent_url, const std::string& task_id);

 private:
  // Used by ConnectToAgentService() to connect to the agent (if known) and its
  // named service. Calls ConnectToAgent(), providing a temporary
  // |ServiceProviderPtr| on which to then invoke ConnecToService() with the
  // given service_name and channel.
  //
  // |requestor_url| The URL of the component requesting the service.
  // |agent_url| The URL of the agent believed to provide the service.
  // |agent_controller_request| Returns the object that maintains the requestor
  // connection to the agent.
  // |service_name| The name of the requested service.
  // |channel| The channel associated with the requestor's pending service
  // request, to be used to communicate with the service, once connected.
  void ConnectToService(
      std::string requestor_url, std::string agent_url,
      fidl::InterfaceRequest<fuchsia::modular::AgentController>
          agent_controller_request,
      std::string service_name, ::zx::channel channel);

  // During ConnectToAgentService, if an agent is not found, close the channel
  // established for the service, and indicate the reason with FIDL epitaph
  // error ZX_ERR_NOT_FOUND.
  void HandleAgentServiceNotFound(::zx::channel channel,
                                  std::string service_name);

  // Schedules the agent to start running if it isn't already running (e.g.,
  // it could be not running or in the middle of terminating). Once the agent
  // is in a running state, calls |done|.
  void EnsureAgentIsRunning(const std::string& agent_url,
                            fit::function<void()> done);

  // Actually starts up an agent (used by |EnsureAgentIsRunning()| above).
  void RunAgent(const std::string& agent_url);

  // Will also start and initialize the agent as a consequence.
  void ForwardConnectionsToAgent(const std::string& agent_url);

  // Schedules a task that triggers when a new message is available on a
  // message queue.
  //
  // |agent_url| The URL of the agent creating the trigger. Only the message
  // queue owner can schedule a task with a new message trigger, and thus this
  // is also the agent url of the owner of the message queue.
  // |queue_name| The name of the message queue to observe.
  // |task_id| The identifier for the task.
  void ScheduleMessageQueueNewMessageTask(const std::string& agent_url,
                                          const std::string& task_id,
                                          const std::string& queue_name);

  // Schedules a task that triggers when a message queue is deleted.
  //
  // |agent_url| The URL of the agent creating the trigger.
  // |queue_token| The token of the queue that is to be observed.
  // |task_id| The identifier of the task.
  void ScheduleMessageQueueDeletionTask(const std::string& agent_url,
                                        const std::string& task_id,
                                        const std::string& queue_token);

  // Deletes the task scheduled for |agent_url| and |task_id|, regardless of
  // the task type.
  void DeleteMessageQueueTask(const std::string& agent_url,
                              const std::string& task_id);

  // For triggers based on alarms.
  void ScheduleAlarmTask(const std::string& agent_url,
                         const std::string& task_id, uint32_t alarm_in_seconds,
                         bool is_new_request);
  void DeleteAlarmTask(const std::string& agent_url,
                       const std::string& task_id);

  // A set of all agents that are either running or scheduled to be run.
  std::vector<std::string> GetAllAgents();

  // |AgentRunnerStorage::Delegate|
  void AddedTask(const std::string& key,
                 AgentRunnerStorage::TriggerInfo data) override;

  // |AgentRunnerStorage::Delegate|
  void DeletedTask(const std::string& key) override;

  // agent URL -> { task id -> queue name }
  std::map<std::string, std::map<std::string, std::string>> watched_queues_;

  // agent URL -> { task id -> alarm in seconds }
  std::map<std::string, std::map<std::string, uint32_t>> running_alarms_;

  // agent URL -> pending agent connections
  // This map holds connections to an agent that we hold onto while the
  // existing agent is in a terminating state.
  struct PendingAgentConnectionEntry {
    const std::string requestor_url;
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider>
        incoming_services_request;
    fidl::InterfaceRequest<fuchsia::modular::AgentController>
        agent_controller_request;
  };
  std::map<std::string, std::vector<struct PendingAgentConnectionEntry>>
      pending_agent_connections_;

  // agent URL -> pending entity provider connection
  // This map holds connections to an agents' fuchsia::modular::EntityProvider
  // that we hold onto while the existing agent is in a terminating state.
  struct PendingEntityProviderConnectionEntry {
    fidl::InterfaceRequest<fuchsia::modular::EntityProvider>
        entity_provider_request;
    fidl::InterfaceRequest<fuchsia::modular::AgentController>
        agent_controller_request;
  };
  std::map<std::string, struct PendingEntityProviderConnectionEntry>
      pending_entity_provider_connections_;

  // agent URL -> done callbacks to invoke once agent has started.
  // Holds requests to start an agent; in case an agent is already in a
  // terminating state, we pend those requests here until the agent
  // terminates.
  std::map<std::string, std::vector<fit::function<void()>>>
      run_agent_callbacks_;

  // agent URL -> modular.fuchsia::modular::AgentContext
  std::map<std::string, std::unique_ptr<AgentContextImpl>> running_agents_;

  // ledger key -> [agent URL, task ID]
  //
  // Used to delete entries from the maps above when a ledger key is
  // deleted. This saves us from having to parse a ledger key, which
  // becomes impossible once we use hashes to construct it, or from
  // having to read the value from the previous snapshot, which would
  // be nifty but is easy only once we have Operations.
  std::map<std::string, std::pair<std::string, std::string>>
      task_by_ledger_key_;

  fuchsia::sys::Launcher* const launcher_;
  MessageQueueManager* const message_queue_manager_;
  fuchsia::ledger::internal::LedgerRepository* const ledger_repository_;
  // |agent_runner_storage_| must outlive this class.
  AgentRunnerStorage* const agent_runner_storage_;
  fuchsia::auth::TokenManager* const token_manager_;
  fuchsia::modular::UserIntelligenceProvider* const user_intelligence_provider_;
  EntityProviderRunner* const entity_provider_runner_;

  // When this is marked true, no new new tasks will be scheduled.
  std::shared_ptr<bool> terminating_;

  OperationQueue operation_queue_;

  std::unique_ptr<AgentServiceIndex> agent_service_index_;

  // Operations implemented here.
  class InitializeCall;
  class UpdateCall;
  class DeleteCall;

  FXL_DISALLOW_COPY_AND_ASSIGN(AgentRunner);
};

}  // namespace modular

#endif  // PERIDOT_BIN_SESSIONMGR_AGENT_RUNNER_AGENT_RUNNER_H_
