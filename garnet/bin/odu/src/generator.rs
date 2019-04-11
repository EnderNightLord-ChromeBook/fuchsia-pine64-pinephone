// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Implementation of Generator thread and Generator trait.
//!
//! Generator thread accept a set of serializable arguments.
use {
    crate::file_target::FileBlockingTarget,
    crate::blob_target::BlobBlockingTarget,
    crate::issuer::{run_issuer, IssuerArgs},
    crate::log::Stats,
    crate::operations::{OperationType, PipelineStages, Target},
    crate::sequential_io_generator::SequentialIoGenerator,
    crate::verifier::{run_verifier, VerifierArgs},
    failure::Error,
    log::{debug, error},
    serde_derive::{Deserialize, Serialize},
    std::{
        clone::Clone,
        collections::HashMap,
        ops::Range,
        process,
        sync::{
            mpsc::{channel, sync_channel},
            Arc, Condvar, Mutex,
        },
        thread::spawn,
        time::Instant,
    },
};

/// This structure provides a mechanism for issuer to block on commands from
// generator or from verifiers. When command_count drops to zero, issuer blocks
// on someone to wake them up.
/// When generator or verifier insert a command in issuer's channel they signal
/// the issuer to wake up.
#[derive(Clone)]
pub struct ActiveCommands {
    /// command_count indicates how many commands are in issuers queue.
    /// Mutex and condition variable protect and help to wait on the count.
    command_count: Arc<(Mutex<u64>, Condvar)>,
}

impl ActiveCommands {
    pub fn new() -> ActiveCommands {
        ActiveCommands { command_count: Arc::new((Mutex::new(0 as u64), Condvar::new())) }
    }

    /// Decrements number of active commands. Waits on the condition variable if
    /// command_count is zero. Returns true if command_count was zero and call
    /// was blocked.
    /// ```
    /// let mut count = ActiveCommands::new();
    ///
    /// Thread 1
    /// command_count.remove();
    /// cmd = receiver.try_recv();
    /// assert_eq!(cmd.is_ok());
    ///
    /// Thread 2
    /// sender.send(cmd);
    /// command_count.insert();
    /// ```
    pub fn decrement(&mut self) -> bool {
        let &(ref lock, ref cvar) = &*self.command_count;
        let mut count = lock.lock().unwrap();
        let mut slept = false;

        while (*count) == 0 {
            slept = true;
            debug!("waiting to on command");
            count = cvar.wait(count).unwrap();
        }
        (*count) -= 1;
        slept
    }

    /// Increments command_count and notifies one waiter.
    pub fn increment(&mut self) {
        let &(ref lock, ref cvar) = &*self.command_count;
        let mut count = lock.lock().unwrap();
        (*count) += 1;
        cvar.notify_one();
    }

    /// Returns value of command_count
    pub fn count(&self) -> u64 {
        let &(ref lock, ref _cvar) = &*self.command_count;
        let count = lock.lock().unwrap();
        *count
    }
}

/// Generating an IoPacket involves several variants like
/// - data for the IO and it's checksum
/// - data size
/// - offset of the IO
/// - several other (future) things like file name, directory path.
/// When we want randomly generated IO to be repeatable, we need to generate
/// a random number from a seed and based on that random number, we derive
/// variants of the IO. A typical use of Generator would look something like
/// ```
///  let generator: Generator = create_my_awesome_generator();
///  while (disks_death) {
///     random_number = generator.generate_number();
///     io_range = generator.get_io_range();
///     io_type = generator.get_io_operation();
///     io_packet = create_io_packet(io_type, io_range);
///     generator.fill_buffer(io_packet);
///  }
/// ```
pub trait Generator {
    /// Generates a new [random] number and return it's value.
    fn generate_number(&mut self) -> u64;

    /// Returns type of operation corresponding to the last generated [random]
    /// number
    fn get_io_operation(&self, allowed_ops: &Vec<OperationType>) -> OperationType;

    /// Returns Range (start and end] of IO operation. end - start gives the size
    /// of the IO
    fn get_io_range(&self) -> Range<u64>;

    /// Generates and fills the buf with data.
    fn fill_buffer(&self, buf: &mut Vec<u8>, sequence_number: u64, operation_id: u64, offset_range: &Range<u64>);
}

/// GeneratorArgs contains only the fields that help generator make decisions
/// needed for re-playability. This structure can be serialized and saved
/// for possible later use.
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct GeneratorArgs {
    /// magic_number helps to identify that the block was written
    /// by the app.
    magic_number: u64,

    /// process_id helps to differentiate this run from other runs
    process_id: u64,

    /// Human friendly name for this thread.
    name: String,

    /// Unique identifier for each generator.
    generator_unique_id: u64,

    /// Target block size. For some Targets,
    /// IO might fail if size of IO is not a multiple of
    /// block_size. This size is also used to watermark the
    /// block with block header
    block_size: u64,

    /// MTU per IO that Target can handle.
    /// 0 represents N/A for this Target
    max_io_size: u64,

    /// Hard alignment requirements without which IOs might fail
    align: bool,

    /// Seed that will be used to generate IOs in this thread
    seed: u64,

    /// Name of the target on which generator will perform IOs.
    target_name: String,

    /// target_range describes the portion of the Target
    /// the generator is allowed to work on. Other instances
    /// of Target may work on different ranges within the same
    /// Target.
    /// All generated IoPacket's offset and length should
    /// fall in this range
    target_range: Range<u64>,

    /// Target type. When there are multiple target types in the apps, this
    /// will help us search and load the right target operations.
    target_type: String,

    /// The maximum allowed number of outstanding IOs that are generated and
    /// are in Issuer queue. This number does not limit IOs that belong to verify
    /// operation.
    issuer_queue_depth: u64,

    /// The number of IOs that need to be issued before we gracefully tear-down
    /// generator thread.
    /// TODO(auradkar): Introduce time bound exit criteria.
    max_io_count: u64,

    /// When true, the target access (read/write) are sequential with respect to
    /// offsets within the target and within a thread.
    sequential: bool,
}

impl GeneratorArgs {
    pub fn new(
        magic_number: u64,
        process_id: u64,
        id: u64,
        block_size: u64,
        max_io_size: u64,
        align: bool,
        seed: u64,
        target_name: String,
        target_range: Range<u64>,
        target_type: String,
        issuer_queue_depth: u64,
        max_io_count: u64,
        sequential: bool,
    ) -> GeneratorArgs {
        GeneratorArgs {
            name: String::from(format!("generator-{}", id)),
            generator_unique_id: id,
            block_size: block_size,
            max_io_size: max_io_size,
            align: align,
            seed: seed,
            target_name: target_name,
            target_range: target_range,
            target_type: target_type,
            issuer_queue_depth: issuer_queue_depth,
            magic_number: magic_number,
            process_id: process_id,
            max_io_count: max_io_count,
            sequential: sequential,
        }
    }
}

/// Based on the input args this returns a set of allowed operations that
/// generator is allowed to issue. For now we only allow writes.
fn pick_operation_type(_args: &GeneratorArgs) -> Vec<OperationType> {
    let res = vec![OperationType::Write];
    return res;
}

/// Based on the input args this returns a generator that can generate requested
/// IO load.For now we only allow sequential io.
fn pick_generator_type(args: &GeneratorArgs, target_id: u64) -> Box<Generator> {
    if !args.sequential {
        error!("Only sequential io generator is implemented at the moment");
        process::abort();
    }

    Box::new(SequentialIoGenerator::new(
        args.magic_number,
        args.process_id,
        target_id,
        args.generator_unique_id,
        &args.target_range,
        args.block_size,
        args.max_io_size,
        args.align,
    ))
}

/// Based on the input args, create_target searches available Targets and
/// creates an appropriate Target trait.
fn create_target(
    target_type: String,
    target_id: u64,
    target_name: &String,
    offset_range: &Range<u64>,
    start_instant: &Instant,
) -> Arc<Box<Target + Send + Sync>> {
    // Manually check what is passed is what is supported.
    if target_type == "target_file" {
            return FileBlockingTarget::new(target_name.to_string(), target_id, offset_range, start_instant)
        .unwrap();
    } else if target_type == "target_blob" {
        return BlobBlockingTarget::new(target_name.to_string(), target_id, offset_range, start_instant)
        .unwrap();
    }

        error!("Target type {} not supported", target_type);
        process::abort();
}

/// The main thread for generator. generator creates a set of channels that it
// shares with issuer and verifier to communicate.
pub fn run_generator(
    args: GeneratorArgs,
    start_instant: Instant,
    stats: Arc<Mutex<Stats>>,
) -> Result<(), Error> {
    // Channel used to send commands from generator to issuer
    // This is the only bounded channel. The throttle control happens over this channel.
    let (gi_to_issuer, gi_from_generator) = sync_channel(args.issuer_queue_depth as usize);

    // Channel used to send commands from issuer to verifier
    let (iv_to_verifier, iv_from_issuer) = channel();

    // Channel used to send commands from verifier to generator
    let (vi_to_issuer, vi_from_verifier) = channel();

    // A hashmap of all outstanding IOs. Shared between generator and verifier.
    // Generator inserts entries and verifier removes it.
    let io_map = Arc::new(Mutex::new(HashMap::new()));

    // Mechanism to notify issuer of IOs.
    let mut active_commands = ActiveCommands::new();
    assert_eq!(active_commands.count(), 0);

    // The generator's stage in lifetime of an IO
    let stage = PipelineStages::Generate;

    // An array of allowed operations that helps generator to pick an operation
    // based on generated random number.
    let allowed_operations = pick_operation_type(&args);

    // Generator specific target unique id.
    let target_id = 0;

    // Thread handle to wait on for joining.
    let mut thread_handles = vec![];

    // IO sequence number. Order of IOs issued need not be same as order they arrive at
    // verifier and get logged. While replaying, this number helps us determine order
    // to issue IOs irrespective of the order they are read from replay log.
    let mut io_sequence_number: u64 = 0;

    let target = create_target(
        args.target_type.clone(),
        target_id,
        &args.target_name,
        &args.target_range,
        &start_instant,
    );

    let mut gen = pick_generator_type(&args, target_id);

    // Create Issuer
    let issuer_args = IssuerArgs::new(
        String::from(format!("issues-{}", args.generator_unique_id)),
        0,
        gi_from_generator,
        iv_to_verifier,
        vi_from_verifier,
        active_commands.clone(),
    );
    thread_handles.push(spawn(move || run_issuer(issuer_args)));

    // Create verifier
    let verifier_args = VerifierArgs::new(
        String::from(format!("verifier-{}", args.generator_unique_id)),
        0,
        iv_from_issuer,
        vi_to_issuer,
        false,
        io_map.clone(),
        stats.clone(),
        active_commands.clone(),
    );
    thread_handles.push(spawn(move || run_verifier(verifier_args)));

    for _i in 0..args.max_io_count {
        if active_commands.count() == 0 {
            debug!("{} running slow.", args.name);
        }

        let io_seed = gen.generate_number();
        let io_range = gen.get_io_range();
        let op_type = gen.get_io_operation(&allowed_operations);

        let mut io_packet =
            target.create_io_packet(op_type, io_sequence_number, io_seed, io_range, &target);
        io_packet.timestamp_stage_start(&stage);
        io_sequence_number += 1;
        let io_offset_range = io_packet.io_offset_range().clone();
        gen.fill_buffer(io_packet.buffer_mut(), io_sequence_number, op_type as u64, &io_offset_range);
        {
            let mut map = io_map.lock().unwrap();
            map.insert(io_packet.sequence_number().clone(), io_packet.clone());
        }
        io_packet.timestamp_stage_end(&stage);
        if gi_to_issuer.send(io_packet).is_err() {
            error!("error sending command");
            process::abort();
        }
        active_commands.increment();
    }

    // Exit gracefully
    let io_packet =
        target.create_io_packet(OperationType::Exit, io_sequence_number, 4, 0..1, &target);
    if gi_to_issuer.send(io_packet).is_err() {
        error!("error sending exit command");
        process::abort();
    }
    // drop the channel so that receiver doesn't wait and then increment
    drop(gi_to_issuer);
    active_commands.increment();

    for handle in thread_handles {
        handle.join().unwrap()?;
    }
    stats.lock().unwrap().stop_clock();
    Ok(())
}

#[cfg(test)]
mod tests {
    use {
        crate::generator::ActiveCommands,
        std::thread::sleep,
        std::{thread, time},
    };

    #[test]
    fn active_command_test() {
        let mut command_count = ActiveCommands::new();
        assert_eq!(command_count.count(), 0);

        command_count.increment();
        assert_eq!(command_count.count(), 1);

        command_count.increment();
        assert_eq!(command_count.count(), 2);

        assert_eq!(command_count.decrement(), false);
        assert_eq!(command_count.count(), 1);

        assert_eq!(command_count.decrement(), false);
        assert_eq!(command_count.count(), 0);
    }

    #[test]
    fn active_command_block_test() {
        let mut command_count = ActiveCommands::new();
        assert_eq!(command_count.count(), 0);
        let mut command_count_copy = command_count.clone();

        command_count.increment();

        let thd = thread::spawn(move || {
            sleep(time::Duration::from_secs(1));
            // First repay will wake the other threads sleeping borrower.
            command_count_copy.increment();
        });

        // On first call we dont block as the we find it immediately
        assert_eq!(command_count.decrement(), false);

        // On second call we block as the thread that is supposed to increment in
        // sleeping for a second.
        assert_eq!(command_count.decrement(), true);
        let _ = thd.join();

        // command count should be zero now
        assert_eq!(command_count.count(), 0);
    }
}
