// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

// This file defines:
// * Initialization code for kernel/object module
// * Singleton instances and global locks
// * Helper functions

#include <inttypes.h>
#include <lib/cmdline.h>
#include <lib/crashlog.h>
#include <zircon/syscalls/object.h>
#include <zircon/types.h>

#include <lk/init.h>
#include <object/diagnostics.h>
#include <object/event_dispatcher.h>
#include <object/excp_port.h>
#include <object/job_dispatcher.h>
#include <object/port_dispatcher.h>
#include <platform/halt_helper.h>

// All jobs and processes are rooted at the |root_job|.
static fbl::RefPtr<JobDispatcher> root_job;

fbl::RefPtr<JobDispatcher> GetRootJobDispatcher() { return root_job; }

// Kernel-owned event that is used to signal userspace before taking action in OOM situation.
static fbl::RefPtr<EventDispatcher> low_mem_event;
// Event used for communicating lowmem state between the lowmem callback and the oom thread.
static Event mem_state_signal(EVENT_FLAG_AUTOUNSIGNAL);
static ktl::atomic<uint8_t> mem_event_idx = 1;

fbl::RefPtr<EventDispatcher> GetLowMemEvent() { return low_mem_event; }

// Callback used with |pmm_init_reclamation|.
// This is a very minimal save idx and signal an event as we are called under the pmm lock and must
// avoid causing any additional allocations.
static void mem_avail_state_updated_cb(uint8_t idx) {
  mem_event_idx = idx;
  mem_state_signal.Signal();
}

// Helper called by the oom thread when low memory mode is entered.
static void on_lowmem() {
#if defined(ENABLE_KERNEL_DEBUGGING_FEATURES)
  // See ZX-3637 for the product details on when this path vs. the reboot
  // should be used.

  if (!root_job->KillJobWithKillOnOOM()) {
    printf("OOM: no alive job has a kill bit\n");
  }

  // Since killing is asynchronous, sleep for a short period for the system to quiesce. This
  // prevents us from rapidly killing more jobs than necessary. And if we don't find a
  // killable job, don't just spin since the next iteration probably won't find a one either.
  thread_sleep_relative(ZX_MSEC(500));
#else
  const int kSleepSeconds = 8;
  printf("OOM: pausing for %ds after low mem signal\n", kSleepSeconds);
  zx_status_t status = thread_sleep_relative(ZX_SEC(kSleepSeconds));
  if (status != ZX_OK) {
    printf("OOM: sleep failed: %d\n", status);
  }
  printf("OOM: rebooting\n");
  static char buf[1024];
  size_t len = crashlog_to_string(buf, sizeof(buf), CrashlogType::OOM);
  platform_stow_crashlog(buf, len);
  platform_graceful_halt_helper(HALT_ACTION_REBOOT);
#endif
}

static int oom_thread(void* unused) {
  while (true) {
    // Check if the current index is 0. After observing this we know that if it should change to
    // zero that the event will get signaled and we would immediately wake back up.
    if (mem_event_idx != 0) {
      zx_status_t status = low_mem_event->user_signal_self(ZX_EVENT_SIGNALED, 0);
      if (status != ZX_OK) {
        printf("OOM: unsignal low mem failed: %d\n", status);
      }
      mem_state_signal.Wait(Deadline::infinite());
    }
    // get local copy of the atomic. It's possible by the time we read this that we've already
    // exited low mem mode, but that's fine as we're happy to not have to invoke the oom killer.
    uint8_t idx = mem_event_idx;
    printf("OOM: memory availability state %u\n", idx);

    if (idx == 0) {
      // Tell the user we're in low memory mode and then run our oom handler
#if !defined(ENABLE_KERNEL_DEBUGGING_FEATURES)
      zx_status_t status = low_mem_event->user_signal_self(0, ZX_EVENT_SIGNALED);
      if (status != ZX_OK) {
        printf("OOM: signal low mem failed: %d\n", status);
      }
#endif
      on_lowmem();
    }
  }
}

static void object_glue_init(uint level) TA_NO_THREAD_SAFETY_ANALYSIS {
  Handle::Init();
  root_job = JobDispatcher::CreateRootJob();
  PortDispatcher::Init();

  KernelHandle<EventDispatcher> event;
  zx_rights_t rights;
  zx_status_t status = EventDispatcher::Create(0, &event, &rights);
  if (status != ZX_OK) {
    panic("low mem event create: %d\n", status);
  }
  low_mem_event = event.release();

  if (gCmdline.GetBool("kernel.oom.enable", true)) {
    auto redline = gCmdline.GetUInt64("kernel.oom.redline-mb", 50) * MB;
    zx_status_t status = pmm_init_reclamation(&redline, 1, MB, mem_avail_state_updated_cb);
    if (status != ZX_OK) {
      panic("failed to initialize pmm reclamation: %d\n", status);
    }

    auto thread = thread_create("oom-thread", oom_thread, nullptr, HIGH_PRIORITY);
    DEBUG_ASSERT(thread);
    thread_detach(thread);
    thread_resume(thread);
  }
}

LK_INIT_HOOK(libobject, object_glue_init, LK_INIT_LEVEL_THREADING)
