// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]
use {
    self::SystemUpdaterInteraction::{BlobfsSync, Gc, PackageResolve, Paver, Reboot},
    anyhow::Error,
    cobalt_sw_delivery_registry as metrics,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::DirectoryMarker,
    fidl_fuchsia_paver as paver,
    fidl_fuchsia_pkg::{PackageResolverRequestStream, PackageResolverResolveResponder},
    fidl_fuchsia_sys::{LauncherProxy, TerminationReason},
    fuchsia_async as fasync,
    fuchsia_component::{
        client::AppBuilder,
        server::{NestedEnvironment, ServiceFs},
    },
    fuchsia_zircon::Status,
    futures::prelude::*,
    mock_paver::{MockPaverService, MockPaverServiceBuilder, PaverEvent},
    parking_lot::Mutex,
    pretty_assertions::assert_eq,
    serde_json::json,
    std::{
        collections::HashMap,
        fs::{self, create_dir, File},
        io::Write,
        path::{Path, PathBuf},
        sync::Arc,
    },
    tempfile::TempDir,
};

// A set of tags for interactions the system updater has with external services.
// We aren't tracking Cobalt interactions, since those may arrive out of order,
// and they are tested in individual tests which care about them specifically.
#[derive(Debug, PartialEq, Clone)]
enum SystemUpdaterInteraction {
    BlobfsSync,
    Gc,
    PackageResolve(String),
    Paver(PaverEvent),
    Reboot,
}

type SystemUpdaterInteractions = Arc<Mutex<Vec<SystemUpdaterInteraction>>>;

struct TestEnvBuilder {
    paver_service_builder: MockPaverServiceBuilder,
}

impl TestEnvBuilder {
    fn new() -> Self {
        TestEnvBuilder { paver_service_builder: MockPaverServiceBuilder::new() }
    }

    fn paver_service<F>(mut self, f: F) -> Self
    where
        F: FnOnce(MockPaverServiceBuilder) -> MockPaverServiceBuilder,
    {
        self.paver_service_builder = f(self.paver_service_builder);
        self
    }

    fn build(self) -> TestEnv {
        // A buffer to store all the interactions the system-updater has with external services.
        let interactions = Arc::new(Mutex::new(vec![]));

        // Set up the paver service to push events to our interactions buffer by overriding
        // call_hook and firmware_hook.
        let interactions_paver_clone = Arc::clone(&interactions);
        let paver_service = Arc::new(
            self.paver_service_builder
                .event_hook(move |event| {
                    interactions_paver_clone.lock().push(Paver(event.clone()));
                })
                .build(),
        );

        let mut fs = ServiceFs::new();
        fs.add_proxy_service::<fidl_fuchsia_logger::LogSinkMarker, _>();

        let resolver = Arc::new(MockResolverService::new(Arc::clone(&interactions)));
        let resolver_clone = resolver.clone();
        fs.add_fidl_service(move |stream: PackageResolverRequestStream| {
            let resolver_clone = resolver_clone.clone();
            fasync::spawn(
                resolver_clone
                    .run_resolver_service(stream)
                    .unwrap_or_else(|e| panic!("error running resolver service: {:?}", e)),
            )
        });
        let paver_service_clone = paver_service.clone();
        fs.add_fidl_service(move |stream| {
            let paver_service_clone = paver_service_clone.clone();
            fasync::spawn(
                paver_service_clone
                    .run_paver_service(stream)
                    .unwrap_or_else(|e| panic!("error running paver service: {:?}", e)),
            )
        });

        let reboot_service = Arc::new(MockRebootService::new(Arc::clone(&interactions)));
        let reboot_service_clone = reboot_service.clone();
        fs.add_fidl_service(move |stream| {
            let reboot_service_clone = reboot_service_clone.clone();
            fasync::spawn(
                reboot_service_clone
                    .run_reboot_service(stream)
                    .unwrap_or_else(|e| panic!("error running reboot service: {:?}", e)),
            )
        });
        let cache_service = Arc::new(MockCacheService::new(Arc::clone(&interactions)));
        let cache_service_clone = Arc::clone(&cache_service);
        fs.add_fidl_service(move |stream| {
            let cache_service_clone = Arc::clone(&cache_service_clone);
            fasync::spawn(
                cache_service_clone
                    .run_cache_service(stream)
                    .unwrap_or_else(|e| panic!("error running cache service: {:?}", e)),
            )
        });
        let logger_factory = Arc::new(MockLoggerFactory::new());
        let logger_factory_clone = logger_factory.clone();
        fs.add_fidl_service(move |stream| {
            let logger_factory_clone = logger_factory_clone.clone();
            fasync::spawn(
                logger_factory_clone
                    .run_logger_factory(stream)
                    .unwrap_or_else(|e| panic!("error running logger factory: {:?}", e)),
            )
        });
        let space_service = Arc::new(MockSpaceService::new(Arc::clone(&interactions)));
        let space_service_clone = space_service.clone();
        fs.add_fidl_service(move |stream| {
            let space_service_clone = space_service_clone.clone();
            fasync::spawn(
                space_service_clone
                    .run_space_service(stream)
                    .unwrap_or_else(|e| panic!("error running space service: {:?}", e)),
            )
        });
        let env = fs
            .create_salted_nested_environment("systemupdater_env")
            .expect("nested environment to create successfully");
        fasync::spawn(fs.collect());

        let test_dir = TempDir::new().expect("create test tempdir");

        let blobfs_path = test_dir.path().join("blob");
        create_dir(&blobfs_path).expect("create blob dir");

        let packages_path = test_dir.path().join("packages");
        create_dir(&packages_path).expect("create packages dir");

        let fake_path = test_dir.path().join("fake");
        create_dir(&fake_path).expect("create fake stimulus dir");

        let config_path = test_dir.path().join("config");
        create_dir(&config_path).expect("create config dir");

        let misc_path = test_dir.path().join("misc");
        create_dir(&misc_path).expect("create misc dir");

        TestEnv {
            env,
            resolver,
            _paver_service: paver_service,
            _reboot_service: reboot_service,
            cache_service,
            logger_factory,
            _space_service: space_service,
            _test_dir: test_dir,
            packages_path,
            blobfs_path,
            fake_path,
            config_path,
            misc_path,
            interactions,
        }
    }
}
struct TestEnv {
    env: NestedEnvironment,
    resolver: Arc<MockResolverService>,
    _paver_service: Arc<MockPaverService>,
    _reboot_service: Arc<MockRebootService>,
    cache_service: Arc<MockCacheService>,
    logger_factory: Arc<MockLoggerFactory>,
    _space_service: Arc<MockSpaceService>,
    _test_dir: TempDir,
    packages_path: PathBuf,
    blobfs_path: PathBuf,
    fake_path: PathBuf,
    config_path: PathBuf,
    misc_path: PathBuf,
    interactions: SystemUpdaterInteractions,
}

impl TestEnv {
    fn new() -> Self {
        Self::builder().build()
    }

    fn builder() -> TestEnvBuilder {
        TestEnvBuilder::new()
    }

    fn launcher(&self) -> &LauncherProxy {
        self.env.launcher()
    }

    fn take_interactions(&self) -> Vec<SystemUpdaterInteraction> {
        std::mem::replace(&mut *self.interactions.lock(), vec![])
    }

    /// Set the name of the board that system_updater is running on.
    fn set_board_name(&self, board: impl AsRef<str>) {
        // Create the /config/board-info directory.
        let build_info_dir = self.config_path.join("build-info");
        create_dir(&build_info_dir).expect("create build-info dir");

        // Write the "board" file into the directory.
        let mut file = File::create(build_info_dir.join("board")).expect("create board file");
        file.write_all(board.as_ref().as_bytes()).expect("write board file");
    }

    fn set_target_channel(&self, contents: impl AsRef<[u8]>) {
        let misc_ota_dir = self.misc_path.join("ota");
        fs::create_dir_all(&misc_ota_dir).unwrap();

        fs::write(misc_ota_dir.join("target_channel.json"), contents.as_ref()).unwrap();
    }

    fn verify_current_channel(&self, expected: Option<&[u8]>) {
        match fs::read(self.misc_path.join("ota/current_channel.json")) {
            Ok(bytes) => assert_eq!(bytes, expected.unwrap()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => assert_eq!(expected, None),
            Err(e) => panic!(e),
        }
    }

    fn register_package(&mut self, name: impl AsRef<str>, merkle: impl AsRef<str>) -> TestPackage {
        let name = name.as_ref();
        let merkle = merkle.as_ref();

        let root = self.packages_path.join(merkle);
        create_dir(&root).expect("package to not yet exist");

        self.resolver
            .mock_package_result(format!("fuchsia-pkg://fuchsia.com/{}", name), Ok(root.clone()));

        TestPackage { root }.add_file("meta", merkle)
    }

    async fn run_system_updater<'a>(
        &'a self,
        args: SystemUpdaterArgs<'a>,
    ) -> Result<(), fuchsia_component::client::OutputError> {
        let mut v = vec![
            "--initiator".to_string(),
            format!("{}", args.initiator),
            "--target".to_string(),
            format!("{}", args.target),
        ];

        if let Some(update) = args.update {
            v.append(&mut vec!["--update".to_string(), format!("{}", update)]);
        }
        if let Some(reboot) = args.reboot {
            v.append(&mut vec!["--reboot".to_string(), format!("{}", reboot)]);
        }
        if let Some(skip_recovery) = args.skip_recovery {
            v.append(&mut vec!["--skip-recovery".to_string(), format!("{}", skip_recovery)]);
        }

        self.run_system_updater_args(v).await
    }

    async fn run_system_updater_args<'a>(
        &'a self,
        args: impl IntoIterator<Item = impl Into<String>>,
    ) -> Result<(), fuchsia_component::client::OutputError> {
        let launcher = self.launcher();
        let blobfs_dir = File::open(&self.blobfs_path).expect("open blob dir");
        let fake_dir = File::open(&self.fake_path).expect("open fake stimulus dir");
        let config_dir = File::open(&self.config_path).expect("open config dir");
        let misc_dir = File::open(&self.misc_path).expect("open misc dir");

        let system_updater = AppBuilder::new(
            "fuchsia-pkg://fuchsia.com/system-updater-integration-tests#meta/system_updater_isolated.cmx",
        )
        .add_dir_to_namespace("/blob".to_string(), blobfs_dir)
        .expect("/blob to mount")
        .add_dir_to_namespace("/fake".to_string(), fake_dir)
        .expect("/fake to mount")
        .add_dir_to_namespace("/config".to_string(), config_dir)
        .expect("/config to mount")
        .add_dir_to_namespace("/misc".to_string(), misc_dir)
        .expect("/misc to mount")
        .args(args);

        let output = system_updater
            .output(launcher)
            .expect("system_updater to launch")
            .await
            .expect("no errors while waiting for exit");

        if !output.stdout.is_empty() {
            eprintln!("TEST: system updater stdout:\n{}", String::from_utf8_lossy(&output.stdout));
        }

        if !output.stderr.is_empty() {
            eprintln!("TEST: system updater stderr:\n{}", String::from_utf8_lossy(&output.stderr));
        }

        assert_eq!(output.exit_status.reason(), TerminationReason::Exited);
        output.ok()
    }
}

struct TestPackage {
    root: PathBuf,
}

impl TestPackage {
    fn add_file(self, path: impl AsRef<Path>, contents: impl AsRef<[u8]>) -> Self {
        std::fs::write(self.root.join(path), contents).expect("create fake package file");
        self
    }
}

struct SystemUpdaterArgs<'a> {
    initiator: &'a str,
    target: &'a str,
    update: Option<&'a str>,
    reboot: Option<bool>,
    skip_recovery: Option<bool>,
}

struct MockResolverService {
    expectations: Mutex<HashMap<String, Result<PathBuf, Status>>>,
    interactions: SystemUpdaterInteractions,
}

impl MockResolverService {
    fn new(interactions: SystemUpdaterInteractions) -> Self {
        Self { expectations: Mutex::new(HashMap::new()), interactions }
    }
    async fn run_resolver_service(
        self: Arc<Self>,
        mut stream: PackageResolverRequestStream,
    ) -> Result<(), Error> {
        while let Some(event) = stream.try_next().await? {
            match event {
                fidl_fuchsia_pkg::PackageResolverRequest::Resolve {
                    package_url,
                    selectors: _,
                    update_policy: _,
                    dir,
                    responder,
                } => self.handle_resolve(package_url, dir, responder).await?,
                fidl_fuchsia_pkg::PackageResolverRequest::GetHash {
                    package_url: _,
                    responder: _,
                } => panic!("GetHash not implemented"),
            }
        }
        Ok(())
    }
    async fn handle_resolve(
        &self,
        package_url: String,
        dir: ServerEnd<DirectoryMarker>,
        responder: PackageResolverResolveResponder,
    ) -> Result<(), Error> {
        eprintln!("TEST: Got resolve request for {:?}", package_url);

        let response = self
            .expectations
            .lock()
            .get(&package_url)
            .map(|entry| entry.clone())
            // Successfully resolve unexpected packages without serving a package dir. Log the
            // transaction so tests can decide if it was expected.
            .unwrap_or(Err(Status::OK));

        self.interactions.lock().push(PackageResolve(package_url));

        let response_status = match response {
            Ok(package_dir) => {
                // Open the package directory using the directory request given by the client
                // asking to resolve the package.
                fdio::service_connect(
                    package_dir.to_str().expect("path to str"),
                    dir.into_channel(),
                )
                .unwrap_or_else(|err| panic!("error connecting to tempdir {:?}", err));
                Status::OK
            }
            Err(status) => status,
        };
        responder.send(response_status.into_raw())?;
        Ok(())
    }

    fn mock_package_result(&self, url: impl Into<String>, response: Result<PathBuf, Status>) {
        self.expectations.lock().insert(url.into(), response);
    }
}

struct MockCacheService {
    sync_response: Mutex<Option<i32>>,
    interactions: SystemUpdaterInteractions,
}
impl MockCacheService {
    fn new(interactions: SystemUpdaterInteractions) -> Self {
        Self { sync_response: Mutex::new(None), interactions }
    }

    fn set_sync_response(&self, response: i32) {
        self.sync_response.lock().replace(response);
    }

    async fn run_cache_service(
        self: Arc<Self>,
        mut stream: fidl_fuchsia_pkg::PackageCacheRequestStream,
    ) -> Result<(), Error> {
        while let Some(event) = stream.try_next().await? {
            match event {
                fidl_fuchsia_pkg::PackageCacheRequest::Sync { responder } => {
                    self.interactions.lock().push(BlobfsSync);
                    responder.send(self.sync_response.lock().unwrap_or(Status::OK.into_raw()))?;
                }
                other => panic!("unsupported PackageCache request: {:?}", other),
            }
        }

        Ok(())
    }
}

struct MockRebootService {
    interactions: SystemUpdaterInteractions,
}
impl MockRebootService {
    fn new(interactions: SystemUpdaterInteractions) -> Self {
        Self { interactions }
    }

    async fn run_reboot_service(
        self: Arc<Self>,
        mut stream: fidl_fuchsia_hardware_power_statecontrol::AdminRequestStream,
    ) -> Result<(), Error> {
        while let Some(event) = stream.try_next().await? {
            match event {
                fidl_fuchsia_hardware_power_statecontrol::AdminRequest::Suspend2 {
                    request,
                    responder,
                } => {
                    assert_eq!(
                        request.state,
                        Some(fidl_fuchsia_hardware_power_statecontrol::SystemPowerState::Reboot)
                    );
                    assert_eq!(
                        request.reason,
                        Some(fidl_fuchsia_hardware_power_statecontrol::RebootReason::SystemUpdate)
                    );
                    eprintln!(
                        "TEST: Got reboot request with state {:?} and reason {:?}",
                        request.state, request.reason
                    );
                    self.interactions.lock().push(Reboot);
                    responder.send(&mut Ok(()))?;
                }
                _ => {
                    panic!("unhandled RebootService method {:?}", event);
                }
            }
        }
        Ok(())
    }
}

struct MockSpaceService {
    interactions: SystemUpdaterInteractions,
}
impl MockSpaceService {
    fn new(interactions: SystemUpdaterInteractions) -> Self {
        Self { interactions }
    }

    async fn run_space_service(
        self: Arc<Self>,
        mut stream: fidl_fuchsia_space::ManagerRequestStream,
    ) -> Result<(), Error> {
        while let Some(event) = stream.try_next().await? {
            let fidl_fuchsia_space::ManagerRequest::Gc { responder } = event;
            self.interactions.lock().push(Gc);
            responder.send(&mut Ok(()))?;
        }

        Ok(())
    }
}

#[derive(Clone)]
struct CustomEvent {
    metric_id: u32,
    values: Vec<fidl_fuchsia_cobalt::CustomEventValue>,
}

struct MockLogger {
    cobalt_events: Mutex<Vec<fidl_fuchsia_cobalt::CobaltEvent>>,
}

impl MockLogger {
    fn new() -> Self {
        Self { cobalt_events: Mutex::new(vec![]) }
    }

    async fn run_logger(
        self: Arc<Self>,
        mut stream: fidl_fuchsia_cobalt::LoggerRequestStream,
    ) -> Result<(), Error> {
        while let Some(event) = stream.try_next().await? {
            match event {
                fidl_fuchsia_cobalt::LoggerRequest::LogCobaltEvent { event, responder } => {
                    self.cobalt_events.lock().push(event);
                    responder.send(fidl_fuchsia_cobalt::Status::Ok)?;
                }
                _ => {
                    panic!("unhandled Logger method {:?}", event);
                }
            }
        }

        Ok(())
    }
}

struct MockLoggerFactory {
    loggers: Mutex<Vec<Arc<MockLogger>>>,
    broken: Mutex<bool>,
}

impl MockLoggerFactory {
    fn new() -> Self {
        Self { loggers: Mutex::new(vec![]), broken: Mutex::new(false) }
    }

    async fn run_logger_factory(
        self: Arc<Self>,
        mut stream: fidl_fuchsia_cobalt::LoggerFactoryRequestStream,
    ) -> Result<(), Error> {
        if *self.broken.lock() {
            eprintln!("TEST: This LoggerFactory is broken by order of the test.");
            // Drop the stream, closing the channel.
            return Ok(());
        }
        while let Some(event) = stream.try_next().await? {
            match event {
                fidl_fuchsia_cobalt::LoggerFactoryRequest::CreateLoggerFromProjectId {
                    project_id,
                    logger,
                    responder,
                } => {
                    eprintln!("TEST: Got CreateLogger request with project_id {:?}", project_id);
                    let mock_logger = Arc::new(MockLogger::new());
                    self.loggers.lock().push(mock_logger.clone());
                    fasync::spawn(
                        mock_logger
                            .run_logger(logger.into_stream()?)
                            .unwrap_or_else(|e| eprintln!("error while running Logger: {:?}", e)),
                    );
                    responder.send(fidl_fuchsia_cobalt::Status::Ok)?;
                }
                _ => {
                    panic!("unhandled LoggerFactory method: {:?}", event);
                }
            }
        }

        Ok(())
    }
}

#[derive(PartialEq, Eq, Debug)]
struct OtaMetrics {
    initiator: u32,
    phase: u32,
    status_code: u32,
    target: String,
    // TODO: support free_space_delta assertions
}

impl OtaMetrics {
    fn from_events(mut events: Vec<fidl_fuchsia_cobalt::CobaltEvent>) -> Self {
        events.sort_by_key(|e| e.metric_id);

        // expecting one of each event
        assert_eq!(
            events.iter().map(|e| e.metric_id).collect::<Vec<_>>(),
            vec![
                metrics::OTA_START_METRIC_ID,
                metrics::OTA_RESULT_ATTEMPTS_METRIC_ID,
                metrics::OTA_RESULT_DURATION_METRIC_ID,
                metrics::OTA_RESULT_FREE_SPACE_DELTA_METRIC_ID
            ]
        );

        // we just asserted that we have the exact 4 things we're expecting, so unwrap them
        let mut iter = events.into_iter();
        let start = iter.next().unwrap();
        let attempt = iter.next().unwrap();
        let duration = iter.next().unwrap();
        let free_space_delta = iter.next().unwrap();

        // Some basic sanity checks follow
        assert_eq!(
            attempt.payload,
            fidl_fuchsia_cobalt::EventPayload::EventCount(fidl_fuchsia_cobalt::CountEvent {
                period_duration_micros: 0,
                count: 1
            })
        );

        let fidl_fuchsia_cobalt::CobaltEvent { event_codes, component, .. } = attempt;

        // metric event_codes and component should line up across all 3 result metrics
        assert_eq!(&duration.event_codes, &event_codes);
        assert_eq!(&duration.component, &component);
        assert_eq!(&free_space_delta.event_codes, &event_codes);
        assert_eq!(&free_space_delta.component, &component);

        // OtaStart only has initiator and hour_of_day, so just check initiator.
        assert_eq!(start.event_codes[0], event_codes[0]);
        assert_eq!(&start.component, &component);

        let target = component.expect("a target update merkle");

        assert_eq!(event_codes.len(), 3);
        let initiator = event_codes[0];
        let phase = event_codes[1];
        let status_code = event_codes[2];

        match duration.payload {
            fidl_fuchsia_cobalt::EventPayload::ElapsedMicros(_time) => {
                // Ignore the value since timing is not predictable.
            }
            other => {
                panic!("unexpected duration payload {:?}", other);
            }
        }

        // Ignore this for now, since it's using a shared tempdir, the values
        // are not deterministic.
        let _free_space_delta = match free_space_delta.payload {
            fidl_fuchsia_cobalt::EventPayload::EventCount(fidl_fuchsia_cobalt::CountEvent {
                period_duration_micros: 0,
                count,
            }) => count,
            other => {
                panic!("unexpected free space delta payload {:?}", other);
            }
        };

        Self { initiator, phase, status_code, target }
    }
}

fn force_recovery_json() -> String {
    json!({
      "version": "1",
      "content": {
        "mode": "force-recovery",
      }
    })
    .to_string()
}

const SYSTEM_IMAGE_URL: &str = "fuchsia-pkg://fuchsia.com/system_image/0?hash=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296";
const UPDATE_PKG_URL: &str = "fuchsia-pkg://fuchsia.com/update";

fn resolved_urls(interactions: SystemUpdaterInteractions) -> Vec<String> {
    (*interactions.lock())
        .iter()
        .filter_map(|interaction| match interaction {
            PackageResolve(package_url) => Some(package_url.clone()),
            _ => None,
        })
        .collect()
}

#[fasync::run_singlethreaded(test)]
async fn test_system_update() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("run system_updater");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::SuccessPendingReboot as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Success as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_system_update_force_recovery() {
    let mut env = TestEnv::new();

    let package_url = SYSTEM_IMAGE_URL;
    env.register_package("update", "upd4t3")
        .add_file("packages", package_url)
        .add_file("update-mode", &force_recovery_json());

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("run system_updater");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::SuccessPendingReboot as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Success as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::SetConfigurationUnbootable {
                configuration: paver::Configuration::A
            }),
            Paver(PaverEvent::SetConfigurationUnbootable {
                configuration: paver::Configuration::B
            }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_packages_json_takes_precedence() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file(
            "packages.json",
            &json!({
              // TODO: Change to "1" once we remove support for versions as ints.
              "version": 1,
              "content": [
                "fuchsia-pkg://fuchsia.com/amber/0?hash=abcdef",
                "fuchsia-pkg://fuchsia.com/pkgfs/0?hash=123456789",
              ]
            })
            .to_string(),
        )
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("run system_updater");

    assert_eq!(
        resolved_urls(Arc::clone(&env.interactions)),
        vec![
            UPDATE_PKG_URL,
            "fuchsia-pkg://fuchsia.com/amber/0?hash=abcdef",
            "fuchsia-pkg://fuchsia.com/pkgfs/0?hash=123456789"
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_metrics_report_untrusted_tuf_repo() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages.json",
            &json!({
              // TODO: Change to "1" once we remove support for versions as ints.
              "version": 1,
              "content": [
                "fuchsia-pkg://non-existent-repo.com/amber/0?hash=abcdef",
                "fuchsia-pkg://fuchsia.com/pkgfs/0?hash=123456789",
              ]
            })
            .to_string(),
        )
        .add_file("zbi", "fake zbi");

    env.resolver.mock_package_result(
        "fuchsia-pkg://non-existent-repo.com/amber/0?hash=abcdef",
        Err(Status::ADDRESS_UNREACHABLE),
    );

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::PackageDownload as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::ErrorUntrustedTufRepo
                as u32,
            target: "m3rk13".into(),
        }
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_system_update_no_reboot() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: Some(false),
        skip_recovery: None,
    })
    .await
    .expect("run system_updater");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::SuccessPendingReboot as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Success as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_system_update_force_recovery_reboots_regardless_of_reboot_arg() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file("packages", "")
        .add_file("update-mode", &force_recovery_json());

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: Some(false),
        skip_recovery: None,
    })
    .await
    .expect("run system_updater");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::SetConfigurationUnbootable {
                configuration: paver::Configuration::A
            }),
            Paver(PaverEvent::SetConfigurationUnbootable {
                configuration: paver::Configuration::B
            }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_broken_logger() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    *env.logger_factory.broken.lock() = true;

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("run system_updater");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 0);

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_failing_package_fetch() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3").add_file(
        "packages",
        "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
    );

    let system_image_url = SYSTEM_IMAGE_URL;
    env.resolver.mock_package_result(system_image_url, Err(Status::NOT_FOUND));

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::PackageDownload as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Error as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(system_image_url.to_string()),
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_system_update_fails_when_sync_fails() {
    let mut env = TestEnv::new();
    env.cache_service.set_sync_response(Status::INTERNAL.into_raw());
    env.register_package("update", "upd4t3").add_file(
        "packages",
        "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
    );

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;

    assert!(result.is_err(), "system_updater succeeded when it should fail");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_normal_requires_zbi() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("bootloader", "new bootloader");

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_force_recovery_rejects_zbi() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("update-mode", &force_recovery_json())
        .add_file("bootloader", "new bootloader")
        .add_file("zbi", "fake zbi");

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    assert_eq!(
        env.take_interactions(),
        vec![Gc, PackageResolve(UPDATE_PKG_URL.to_string()), Gc, BlobfsSync,]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_validate_board() {
    let mut env = TestEnv::new();

    env.set_board_name("x64");

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("board", "x64")
        .add_file("zbi", "fake zbi")
        .add_file("bootloader", "new bootloader");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        resolved_urls(Arc::clone(&env.interactions)),
        vec![UPDATE_PKG_URL, SYSTEM_IMAGE_URL,]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_invalid_board() {
    let mut env = TestEnv::new();

    env.set_board_name("x64");

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("board", "arm")
        .add_file("zbi", "fake zbi")
        .add_file("bootloader", "new bootloader");

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    // Expect to have failed prior to downloading images.
    assert_eq!(resolved_urls(Arc::clone(&env.interactions)), vec![UPDATE_PKG_URL]);

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: 0u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Error as u32,
            target: "m3rk13".into(),
        }
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_writes_bootloader() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("bootloader", "new bootloader");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteFirmware {
                firmware_type: "".to_string(),
                payload: b"new bootloader".to_vec()
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_writes_recovery() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("zedboot", "new recovery");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::Recovery,
                asset: paver::Asset::Kernel,
                payload: b"new recovery".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

// TODO(52356): drop this duplicate test
#[fasync::run_singlethreaded(test)]
async fn test_writes_recovery_named_recovery() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("recovery", "new recovery");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::Recovery,
                asset: paver::Asset::Kernel,
                payload: b"new recovery".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_writes_recovery_vbmeta() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("zedboot", "new recovery")
        .add_file("recovery.vbmeta", "new recovery vbmeta");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::Recovery,
                asset: paver::Asset::Kernel,
                payload: b"new recovery".to_vec(),
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::Recovery,
                asset: paver::Asset::VerifiedBootMetadata,
                payload: b"new recovery vbmeta".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_writes_fuchsia_vbmeta() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("fuchsia.vbmeta", "fake zbi vbmeta");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::VerifiedBootMetadata,
                payload: b"fake zbi vbmeta".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_skips_recovery_vbmeta() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("zedboot", "new recovery")
        .add_file("recovery.vbmeta", "new recovery vbmeta");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: Some(true),
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

async fn do_test_working_image_write_with_abr(
    active_config: paver::Configuration,
    target_config: paver::Configuration,
) {
    let mut env =
        TestEnv::builder().paver_service(|builder| builder.active_config(active_config)).build();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake_zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::SuccessPendingReboot as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Success as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: target_config,
                asset: paver::Asset::Kernel,
                payload: b"fake_zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: target_config }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_working_image_write_with_abr_and_active_config_a() {
    do_test_working_image_write_with_abr(paver::Configuration::A, paver::Configuration::B).await
}

#[fasync::run_singlethreaded(test)]
async fn test_working_image_write_with_abr_and_active_config_b() {
    do_test_working_image_write_with_abr(paver::Configuration::B, paver::Configuration::A).await
}

#[fasync::run_singlethreaded(test)]
async fn test_working_image_with_unsupported_abr() {
    let mut env = TestEnv::builder()
        .paver_service(|builder| builder.boot_manager_close_with_epitaph(Status::NOT_SUPPORTED))
        .build();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake_zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::SuccessPendingReboot as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Success as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::A,
                asset: paver::Asset::Kernel,
                payload: b"fake_zbi".to_vec(),
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake_zbi".to_vec(),
            }),
            Paver(PaverEvent::DataSinkFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_failing_image_write() {
    let mut env =
        TestEnv::builder().paver_service(|builder| builder.call_hook(|_| Status::INTERNAL)).build();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake_zbi");

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    let loggers = env.logger_factory.loggers.lock().clone();
    assert_eq!(loggers.len(), 1);
    let logger = loggers.into_iter().next().unwrap();
    assert_eq!(
        OtaMetrics::from_events(logger.cobalt_events.lock().clone()),
        OtaMetrics {
            initiator: metrics::OtaResultAttemptsMetricDimensionInitiator::UserInitiatedCheck
                as u32,
            phase: metrics::OtaResultAttemptsMetricDimensionPhase::ImageWrite as u32,
            status_code: metrics::OtaResultAttemptsMetricDimensionStatusCode::Error as u32,
            target: "m3rk13".into(),
        }
    );

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_uses_custom_update_package() {
    let mut env = TestEnv::new();

    env.register_package("another-update/4", "upd4t3r")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: Some("fuchsia-pkg://fuchsia.com/another-update/4"),
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("run system_updater");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve("fuchsia-pkg://fuchsia.com/another-update/4".to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_requires_update_package() {
    let env = TestEnv::new();

    env.resolver.mock_package_result(UPDATE_PKG_URL, Err(Status::NOT_FOUND));

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    assert_eq!(env.take_interactions(), vec![Gc, PackageResolve(UPDATE_PKG_URL.to_string()),]);
}

#[fasync::run_singlethreaded(test)]
async fn test_rejects_invalid_update_package_url() {
    let env = TestEnv::new();

    let bogus_url = "not-fuchsia-pkg://fuchsia.com/not-a-update";

    env.resolver.mock_package_result(bogus_url, Err(Status::INVALID_ARGS));

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: Some(bogus_url),
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    assert_eq!(env.take_interactions(), vec![Gc, PackageResolve(bogus_url.to_string()),]);
}

#[fasync::run_singlethreaded(test)]
async fn test_rejects_unknown_flags() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    let result = env
        .run_system_updater_args(vec![
            "--initiator",
            "manual",
            "--target",
            "m3rk13",
            "--foo",
            "bar",
        ])
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");
    assert_eq!(env.take_interactions(), vec![]);
}

#[fasync::run_singlethreaded(test)]
async fn test_rejects_extra_args() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    let result = env
        .run_system_updater_args(vec!["--initiator", "manual", "--target", "m3rk13", "foo"])
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    assert_eq!(env.take_interactions(), vec![]);
}

#[fasync::run_singlethreaded(test)]
async fn test_writes_firmware() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("firmware", "fake firmware");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteFirmware {
                firmware_type: "".to_string(),
                payload: b"fake firmware".to_vec()
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_writes_multiple_firmware_types() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("firmware_a", "fake firmware A")
        .add_file("firmware_b", "fake firmware B");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    let mut interactions = env.take_interactions();
    // The order of files listed from a directory isn't guaranteed so the
    // firmware could be written in either order. Sort by type string so
    // we can easily validate contents.
    interactions[6..8].sort_by_key(|event| {
        if let Paver(PaverEvent::WriteFirmware { firmware_type, payload: _ }) = event {
            return firmware_type.clone();
        } else {
            panic!("Not a WriteFirmware event: {:?}", event);
        }
    });

    assert_eq!(
        interactions,
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteFirmware {
                firmware_type: "a".to_string(),
                payload: b"fake firmware A".to_vec()
            }),
            Paver(PaverEvent::WriteFirmware {
                firmware_type: "b".to_string(),
                payload: b"fake firmware B".to_vec()
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_unsupported_firmware_type() {
    let mut env = TestEnv::builder()
        .paver_service(|builder| {
            builder.firmware_hook(|_| paver::WriteFirmwareResult::UnsupportedType(true))
        })
        .build();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("firmware", "fake firmware");

    // Update should still succeed, we want to skip unsupported firmware types.
    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect("success");

    assert_eq!(
        env.take_interactions(),
        vec![
            Gc,
            PackageResolve(UPDATE_PKG_URL.to_string()),
            Gc,
            PackageResolve(SYSTEM_IMAGE_URL.to_string()),
            BlobfsSync,
            Paver(PaverEvent::QueryActiveConfiguration),
            Paver(PaverEvent::WriteFirmware {
                firmware_type: "".to_string(),
                payload: b"fake firmware".to_vec(),
            }),
            Paver(PaverEvent::WriteAsset {
                configuration: paver::Configuration::B,
                asset: paver::Asset::Kernel,
                payload: b"fake zbi".to_vec(),
            }),
            Paver(PaverEvent::SetConfigurationActive { configuration: paver::Configuration::B }),
            Paver(PaverEvent::DataSinkFlush),
            Paver(PaverEvent::BootManagerFlush),
            Reboot,
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_write_firmware_failure() {
    let mut env = TestEnv::builder()
        .paver_service(|builder| {
            builder
                .firmware_hook(|_| paver::WriteFirmwareResult::Status(Status::INTERNAL.into_raw()))
        })
        .build();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi")
        .add_file("firmware", "fake firmware");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .expect_err("update should fail");
}

#[fasync::run_singlethreaded(test)]
async fn promotes_target_channel_as_current_channel() {
    let mut env = TestEnv::new();

    env.set_target_channel("target-channel");

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .unwrap();

    env.verify_current_channel(Some(b"target-channel"));

    // even if current channel already exists.

    env.set_target_channel("target-channel-2");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .unwrap();

    env.verify_current_channel(Some(b"target-channel-2"));
}

#[fasync::run_singlethreaded(test)]
async fn succeeds_even_if_target_channel_does_not_exist() {
    let mut env = TestEnv::new();

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    env.run_system_updater(SystemUpdaterArgs {
        initiator: "manual",
        target: "m3rk13",
        update: None,
        reboot: None,
        skip_recovery: None,
    })
    .await
    .unwrap();

    env.verify_current_channel(None);
}

#[fasync::run_singlethreaded(test)]
async fn does_not_promote_target_channel_on_failure() {
    let mut env =
        TestEnv::builder().paver_service(|builder| builder.call_hook(|_| Status::INTERNAL)).build();

    env.set_target_channel("target-channel");

    env.register_package("update", "upd4t3")
        .add_file(
            "packages",
            "system_image/0=42ade6f4fd51636f70c68811228b4271ed52c4eb9a647305123b4f4d0741f296\n",
        )
        .add_file("zbi", "fake zbi");

    let result = env
        .run_system_updater(SystemUpdaterArgs {
            initiator: "manual",
            target: "m3rk13",
            update: None,
            reboot: None,
            skip_recovery: None,
        })
        .await;
    assert!(result.is_err(), "system_updater succeeded when it should fail");

    env.verify_current_channel(None);
}
