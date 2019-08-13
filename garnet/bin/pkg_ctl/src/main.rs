// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {
    crate::args::{Command, RepoCommand, RuleCommand, RuleConfigInputType},
    failure::{self, format_err, Fail, ResultExt},
    fidl_fuchsia_pkg::{
        PackageCacheMarker, PackageResolverMarker, RepositoryManagerMarker, RepositoryManagerProxy,
        UpdatePolicy,
    },
    fidl_fuchsia_pkg_ext::RepositoryConfig,
    fidl_fuchsia_pkg_rewrite::{EditTransactionProxy, EngineMarker, EngineProxy},
    fidl_fuchsia_update as fidl_update, files_async, fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service,
    fuchsia_url_rewrite::{Rule as RewriteRule, RuleConfig},
    fuchsia_zircon as zx,
    futures::Future,
    serde_json,
    std::{
        convert::{TryFrom, TryInto},
        env,
        fs::File,
        process,
    },
};

mod args;
mod error;

fn main() -> Result<(), failure::Error> {
    // Ignore the first argument.
    let args = env::args().skip(1).collect::<Vec<_>>();
    let cmd = match args::parse_args(args.iter().map(|s| &**s)) {
        Ok(cmd) => cmd,
        Err(err) => {
            eprintln!("{}", err);
            process::exit(1);
        }
    };

    let mut executor = fasync::Executor::new().context("Error creating executor")?;

    let fut = async {
        match cmd {
            Command::Resolve { pkg_url, selectors } => {
                let resolver = connect_to_service::<PackageResolverMarker>()
                    .context("Failed to connect to resolver service")?;
                println!("resolving {} with the selectors {:?}", pkg_url, selectors);

                let (dir, dir_server_end) = fidl::endpoints::create_proxy()?;

                let res = resolver
                    .resolve(
                        &pkg_url,
                        &mut selectors.iter().map(|s| s.as_str()),
                        &mut UpdatePolicy { fetch_if_absent: true, allow_old_versions: true },
                        dir_server_end,
                    )
                    .await?;
                zx::Status::ok(res)?;

                let entries = files_async::readdir_recursive(&dir).await?;
                println!("package contents:");
                for entry in entries {
                    println!("/{:?}", entry);
                }

                Ok(())
            }
            Command::Open { meta_far_blob_id, selectors } => {
                let cache = connect_to_service::<PackageCacheMarker>()
                    .context("Failed to connect to cache service")?;
                println!("opening {} with the selectors {:?}", meta_far_blob_id, selectors);

                let (dir, dir_server_end) = fidl::endpoints::create_proxy()?;

                let res = cache
                    .open(
                        &mut meta_far_blob_id.into(),
                        &mut selectors.iter().map(|s| s.as_str()),
                        dir_server_end,
                    )
                    .await?;
                zx::Status::ok(res)?;

                let entries = files_async::readdir_recursive(&dir).await?;
                println!("package contents:");
                for entry in entries {
                    println!("/{:?}", entry);
                }

                Ok(())
            }
            Command::Repo(cmd) => {
                let repo_manager = connect_to_service::<RepositoryManagerMarker>()
                    .context("Failed to connect to resolver service")?;

                match cmd {
                    RepoCommand::Add { file } => {
                        let repo: RepositoryConfig = serde_json::from_reader(File::open(file)?)?;

                        let res = repo_manager.add(repo.into()).await?;
                        zx::Status::ok(res)?;

                        Ok(())
                    }

                    RepoCommand::Remove { repo_url } => {
                        let res = repo_manager.remove(&repo_url).await?;
                        zx::Status::ok(res)?;

                        Ok(())
                    }

                    RepoCommand::List => {
                        let repos = fetch_repos(repo_manager).await?;

                        let mut urls =
                            repos.into_iter().map(|r| r.repo_url().to_string()).collect::<Vec<_>>();
                        urls.sort_unstable();
                        urls.into_iter().for_each(|url| println!("{}", url));

                        Ok(())
                    }

                    RepoCommand::ListVerbose => {
                        let repos = fetch_repos(repo_manager).await?;

                        let s = serde_json::to_string_pretty(&repos).expect("valid json");
                        println!("{}", s);

                        Ok(())
                    }
                }
            }
            Command::Rule(cmd) => {
                let engine = connect_to_service::<EngineMarker>()
                    .context("Failed to connect to rewrite engine service")?;

                match cmd {
                    RuleCommand::List => {
                        let (iter, iter_server_end) = fidl::endpoints::create_proxy()?;
                        engine.list(iter_server_end)?;

                        let mut rules = Vec::new();
                        loop {
                            let more = iter.next().await?;
                            if more.is_empty() {
                                break;
                            }
                            rules.extend(more);
                        }
                        let rules = rules
                            .into_iter()
                            .map(|rule| rule.try_into())
                            .collect::<Result<Vec<RewriteRule>, _>>()?;

                        for rule in rules {
                            println!("{:#?}", rule);
                        }
                    }
                    RuleCommand::Clear => {
                        do_transaction(engine, |transaction| {
                            async move {
                                transaction.reset_all()?;
                                Ok(transaction)
                            }
                        })
                        .await?;
                    }
                    RuleCommand::Replace { input_type } => {
                        let RuleConfig::Version1(ref rules) = match input_type {
                            RuleConfigInputType::File { path } => {
                                serde_json::from_reader(File::open(path)?)?
                            }
                            RuleConfigInputType::Json { config } => config,
                        };

                        do_transaction(engine, |transaction| {
                            async move {
                                transaction.reset_all()?;
                                // add() inserts rules as highest priority, so iterate over our
                                // prioritized list of rules so they end up in the right order.
                                for rule in rules.iter().rev() {
                                    transaction.add(&mut rule.clone().into()).await?;
                                }
                                Ok(transaction)
                            }
                        })
                        .await?;
                    }
                }

                Ok(())
            }
            Command::Update => {
                let update = connect_to_service::<fidl_update::ManagerMarker>()
                    .context("Failed to connect to update manager service")?;
                match update
                    .check_now(
                        fidl_update::Options { initiator: Some(fidl_update::Initiator::User) },
                        None,
                    )
                    .await?
                {
                    fidl_update::CheckStartedResult::Throttled => {
                        Err(format_err!("Update check was throttled."))
                    }
                    fidl_update::CheckStartedResult::Started
                    | fidl_update::CheckStartedResult::InProgress => Ok(()),
                }
            }
        }
    };

    executor.run_singlethreaded(fut)
}

#[derive(Debug, Fail)]
enum EditTransactionError {
    #[fail(display = "internal fidl error: {}", _0)]
    Fidl(#[cause] fidl::Error),

    #[fail(display = "commit error: {}", _0)]
    CommitError(zx::Status),
}

impl From<fidl::Error> for EditTransactionError {
    fn from(x: fidl::Error) -> Self {
        EditTransactionError::Fidl(x)
    }
}

/// Perform a rewrite rule edit transaction, retrying as necessary if another edit transaction runs
/// concurrently.
///
/// The given callback `cb` should perform the needed edits to the state of the rewrite rules but
/// not attempt to `commit()` the transaction. `do_transaction` will internally attempt to commit
/// the transaction and trigger a retry if necessary.
async fn do_transaction<T, R>(engine: EngineProxy, cb: T) -> Result<(), EditTransactionError>
where
    T: Fn(EditTransactionProxy) -> R,
    R: Future<Output = Result<EditTransactionProxy, fidl::Error>>,
{
    // Make a reasonable effort to retry the edit after a concurrent edit, but don't retry forever.
    for _ in 0..100 {
        let (transaction, transaction_server_end) = fidl::endpoints::create_proxy()?;
        engine.start_edit_transaction(transaction_server_end)?;

        let transaction = cb(transaction).await?;

        let status = transaction.commit().await?;

        // Retry edit transaction on concurrent edit
        return match zx::Status::from_raw(status) {
            zx::Status::OK => Ok(()),
            zx::Status::UNAVAILABLE => {
                continue;
            }
            status => Err(EditTransactionError::CommitError(status)),
        };
    }

    Err(EditTransactionError::CommitError(zx::Status::UNAVAILABLE))
}

async fn fetch_repos(
    repo_manager: RepositoryManagerProxy,
) -> Result<Vec<RepositoryConfig>, failure::Error> {
    let (iter, server_end) = fidl::endpoints::create_proxy()?;
    repo_manager.list(server_end)?;
    let mut repos = vec![];

    loop {
        let chunk = iter.next().await?;
        if chunk.is_empty() {
            break;
        }
        repos.extend(chunk);
    }

    repos
        .into_iter()
        .map(|repo| RepositoryConfig::try_from(repo).map_err(|e| failure::Error::from(e)))
        .collect()
}
