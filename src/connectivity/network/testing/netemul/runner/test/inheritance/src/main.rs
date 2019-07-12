// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]

use {
    failure::{format_err, Error, ResultExt},
    fidl_fuchsia_netemul_example::{CounterMarker, CounterRequest, CounterRequestStream},
    fidl_fuchsia_sys::{
        ComponentControllerEvent, ComponentControllerMarker, ComponentControllerProxy,
        EnvironmentControllerMarker, EnvironmentMarker, EnvironmentOptions, EnvironmentProxy,
        LaunchInfo, LauncherMarker, TerminationReason,
    },
    fuchsia_async as fasync,
    fuchsia_component::client,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{fx_log_err, fx_log_info},
    futures::prelude::*,
    std::sync::Arc,
    std::sync::Mutex,
    structopt::StructOpt,
};

const MY_PACKAGE: &str = "fuchsia-pkg://fuchsia.com/netemul_sandbox_test#meta/inheritance.cmx";

#[derive(StructOpt, Debug)]
enum Ops {
    #[structopt(name = "serve")]
    Serve,
    #[structopt(name = "inherit")]
    Inherit,
    #[structopt(name = "no-inherit")]
    NoInherit,
    #[structopt(name = "same-instance")]
    SameInstance { expect: u32 },
}

#[derive(StructOpt, Debug)]
struct Opt {
    #[structopt(subcommand)]
    mode: Option<Ops>,
}

async fn simple_increment(expect: u32) -> Result<(), Error> {
    let counter = client::connect_to_service::<CounterMarker>()?;

    let value = await!(counter.increment())?;
    if value != expect {
        return Err(format_err!("unexpected counter value {}, was expecting {}", value, expect));
    }
    return Ok(());
}

async fn wait_for_component(component: &ComponentControllerProxy) -> Result<(), Error> {
    let mut component_events = component.take_event_stream();
    // wait for child to exit and mimic the result code
    let result = loop {
        let event = await!(component_events.try_next())
            .context("wait for child component to exit")?
            .ok_or_else(|| format_err!("Child didn't exit cleanly"))?;

        match event {
            ComponentControllerEvent::OnTerminated {
                return_code: code,
                termination_reason: reason,
            } => {
                if code != 0 || reason != TerminationReason::Exited {
                    break Err(format_err!(
                        "Child exited with code {}, reason {}",
                        code,
                        reason as u32
                    ));
                } else {
                    break Ok(());
                }
            }
            _ => {
                continue;
            }
        }
    };
    result
}

async fn spawn_child_and_expect(env: EnvironmentProxy, expect: u32) -> Result<(), Error> {
    // connect to fuchsia.sys.Launcher:
    let (launcher, lch) = fidl::endpoints::create_proxy::<LauncherMarker>()?;
    let () = env.get_launcher(lch)?;

    let expect = format!("{}", expect);
    let mut linfo = LaunchInfo {
        url: String::from(MY_PACKAGE),
        arguments: Some(vec![String::from("same-instance"), String::from(expect)]),
        additional_services: None,
        directory_request: None,
        err: None,
        out: None,
        flat_namespace: None,
    };
    let (comp_controller, comp_controller_req) =
        fidl::endpoints::create_proxy::<ComponentControllerMarker>()?;
    let () = launcher.create_component(&mut linfo, Some(comp_controller_req))?;
    let () = await!(wait_for_component(&comp_controller))?;

    Ok(())
}

async fn run_root() -> Result<(), Error> {
    // root process should have a brand new instance of the counter service,
    // so we should expect the value to be 1 after a first increment:
    let () = await!(simple_increment(1))?;

    // not let's try to spawn a process in this environment
    // it should have access to exactly the same instance of the counter service,
    // hence we're tell it to expect the value "2" on the counter:

    let env = client::connect_to_service::<EnvironmentMarker>()?;
    let () = await!(spawn_child_and_expect(env, 2))?;

    // finally we can spawn another fidl.sys.Environment from the first one
    // with the option to inherit the parent services.
    // Then, from that nested environment, we can observe that the same instance is there
    // by expecting a new increment of the counter to be "3"
    let env = client::connect_to_service::<EnvironmentMarker>()?;
    let (child_env, child_env_req) = fidl::endpoints::create_proxy::<EnvironmentMarker>()?;
    let (_child_ctlr, child_ctlr_req) =
        fidl::endpoints::create_proxy::<EnvironmentControllerMarker>()?;

    let mut options = EnvironmentOptions {
        inherit_parent_services: true,
        allow_parent_runners: true,
        kill_on_oom: true,
        delete_storage_on_death: false,
    };
    let () = env.create_nested_environment(
        child_env_req,
        child_ctlr_req,
        "same-instance-child",
        None,
        &mut options,
    )?;
    let () = await!(spawn_child_and_expect(child_env, 3))?;

    Ok(())
}

async fn run_no_inherit() -> Result<(), Error> {
    await!(simple_increment(1))
        .expect_err("Shouldn't be able to use service due to inheritance setup");
    Ok(())
}

struct CounterData {
    value: u32,
}

fn spawn_counter_server(mut stream: CounterRequestStream, data: Arc<Mutex<CounterData>>) {
    fasync::spawn(
        async move {
            while let Some(CounterRequest::Increment { responder }) =
                await!(stream.try_next()).context("error running counter server")?
            {
                let mut d = data.lock().unwrap();
                d.value += 1;
                fx_log_info!("Incrementing counter to {}", d.value);
                responder.send(d.value).context("Error sending response")?;
            }
            Ok(())
        }
            .unwrap_or_else(|e: failure::Error| fx_log_err!("{:?}", e)),
    )
}

async fn run_server() -> Result<(), Error> {
    let data = Arc::new(Mutex::new(CounterData { value: 0 }));
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |chan| spawn_counter_server(chan, data.clone()));
    fs.take_and_serve_directory_handle()?;
    let () = await!(fs.collect());
    Ok(())
}

// check this test's .cmx file to see which options are run when.
// the point here is to verify and exemplify the inheritance behavior and interplay
// between fuchsia.netemul.environment.ManagedEnvironment and fuchsia.sys.Environment
// The "root" path has an instance of the Counter service.
// "inherit" and "no-inherit" paths demonstrate how *configuration* inheritance works:
// "inherit" will gain a brand new inherit instance (as seen in the cmx option to inherit the services)
// and "no-inherit" is just verifying that if inheritance is off, the service is not available.
// The "root" path also uses fuchsia.sys.Environment to show how the same *instance* of the Counter
// service can be passed to other processes.
// Observe that ManagedEnvironment will always be hermetic in terms of same instance of services
// inerited from the parent.
#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["inheritance"])?;
    fx_log_info!("Started");

    let opt = Opt::from_args();
    match opt.mode {
        None => await!(run_root()),
        Some(Ops::Inherit) => await!(simple_increment(1)),
        Some(Ops::Serve) => await!(run_server()),
        Some(Ops::NoInherit) => await!(run_no_inherit()),
        Some(Ops::SameInstance { expect }) => await!(simple_increment(expect)),
    }
}
