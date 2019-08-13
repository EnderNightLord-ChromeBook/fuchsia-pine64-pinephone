// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::connect::*;
use failure::Fail;
use fidl_fuchsia_cobalt::{
    Status as CobaltStatus, SystemDataUpdaterMarker, SystemDataUpdaterProxy,
};
use fidl_fuchsia_pkg_rewrite::EngineMarker;
use fuchsia_async as fasync;
use fuchsia_syslog::{fx_log_err, fx_log_info, fx_log_warn};
use fuchsia_url::pkg_url::PkgUrl;
use fuchsia_zircon as zx;
use serde_derive::{Deserialize, Serialize};
use serde_json;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::Duration;

pub struct CurrentChannelNotifier<S = ServiceConnector> {
    service_connector: S,
    current_channel: String,
}

impl<S: ServiceConnect> CurrentChannelNotifier<S> {
    pub fn new(service_connector: S, dir: impl AsRef<Path>) -> Self {
        let current_channel = read_current_channel(dir.as_ref()).unwrap_or_else(|err| {
            fx_log_err!(
                "Error reading current_channel, defaulting to the empty string. This is expected before the first OTA. {}",
                err
            );
            String::new()
        });

        CurrentChannelNotifier { service_connector, current_channel }
    }

    pub async fn run(self) {
        loop {
            let cobalt = self.connect().await;

            fx_log_info!("calling cobalt.SetChannel(\"{}\")", self.current_channel);

            match cobalt.set_channel(&self.current_channel).await {
                Ok(CobaltStatus::Ok) => {
                    break;
                }
                Ok(CobaltStatus::EventTooBig) => {
                    fx_log_warn!("cobalt.SetChannel returned Status.EVENT_TOO_BIG, retrying");
                    self.sleep().await;
                }
                Ok(status) => {
                    // Not much we can do about the other status codes but log.
                    fx_log_err!("cobalt.SetChannel returned non-OK status: {:?}", status);
                    break;
                }
                Err(err) => {
                    // channel broken, so log the error and reconnect.
                    fx_log_warn!("cobalt.SetChannel returned error: {}, retrying", err);
                    self.sleep().await;
                }
            }
        }
    }

    async fn connect(&self) -> SystemDataUpdaterProxy {
        loop {
            match self.service_connector.connect_to_service::<SystemDataUpdaterMarker>() {
                Ok(cobalt) => {
                    return cobalt;
                }
                Err(err) => {
                    fx_log_err!("error connecting to cobalt: {}", err);
                    self.sleep().await
                }
            }
        }
    }

    async fn sleep(&self) {
        let delay = fasync::Time::after(Duration::from_secs(5).into());
        fasync::Timer::new(delay).await;
    }
}

pub struct TargetChannelManager<S = ServiceConnector> {
    service_connector: S,
    path: PathBuf,
    target_channel: Option<String>,
}

impl<S: ServiceConnect> TargetChannelManager<S> {
    pub fn new(service_connector: S, dir: impl Into<PathBuf>) -> Self {
        let mut path = dir.into();
        path.push("target_channel.json");
        let target_channel = read_channel(&path).ok();

        Self { service_connector, path, target_channel }
    }

    pub async fn update(&mut self) -> Result<(), failure::Error> {
        let target_channel = self.lookup_target_channel().await?;
        if self.target_channel.as_ref().map_or(false, |c| c == &target_channel) {
            return Ok(());
        }

        write_channel(&self.path, target_channel.clone())?;
        self.target_channel = Some(target_channel);
        Ok(())
    }

    async fn lookup_target_channel(&self) -> Result<String, failure::Error> {
        let rewrite_engine = self.service_connector.connect_to_service::<EngineMarker>()?;
        let rewritten: PkgUrl = rewrite_engine
            .test_apply("fuchsia-pkg://fuchsia.com/update/0")
            .await?
            .map_err(|s| zx::Status::from_raw(s))?
            .parse()?;
        let channel = host_to_channel(rewritten.host());

        Ok(channel.to_owned())
    }
}

#[derive(Serialize, Deserialize)]
#[serde(tag = "version", content = "content", deny_unknown_fields)]
enum Channel {
    #[serde(rename = "1")]
    Version1 { legacy_amber_source_name: String },
}

fn read_current_channel(p: &Path) -> Result<String, Error> {
    read_channel(p.join("current_channel.json"))
}

fn read_channel(path: impl AsRef<Path>) -> Result<String, Error> {
    let f = fs::File::open(path.as_ref())?;
    match serde_json::from_reader(f)? {
        Channel::Version1 { legacy_amber_source_name } => Ok(legacy_amber_source_name),
    }
}

fn write_channel(path: impl AsRef<Path>, channel: impl Into<String>) -> Result<(), io::Error> {
    let path = path.as_ref();
    let channel = Channel::Version1 { legacy_amber_source_name: channel.into() };

    let mut temp_path = path.to_owned().into_os_string();
    temp_path.push(".new");
    let temp_path = PathBuf::from(temp_path);
    {
        if let Some(dir) = temp_path.parent() {
            fs::create_dir_all(dir)?;
        }
        let f = fs::File::create(&temp_path)?;
        serde_json::to_writer(f, &channel)?;
    };
    fs::rename(temp_path, path)
}

#[derive(Debug, Fail)]
enum Error {
    #[fail(display = "io error: {}", _0)]
    Io(#[cause] io::Error),

    #[fail(display = "json error: {}", _0)]
    Json(#[cause] serde_json::Error),
}

impl From<io::Error> for Error {
    fn from(err: io::Error) -> Self {
        Error::Io(err)
    }
}

impl From<serde_json::Error> for Error {
    fn from(err: serde_json::Error) -> Self {
        Error::Json(err)
    }
}

fn host_to_channel(host: &str) -> &str {
    if let Some(n) = host.rfind(".fuchsia.com") {
        let (prefix, _) = host.split_at(n);
        prefix.split('.').nth(1).unwrap_or(host)
    } else {
        host
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl::endpoints::{DiscoverableService, RequestStream};
    use fidl_fuchsia_cobalt::{SystemDataUpdaterRequest, SystemDataUpdaterRequestStream};
    use fidl_fuchsia_pkg_rewrite::{EngineRequest, EngineRequestStream};
    use fuchsia_async::DurationExt;
    use fuchsia_component::server::ServiceFs;
    use fuchsia_zircon::DurationNum;
    use futures::prelude::*;
    use futures::task::Poll;
    use matches::assert_matches;
    use parking_lot::Mutex;
    use serde_json::{json, Value};
    use std::sync::Arc;
    use tempfile;

    #[test]
    fn test_host_to_channel_identities() {
        for s in vec![
            "devhost",
            "fuchsia.com",
            "example.com",
            "test.fuchsia.com",
            "test.example.com",
            "a.b-c.d.example.com",
        ] {
            assert_eq!(host_to_channel(s), s);
        }
    }

    #[test]
    fn test_host_to_channel_extracts_proper_subdomain() {
        assert_eq!(host_to_channel("a.b-c.d.fuchsia.com"), "b-c");
    }

    #[test]
    fn test_read_current_channel() {
        let dir = tempfile::tempdir().unwrap();

        fs::write(
            dir.path().join("current_channel.json"),
            r#"{"version":"1","content":{"legacy_amber_source_name":"stable"}}"#,
        )
        .unwrap();

        assert_matches!(read_current_channel(dir.path()), Ok(ref channel) if channel == "stable");
    }

    #[test]
    fn test_write_channel() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("channel.json");

        assert_matches!(write_channel(&path, "test"), Ok(()));

        let f = fs::File::open(path).expect("file to exist");
        let value: Value = serde_json::from_reader(f).expect("valid json");
        assert_eq!(
            value,
            json!({
                "version": "1",
                "content": {
                    "legacy_amber_source_name": "test",
                }
            })
        );
    }

    #[test]
    fn test_write_channel_create_subdir() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("subdir").join("channel.json");

        assert_matches!(write_channel(&path, "test"), Ok(()));

        let f = fs::File::open(path).expect("file to exist");
        let value: Value = serde_json::from_reader(f).expect("valid json");
        assert_eq!(
            value,
            json!({
                "version": "1",
                "content": {
                    "legacy_amber_source_name": "test",
                }
            })
        );
    }

    #[test]
    fn test_read_current_channel_rejects_invalid_json() {
        let dir = tempfile::tempdir().unwrap();

        fs::write(dir.path().join("current_channel.json"), "no channel here").unwrap();

        assert_matches!(read_current_channel(dir.path()), Err(Error::Json(_)));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_current_channel_notifier() {
        let dir = tempfile::tempdir().unwrap();
        let current_channel_path = dir.path().join("current_channel.json");

        fs::write(
            &current_channel_path,
            r#"{"version":"1","content":{"legacy_amber_source_name":"stable"}}"#,
        )
        .unwrap();

        let (connector, svc_dir) =
            NamespacedServiceConnector::bind("/test/current_channel_notifier/svc")
                .expect("ns to bind");
        let c = CurrentChannelNotifier::new(connector, dir.path());

        let mut fs = ServiceFs::new_local();
        let channel = Arc::new(Mutex::new(None));
        let chan = channel.clone();

        fs.add_fidl_service(move |mut stream: SystemDataUpdaterRequestStream| {
            let chan = chan.clone();

            fasync::spawn_local(async move {
                while let Some(req) = stream.try_next().await.unwrap_or(None) {
                    match req {
                        SystemDataUpdaterRequest::SetChannel { current_channel, responder } => {
                            *chan.lock() = Some(current_channel);
                            responder.send(CobaltStatus::Ok).unwrap();
                        }
                        _ => unreachable!(),
                    }
                }
            })
        })
        .serve_connection(svc_dir)
        .expect("serve_connection");

        fasync::spawn_local(fs.collect());

        c.run().await;

        assert_eq!(channel.lock().as_ref().map(|s| s.as_str()), Some("stable"));
    }

    #[test]
    fn test_current_channel_notifier_retries() {
        let mut executor = fasync::Executor::new_with_fake_time().unwrap();

        let dir = tempfile::tempdir().unwrap();
        let current_channel_path = dir.path().join("current_channel.json");

        write_channel(&current_channel_path, "stable").unwrap();

        #[derive(Debug, Clone)]
        enum FlakeMode {
            ErrorOnConnect,
            DropConnection,
            StatusOnCall(CobaltStatus),
        }

        #[derive(Debug, Clone)]
        struct State {
            mode: Option<FlakeMode>,
            channel: Option<String>,
            connect_count: u64,
            call_count: u64,
        }

        #[derive(Clone, Debug)]
        struct FlakeyServiceConnector {
            state: Arc<Mutex<State>>,
        };

        impl FlakeyServiceConnector {
            fn new() -> Self {
                Self {
                    state: Arc::new(Mutex::new(State {
                        mode: Some(FlakeMode::ErrorOnConnect),
                        channel: None,
                        connect_count: 0,
                        call_count: 0,
                    })),
                }
            }
            fn set_flake_mode(&self, mode: impl Into<Option<FlakeMode>>) {
                self.state.lock().mode = mode.into();
            }
            fn channel(&self) -> Option<String> {
                self.state.lock().channel.clone()
            }
            fn connect_count(&self) -> u64 {
                self.state.lock().connect_count
            }
            fn call_count(&self) -> u64 {
                self.state.lock().call_count
            }
        }

        impl ServiceConnect for FlakeyServiceConnector {
            fn connect_to_service<S: DiscoverableService>(
                &self,
            ) -> Result<S::Proxy, failure::Error> {
                assert_eq!(S::SERVICE_NAME, SystemDataUpdaterMarker::SERVICE_NAME);
                self.state.lock().connect_count += 1;
                match self.state.lock().mode {
                    Some(FlakeMode::ErrorOnConnect) => failure::bail!("test error on connect"),
                    Some(FlakeMode::DropConnection) => {
                        let (proxy, _stream) = fidl::endpoints::create_proxy::<S>().unwrap();
                        Ok(proxy)
                    }
                    Some(FlakeMode::StatusOnCall(status)) => {
                        let (proxy, stream) =
                            fidl::endpoints::create_proxy_and_stream::<S>().unwrap();
                        let mut stream: SystemDataUpdaterRequestStream = stream.cast_stream();

                        let state = self.state.clone();
                        fasync::spawn_local(async move {
                            while let Some(req) = stream.try_next().await.unwrap() {
                                match req {
                                    SystemDataUpdaterRequest::SetChannel {
                                        current_channel: _current_channel,
                                        responder,
                                    } => {
                                        state.lock().call_count += 1;
                                        responder.send(status).unwrap();
                                    }
                                    _ => unreachable!(),
                                }
                            }
                        });
                        Ok(proxy)
                    }
                    None => {
                        let (proxy, stream) =
                            fidl::endpoints::create_proxy_and_stream::<S>().unwrap();
                        let mut stream: SystemDataUpdaterRequestStream = stream.cast_stream();

                        let state = self.state.clone();
                        fasync::spawn_local(async move {
                            while let Some(req) = stream.try_next().await.unwrap() {
                                match req {
                                    SystemDataUpdaterRequest::SetChannel {
                                        current_channel,
                                        responder,
                                    } => {
                                        state.lock().call_count += 1;
                                        state.lock().channel = Some(current_channel);
                                        responder.send(CobaltStatus::Ok).unwrap();
                                    }
                                    _ => unreachable!(),
                                }
                            }
                        });
                        Ok(proxy)
                    }
                }
            }
        }

        let connector = FlakeyServiceConnector::new();
        let c = CurrentChannelNotifier::new(connector.clone(), dir.path());
        let mut task = c.run().boxed();
        assert_eq!(executor.run_until_stalled(&mut task), Poll::Pending);

        // Retries if connecting fails
        assert_eq!(executor.wake_expired_timers(), false);
        executor.set_fake_time(5.seconds().after_now());
        assert_eq!(executor.wake_expired_timers(), true);
        assert_eq!(executor.run_until_stalled(&mut task), Poll::Pending);
        assert_eq!(connector.connect_count(), 2);

        // Retries if a fidl error occurs during the request
        connector.set_flake_mode(FlakeMode::DropConnection);
        executor.set_fake_time(5.seconds().after_now());
        assert_eq!(executor.wake_expired_timers(), true);
        assert_eq!(executor.run_until_stalled(&mut task), Poll::Pending);
        assert_eq!(connector.connect_count(), 3);

        // Retries on expected Cobalt error status codes
        connector.set_flake_mode(FlakeMode::StatusOnCall(CobaltStatus::EventTooBig));
        executor.set_fake_time(5.seconds().after_now());
        assert_eq!(connector.call_count(), 0);
        assert_eq!(executor.wake_expired_timers(), true);
        assert_eq!(executor.run_until_stalled(&mut task), Poll::Pending);
        assert_eq!(connector.connect_count(), 4);
        assert_eq!(connector.call_count(), 1);

        // Stops trying when it eventually succeeds
        connector.set_flake_mode(None);
        executor.set_fake_time(5.seconds().after_now());
        assert_eq!(executor.wake_expired_timers(), true);
        assert_eq!(connector.channel(), None);
        assert_eq!(executor.run_until_stalled(&mut task), Poll::Ready(()));
        assert_eq!(connector.connect_count(), 5);
        assert_eq!(connector.call_count(), 2);
        assert_eq!(connector.channel(), Some("stable".to_owned()));

        // Bails out if Cobalt responds with an unexpected status code
        let connector = FlakeyServiceConnector::new();
        let c = CurrentChannelNotifier::new(connector.clone(), dir.path());
        let mut task = c.run().boxed();
        connector.set_flake_mode(FlakeMode::StatusOnCall(CobaltStatus::InvalidArguments));
        assert_eq!(executor.run_until_stalled(&mut task), Poll::Ready(()));
        assert_eq!(connector.connect_count(), 1);
        assert_eq!(connector.call_count(), 1);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_channel_manager_writes_channel() {
        let dir = tempfile::tempdir().unwrap();
        let target_channel_path = dir.path().join("target_channel.json");

        let connector = RewriteServiceConnector::new("fuchsia-pkg://devhost/update/0");
        let mut channel_manager = TargetChannelManager::new(connector.clone(), dir.path());

        // First write of the file with the correct data.
        assert_matches!(read_channel(&target_channel_path), Err(_));
        channel_manager.update().await.expect("channel update to succeed");
        assert_eq!(read_channel(&target_channel_path).unwrap(), "devhost");

        // If the file changes while the service is running, an update doesn't know to replace it.
        write_channel(&target_channel_path, "unique").unwrap();
        channel_manager.update().await.expect("channel update to succeed");
        assert_eq!(read_channel(&target_channel_path).unwrap(), "unique");

        // If the update package changes, however, the file will be updated.
        connector.set("fuchsia-pkg://hello.world.fuchsia.com/update/0");
        channel_manager.update().await.expect("channel update to succeed");
        assert_eq!(read_channel(&target_channel_path).unwrap(), "world");
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_target_channel_manager_recovers_from_corrupt_data() {
        let dir = tempfile::tempdir().unwrap();
        let target_channel_path = dir.path().join("target_channel.json");

        fs::write(&target_channel_path, r#"invalid json"#).unwrap();

        let connector = RewriteServiceConnector::new("fuchsia-pkg://a.b.c.fuchsia.com/update/0");
        let mut channel_manager = TargetChannelManager::new(connector, dir.path());

        assert!(read_channel(&target_channel_path).is_err());
        channel_manager.update().await.expect("channel update to succeed");
        assert_eq!(read_channel(&target_channel_path).unwrap(), "b");
    }

    #[derive(Clone)]
    struct RewriteServiceConnector {
        target: Arc<Mutex<String>>,
    }

    impl RewriteServiceConnector {
        fn new(target: impl Into<String>) -> Self {
            Self { target: Arc::new(Mutex::new(target.into())) }
        }
        fn set(&self, target: impl Into<String>) {
            *self.target.lock() = target.into();
        }
    }

    impl ServiceConnect for RewriteServiceConnector {
        fn connect_to_service<S: DiscoverableService>(&self) -> Result<S::Proxy, failure::Error> {
            let (proxy, stream) = fidl::endpoints::create_proxy_and_stream::<S>().unwrap();
            assert_eq!(S::SERVICE_NAME, EngineMarker::SERVICE_NAME);
            let mut stream: EngineRequestStream = stream.cast_stream();

            let target = self.target.lock().clone();
            fasync::spawn_local(async move {
                while let Some(req) = stream.try_next().await.unwrap() {
                    match req {
                        EngineRequest::TestApply { url, responder } => {
                            assert_eq!(url, "fuchsia-pkg://fuchsia.com/update/0");

                            responder.send(&mut Ok(target.clone())).unwrap();
                        }
                        _ => unreachable!(),
                    }
                }
            });
            Ok(proxy)
        }
    }
}
