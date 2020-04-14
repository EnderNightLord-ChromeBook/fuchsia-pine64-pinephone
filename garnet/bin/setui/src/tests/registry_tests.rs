// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::internal::core::{create_message_hub, Address, Payload};
use crate::internal::handler::{
    create_message_hub as create_handler_message_hub, reply, Address as HandlerAddress,
    MessengerClient, MessengerFactory, Payload as HandlerPayload, Receptor,
};
use crate::message::base::{Audience, MessageEvent, MessengerType};
use crate::message::receptor::Receptor as BaseReceptor;
use crate::registry::base::{Command, HandlerId, SettingHandlerFactory, State};
use crate::registry::registry_impl::RegistryImpl;
use crate::switchboard::base::{
    SettingAction, SettingActionData, SettingEvent, SettingRequest, SettingResponseResult,
    SettingType, SwitchboardError,
};

use async_trait::async_trait;
use fuchsia_async as fasync;
use futures::channel::mpsc::UnboundedSender;
use futures::lock::Mutex;
use futures::StreamExt;
use std::collections::HashMap;
use std::sync::Arc;

pub type SwitchboardReceptor = BaseReceptor<Payload, Address>;

struct SettingHandler {
    setting_type: SettingType,
    messenger: MessengerClient,
    state_tx: UnboundedSender<State>,
    next_response: Option<(SettingRequest, SettingResponseResult)>,
}

impl SettingHandler {
    fn process_state(&mut self, state: State) {
        self.state_tx.unbounded_send(state).ok();
    }

    pub fn set_next_response(&mut self, request: SettingRequest, response: SettingResponseResult) {
        self.next_response = Some((request, response));
    }

    pub fn notify(&self) {
        self.messenger
            .message(
                HandlerPayload::Changed(self.setting_type),
                Audience::Address(HandlerAddress::Registry),
            )
            .send()
            .ack();
    }

    fn process_request(&mut self, request: SettingRequest) -> SettingResponseResult {
        if let Some((match_request, result)) = self.next_response.take() {
            if request == match_request {
                return result;
            }
        }

        Err(SwitchboardError::UnimplementedRequest {
            setting_type: self.setting_type,
            request: request,
        })
    }

    fn create(
        messenger: MessengerClient,
        mut receptor: Receptor,
        setting_type: SettingType,
        state_tx: UnboundedSender<State>,
    ) -> Arc<Mutex<Self>> {
        let handler = Arc::new(Mutex::new(Self {
            messenger: messenger,
            setting_type: setting_type,
            state_tx: state_tx,
            next_response: None,
        }));

        let handler_clone = handler.clone();
        fasync::spawn(async move {
            while let Ok(event) = receptor.watch().await {
                match event {
                    MessageEvent::Message(
                        HandlerPayload::Command(Command::HandleRequest(request)),
                        client,
                    ) => {
                        reply(client, handler_clone.lock().await.process_request(request));
                    }
                    MessageEvent::Message(
                        HandlerPayload::Command(Command::ChangeState(state)),
                        _,
                    ) => {
                        handler_clone.lock().await.process_state(state);
                    }
                    _ => {}
                }
            }
        });

        handler
    }
}

struct FakeFactory {
    handlers: HashMap<SettingType, HandlerId>,
    request_counts: HashMap<SettingType, u64>,
    messenger_factory: MessengerFactory,
    next_id: HandlerId,
}

impl FakeFactory {
    pub fn new(messenger_factory: MessengerFactory) -> Self {
        FakeFactory {
            handlers: HashMap::new(),
            request_counts: HashMap::new(),
            messenger_factory: messenger_factory,
            next_id: 0,
        }
    }

    pub async fn create(&mut self, setting_type: SettingType) -> (MessengerClient, Receptor) {
        let messenger_result = self
            .messenger_factory
            .create(MessengerType::Addressable(HandlerAddress::Handler(self.next_id)))
            .await;
        self.handlers.insert(setting_type, self.next_id);
        self.next_id += 1;

        messenger_result.unwrap()
    }

    pub fn get_request_count(&mut self, setting_type: SettingType) -> u64 {
        if let Some(count) = self.request_counts.get(&setting_type) {
            *count
        } else {
            0
        }
    }
}

#[async_trait]
impl SettingHandlerFactory for FakeFactory {
    async fn generate(
        &mut self,
        setting_type: SettingType,
        _: MessengerFactory,
    ) -> Option<HandlerId> {
        let existing_count = self.get_request_count(setting_type);

        if let Some(handler_id) = self.handlers.get(&setting_type) {
            self.request_counts.insert(setting_type, existing_count + 1);
            return Some(*handler_id);
        } else {
            return None;
        }
    }
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_notify() {
    let messenger_factory = create_message_hub();
    let handler_messenger_factory = create_handler_message_hub();

    let handler_factory = Arc::new(Mutex::new(FakeFactory::new(handler_messenger_factory.clone())));
    let _registry = RegistryImpl::create(
        handler_factory.clone(),
        messenger_factory.clone(),
        handler_messenger_factory,
    )
    .await;
    let setting_type = SettingType::Unknown;
    let (messenger_client, mut receptor) =
        messenger_factory.create(MessengerType::Addressable(Address::Switchboard)).await.unwrap();

    let (handler_messenger, handler_receptor) =
        handler_factory.lock().await.create(setting_type).await;
    let (state_tx, mut state_rx) = futures::channel::mpsc::unbounded::<State>();
    let handler =
        SettingHandler::create(handler_messenger, handler_receptor, setting_type, state_tx);

    // Send a listen state and make sure sink is notified.
    {
        messenger_client
            .message(
                Payload::Action(SettingAction {
                    id: 1,
                    setting_type: setting_type,
                    data: SettingActionData::Listen(1),
                }),
                Audience::Address(Address::Registry),
            )
            .send()
            .ack();

        if let Some(state) = state_rx.next().await {
            assert_eq!(state, State::Listen);
        } else {
            panic!("should have received state update");
        }

        handler.lock().await.notify();

        while let Ok(event) = receptor.watch().await {
            if let MessageEvent::Message(Payload::Event(SettingEvent::Changed(changed_type)), _) =
                event
            {
                assert_eq!(changed_type, setting_type);
                break;
            }
        }
    }

    // Send an end listen state and make sure sink is notified.
    {
        messenger_client
            .message(
                Payload::Action(SettingAction {
                    id: 1,
                    setting_type: setting_type,
                    data: SettingActionData::Listen(0),
                }),
                Audience::Address(Address::Registry),
            )
            .send()
            .ack();
    }

    if let Some(state) = state_rx.next().await {
        assert_eq!(state, State::EndListen);
    } else {
        panic!("should have received state update");
    }
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_request() {
    let messenger_factory = create_message_hub();
    let handler_messenger_factory = create_handler_message_hub();
    let handler_factory = Arc::new(Mutex::new(FakeFactory::new(handler_messenger_factory.clone())));

    let _registry = RegistryImpl::create(
        handler_factory.clone(),
        messenger_factory.clone(),
        handler_messenger_factory,
    )
    .await;
    let setting_type = SettingType::Unknown;
    let (messenger_client, _) =
        messenger_factory.create(MessengerType::Addressable(Address::Switchboard)).await.unwrap();

    let (handler_messenger, handler_receptor) =
        handler_factory.lock().await.create(setting_type).await;
    let (state_tx, _) = futures::channel::mpsc::unbounded::<State>();
    let handler =
        SettingHandler::create(handler_messenger, handler_receptor, setting_type, state_tx);
    let request_id = 42;

    handler.lock().await.set_next_response(SettingRequest::Get, Ok(None));

    // Send initial request.
    let mut receptor = messenger_client
        .message(
            Payload::Action(SettingAction {
                id: request_id,
                setting_type: setting_type,
                data: SettingActionData::Request(SettingRequest::Get),
            }),
            Audience::Address(Address::Registry),
        )
        .send();

    while let Ok(event) = receptor.watch().await {
        if let MessageEvent::Message(
            Payload::Event(SettingEvent::Response(response_id, response)),
            _,
        ) = event
        {
            assert_eq!(request_id, response_id);
            assert!(response.is_ok());
            assert_eq!(None, response.unwrap());
            return;
        }
    }
}

/// Ensures setting handler is only generated once.
#[fuchsia_async::run_until_stalled(test)]
async fn test_generation() {
    let messenger_factory = create_message_hub();
    let handler_messenger_factory = create_handler_message_hub();
    let handler_factory = Arc::new(Mutex::new(FakeFactory::new(handler_messenger_factory.clone())));

    let (messenger_client, _) =
        messenger_factory.create(MessengerType::Addressable(Address::Switchboard)).await.unwrap();
    let _registry = RegistryImpl::create(
        handler_factory.clone(),
        messenger_factory.clone(),
        handler_messenger_factory,
    )
    .await;
    let setting_type = SettingType::Unknown;
    let request_id = 42;

    let (handler_messenger, handler_receptor) =
        handler_factory.lock().await.create(setting_type).await;
    let (state_tx, _) = futures::channel::mpsc::unbounded::<State>();
    let _handler =
        SettingHandler::create(handler_messenger, handler_receptor, setting_type, state_tx);

    // Send initial request.
    let _ = get_response(
        messenger_client
            .message(
                Payload::Action(SettingAction {
                    id: request_id,
                    setting_type: setting_type,
                    data: SettingActionData::Request(SettingRequest::Get),
                }),
                Audience::Address(Address::Registry),
            )
            .send(),
    )
    .await;

    // Ensure the handler was only created once.
    assert_eq!(1, handler_factory.lock().await.get_request_count(setting_type));

    // Send followup request.
    let _ = get_response(
        messenger_client
            .message(
                Payload::Action(SettingAction {
                    id: request_id,
                    setting_type: setting_type,
                    data: SettingActionData::Request(SettingRequest::Get),
                }),
                Audience::Address(Address::Registry),
            )
            .send(),
    )
    .await;

    // Make sure no followup generation was invoked.
    assert_eq!(1, handler_factory.lock().await.get_request_count(setting_type));
}

async fn get_response(mut receptor: SwitchboardReceptor) -> Option<(u64, SettingResponseResult)> {
    while let Ok(event) = receptor.watch().await {
        if let MessageEvent::Message(
            Payload::Event(SettingEvent::Response(response_id, response)),
            _,
        ) = event
        {
            return Some((response_id, response));
        }
    }

    return None;
}
