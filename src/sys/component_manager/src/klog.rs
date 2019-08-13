// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{Error, ResultExt},
    fuchsia_zircon as zx,
    lazy_static::lazy_static,
    std::fmt,
    std::panic,
};

const MAX_LOG_LEVEL: log::Level = log::Level::Info;
/// The largest message that can be written into a debuglog
const MAX_LOG_MSG_SIZE: usize = 224;

/// KernelLogger is a logger implementation for the log crate. It attempts to use the kernel
/// debuglog facility and automatically falls back to stderr if that fails.
pub struct KernelLogger {
    debuglog: zx::DebugLog,
}

lazy_static! {
    static ref LOGGER: KernelLogger = KernelLogger::new();
}

impl KernelLogger {
    fn new() -> KernelLogger {
        let debuglog = zx::DebugLog::create(zx::DebugLogOpts::empty())
            .context("Failed to create debuglog object")
            .unwrap();
        KernelLogger { debuglog }
    }

    /// Initialize the global logger to use KernelLogger.
    ///
    /// Also registers a panic hook that prints the panic payload to the logger before running the
    /// default panic hook.
    ///
    /// Returns an error if this or something else already called [log::set_logger()].
    pub fn init() -> Result<(), Error> {
        log::set_logger(&*LOGGER).context("Failed to set KernelLogger as global logger")?;
        log::set_max_level(MAX_LOG_LEVEL.to_level_filter());
        Self::install_panic_hook();
        Ok(())
    }

    /// Register a panic hook that prints the panic payload to the logger before running the
    /// default panic hook.
    pub fn install_panic_hook() {
        let default_hook = panic::take_hook();
        panic::set_hook(Box::new(move |panic_info| {
            // Handle common cases of &'static str or String payload.
            let msg = match panic_info.payload().downcast_ref::<&'static str>() {
                Some(s) => *s,
                None => match panic_info.payload().downcast_ref::<String>() {
                    Some(s) => &s[..],
                    None => "<Unknown panic payload type>",
                },
            };
            LOGGER.log_helper("PANIC", &format_args!("{}", msg));

            default_hook(panic_info);
        }));
    }

    fn log_helper(&self, level: &str, args: &fmt::Arguments) {
        let mut msg = format!("[component_manager] {}: {}", level, args);

        while msg.len() > 0 {
            // TODO(ZX-3187): zx_debuglog_write also accepts options and the possible options include
            // log levels, but they seem to be mostly unused and not displayed today, so we don't pass
            // along log level yet.
            if let Err(s) = self.debuglog.write(msg.as_bytes()) {
                eprintln!("failed to write log ({}): {}", s, msg);
            }
            let num_to_drain = std::cmp::min(msg.len(), MAX_LOG_MSG_SIZE);
            msg.drain(..num_to_drain);
        }
    }
}

impl log::Log for KernelLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= MAX_LOG_LEVEL
    }

    fn log(&self, record: &log::Record) {
        if self.enabled(record.metadata()) {
            self.log_helper(&record.level().to_string(), record.args());
        }
    }

    fn flush(&self) {}
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fuchsia_zircon::AsHandleRef,
        log::*,
        rand::Rng,
        std::{panic, sync::Once},
    };

    // KernelLogger::init/log::set_logger will fail if called more than once and there's no way to
    // reset.
    fn init_logger_once() {
        static INIT: Once = Once::new();
        INIT.call_once(|| {
            KernelLogger::init().unwrap();
        });
    }

    // expect_message_in_debuglog will read the last 10000 messages in zircon's debuglog, looking
    // for a message that equals `sent_msg`. If found, the function returns. If the first 10,000
    // messages doesn't contain `sent_msg`, it will panic.
    fn expect_message_in_debuglog(sent_msg: String) {
        let debuglog = zx::DebugLog::create(zx::DebugLogOpts::READABLE).unwrap();
        let mut record = Vec::with_capacity(zx::sys::ZX_LOG_RECORD_MAX);
        for _ in 0..10000 {
            match debuglog.read(&mut record) {
                Ok(()) => {
                    // TODO(ZX-3187): Manually unpack log record until zx::DebugLog::read returns
                    // an wrapper type.
                    let mut len_bytes = [0; 2];
                    len_bytes.copy_from_slice(&record[4..6]);
                    let data_len = u16::from_le_bytes(len_bytes) as usize;
                    let log = &record[32..(32 + data_len)];
                    if log == sent_msg.as_bytes() {
                        // We found our log!
                        return;
                    }
                }
                Err(status) if status == zx::Status::SHOULD_WAIT => {
                    debuglog
                        .wait_handle(zx::Signals::LOG_READABLE, zx::Time::INFINITE)
                        .expect("Failed to wait for log readable");
                    continue;
                }
                Err(status) => {
                    panic!("Unexpected error from zx_debuglog_read: {}", status);
                }
            }
        }
        panic!("first 10000 log messages didn't include the one we sent!");
    }

    #[test]
    fn log_info_test() {
        let mut rng = rand::thread_rng();
        let logged_value: u64 = rng.gen();

        init_logger_once();
        info!("log_test {}", logged_value);

        expect_message_in_debuglog(format!("[component_manager] INFO: log_test {}", logged_value));
    }

    #[test]
    fn log_warn_test() {
        let mut rng = rand::thread_rng();
        let logged_value: u64 = rng.gen();

        init_logger_once();
        warn!("log_test {}", logged_value);

        expect_message_in_debuglog(format!("[component_manager] WARN: log_test {}", logged_value));
    }

    #[test]
    fn log_error_test() {
        let mut rng = rand::thread_rng();
        let logged_value: u64 = rng.gen();

        init_logger_once();
        error!("log_test {}", logged_value);

        expect_message_in_debuglog(format!("[component_manager] ERROR: log_test {}", logged_value));
    }

    #[test]
    fn log_panic_test() {
        let mut rng = rand::thread_rng();
        let logged_value: u64 = rng.gen();

        init_logger_once();
        let result = panic::catch_unwind(|| panic!("panic_test {}", logged_value));
        assert!(result.is_err());

        expect_message_in_debuglog(format!(
            "[component_manager] PANIC: panic_test {}",
            logged_value
        ));
    }
}
