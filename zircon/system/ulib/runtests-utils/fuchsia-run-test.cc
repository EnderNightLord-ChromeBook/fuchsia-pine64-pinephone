// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/debugdata/debugdata.h>
#include <lib/fdio/io.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/spawn.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/clock.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/dlfcn.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include <utility>

#include <fbl/auto_call.h>
#include <fbl/string.h>
#include <fbl/string_printf.h>
#include <fbl/unique_fd.h>
#include <fs/synchronous-vfs.h>
#include <loader-service/loader-service.h>
#include <runtests-utils/fuchsia-run-test.h>
#include <runtests-utils/service-proxy-dir.h>

namespace runtests {

namespace {

// Path to helper binary which can run test as a component. This binary takes
// component url as its parameter.
constexpr char kRunTestComponentPath[] = "/bin/run-test-component";

fbl::String DirectoryName(const fbl::String& path) {
  char* cpath = strndup(path.data(), path.length());
  fbl::String ret(dirname(cpath));
  free(cpath);
  return ret;
}

fbl::String BaseName(const fbl::String& path) {
  char* cpath = strndup(path.data(), path.length());
  fbl::String ret(basename(cpath));
  free(cpath);
  return ret;
}

fbl::String RootName(const fbl::String& path) {
  const size_t i = strspn(path.c_str(), "/");
  const char* start = &path.c_str()[i];
  const char* end = strchr(start, '/');
  if (end == nullptr) {
    end = &path.c_str()[path.size()];
  }
  return fbl::String::Concat({"/", fbl::String(start, end - start)});
}

std::optional<DumpFile> ProcessDataSinkDump(debugdata::DataSinkDump& data,
                                            fbl::unique_fd& data_sink_dir_fd, const char* path) {
  if (mkdirat(data_sink_dir_fd.get(), data.sink_name.c_str(), 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "FAILURE: cannot mkdir \"%s\" for data-sink: %s\n", data.sink_name.c_str(),
            strerror(errno));
    return {};
  }
  fbl::unique_fd sink_dir_fd{
      openat(data_sink_dir_fd.get(), data.sink_name.c_str(), O_RDONLY | O_DIRECTORY)};
  if (!sink_dir_fd) {
    fprintf(stderr, "FAILURE: cannot open data-sink directory \"%s\": %s\n", data.sink_name.c_str(),
            strerror(errno));
    return {};
  }

  fzl::VmoMapper mapper;
  zx_status_t status = mapper.Map(data.file_data, 0, 0, ZX_VM_PERM_READ);
  if (status != ZX_OK) {
    fprintf(stderr, "FAILURE: Cannot map VMO for data-sink \"%s\": %s\n", data.sink_name.c_str(),
            zx_status_get_string(status));
    return {};
  }

  zx_info_handle_basic_t info;
  status = data.file_data.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    return {};
  }
  char filename[ZX_MAX_NAME_LEN];
  snprintf(filename, sizeof(filename), "%s.%" PRIu64, data.sink_name.c_str(), info.koid);

  fbl::unique_fd fd{openat(sink_dir_fd.get(), filename, O_WRONLY | O_CREAT | O_EXCL, 0666)};
  if (!fd) {
    fprintf(stderr, "FAILURE: Cannot open data-sink file \"%s\": %s\n", data.sink_name.c_str(),
            strerror(errno));
    return {};
  }
  // Strip any leading slashes (including a sequence of slashes) so the dump
  // file path is a relative to directory that contains the summary file.
  size_t i = strspn(path, "/");
  auto dump_file = JoinPath(&path[i], JoinPath(data.sink_name, filename));

  auto* buf = reinterpret_cast<uint8_t*>(mapper.start());
  ssize_t count = mapper.size();
  while (count > 0) {
    ssize_t len = write(fd.get(), buf, count);
    if (len == -1) {
      fprintf(stderr, "FAILURE: Cannot write data to \"%s\": %s\n", dump_file.c_str(),
              strerror(errno));
      return {};
    }
    count -= len;
    buf += len;
  }

  char name[ZX_MAX_NAME_LEN];
  status = data.file_data.get_property(ZX_PROP_NAME, name, sizeof(name));
  if (status != ZX_OK) {
    return {};
  }
  if (name[0] == '\0') {
    snprintf(name, sizeof(name), "unnamed.%" PRIu64, info.koid);
  }

  return DumpFile{name, dump_file.c_str()};
}

}  // namespace

void TestFileComponentInfo(const fbl::String& path, fbl::String* component_url_out,
                           fbl::String* cmx_file_path_out) {
  if (strncmp(path.c_str(), kPkgPrefix, strlen(kPkgPrefix)) != 0) {
    return;
  }

  // Consume suffixes of the form
  // "test/<test filename>" or "test/disabled/<test filename>"
  bool is_disabled = (strstr(path.c_str(), "/disabled/") != nullptr);
  const auto folder_path = is_disabled ? DirectoryName(DirectoryName(DirectoryName(path)))
                                       : DirectoryName(DirectoryName(path));

  // folder_path should also start with |kPkgPrefix| and should not be equal
  // to |kPkgPrefix|.
  if (strncmp(folder_path.c_str(), kPkgPrefix, strlen(kPkgPrefix)) != 0 ||
      folder_path == fbl::String(kPkgPrefix)) {
    return;
  }

  const char* package_name = path.c_str() + strlen(kPkgPrefix);
  // find occurence of first '/'
  size_t i = 0;
  while (package_name[i] != '\0' && package_name[i] != '/') {
    i++;
  }
  const fbl::String package_name_str(package_name, i);
  const auto test_file_name = BaseName(path);
  *cmx_file_path_out =
      fbl::StringPrintf("%s/meta/%s.cmx", folder_path.c_str(), test_file_name.c_str());
  *component_url_out = fbl::StringPrintf("fuchsia-pkg://fuchsia.com/%s#meta/%s.cmx",
                                         package_name_str.c_str(), test_file_name.c_str());
}

std::unique_ptr<Result> FuchsiaRunTest(const char* argv[], const char* output_dir,
                                       const char* output_filename, const char* test_name) {
  // The arguments passed to fdio_spaws_etc. May be overridden.
  const char** args = argv;
  // calculate size of argv
  size_t argc = 0;
  while (argv[argc] != nullptr) {
    argc++;
  }

  // Values used when running the test as a component.
  const char* component_launch_args[argc + 2];
  fbl::String component_url;
  fbl::String cmx_file_path;

  const char* path = argv[0];

  TestFileComponentInfo(path, &component_url, &cmx_file_path);
  struct stat s;
  // If we get a non empty |cmx_file_path|, check that it exists, and if
  // present launch the test as component using generated |component_url|.
  if (cmx_file_path != "" && stat(cmx_file_path.c_str(), &s) == 0) {
    if (stat(kRunTestComponentPath, &s) == 0) {
      component_launch_args[0] = kRunTestComponentPath;
      component_launch_args[1] = component_url.c_str();
      for (size_t i = 1; i <= argc; i++) {
        component_launch_args[1 + i] = argv[i];
      }
      args = component_launch_args;
    } else {
      // TODO(anmittal): Make this a error once we have a stable
      // system and we can run all tests as components.
      fprintf(stderr,
              "WARNING: Cannot find '%s', running '%s' as normal test "
              "binary.",
              kRunTestComponentPath, path);
    }
  }

  // Truncate the name on the left so the more important stuff on the right part of the path stays
  // in the name.
  const char* test_name_trunc = test_name;
  size_t test_name_length = strlen(test_name_trunc);
  if (test_name_length > ZX_MAX_NAME_LEN - 1) {
    test_name_trunc += test_name_length - (ZX_MAX_NAME_LEN - 1);
  }

  fbl::Vector<fdio_spawn_action_t> fdio_actions = {
      fdio_spawn_action_t{.action = FDIO_SPAWN_ACTION_SET_NAME, .name = {.data = test_name_trunc}},
  };

  zx_status_t status;
  zx::channel svc_proxy_req;
  fbl::RefPtr<ServiceProxyDir> proxy_dir;
  async::Loop loop{&kAsyncLoopConfigNoAttachToThread};
  std::unique_ptr<fs::SynchronousVfs> vfs;
  std::unique_ptr<debugdata::DebugData> debug_data;

  // Export the root namespace.
  fdio_flat_namespace_t* flat;
  if ((status = fdio_ns_export_root(&flat)) != ZX_OK) {
    fprintf(stderr, "FAILURE: Cannot export root namespace: %s\n", zx_status_get_string(status));
    return std::make_unique<Result>(path, FAILED_UNKNOWN, 0, 0);
  }

  auto action_ns_entry = [](const char* prefix, zx_handle_t handle) {
    return fdio_spawn_action{.action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY,
                             .ns = {
                                 .prefix = prefix,
                                 .handle = handle,
                             }};
  };

  // If |output_dir| is provided, set up the loader and debugdata services that will be
  // used to capture any data published.
  if (output_dir != nullptr) {
    fbl::unique_fd root_dir_fd{open("/", O_RDONLY | O_DIRECTORY)};
    if (!root_dir_fd) {
      fprintf(stderr, "FAILURE: Could not open root directory /\n");
      return std::make_unique<Result>(path, FAILED_UNKNOWN, 0, 0);
    }

    zx::channel svc_proxy;
    status = zx::channel::create(0, &svc_proxy, &svc_proxy_req);
    if (status != ZX_OK) {
      fprintf(stderr, "FAILURE: Cannot create channel: %s\n", zx_status_get_string(status));
      return std::make_unique<Result>(path, FAILED_UNKNOWN, 0, 0);
    }

    zx::channel svc_handle;
    for (size_t i = 0; i < flat->count; ++i) {
      if (!strcmp(flat->path[i], "/svc")) {
        // Save the current /svc handle...
        svc_handle.reset(flat->handle[i]);
        // ...and replace it with the proxy /svc.
        fdio_actions.push_back(action_ns_entry("/svc", svc_proxy_req.get()));
      } else {
        fdio_actions.push_back(action_ns_entry(flat->path[i], flat->handle[i]));
      }
    }

    // Setup DebugData service implementation.
    debug_data = std::make_unique<debugdata::DebugData>(std::move(root_dir_fd));

    // Setup proxy dir.
    proxy_dir = fbl::MakeRefCounted<ServiceProxyDir>(std::move(svc_handle));
    auto node = fbl::MakeRefCounted<fs::Service>(
        [dispatcher = loop.dispatcher(), debug_data = debug_data.get()](zx::channel channel) {
          return fidl::Bind(dispatcher, std::move(channel), debug_data);
        });
    proxy_dir->AddEntry(::llcpp::fuchsia::debugdata::DebugData::Name, node);

    // Setup VFS.
    vfs = std::make_unique<fs::SynchronousVfs>(loop.dispatcher());
    vfs->ServeDirectory(std::move(proxy_dir), std::move(svc_proxy),
                        ZX_FS_FLAG_DIRECTORY | ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE);
    loop.StartThread();
  } else {
    for (size_t i = 0; i < flat->count; ++i) {
      fdio_actions.push_back(action_ns_entry(flat->path[i], flat->handle[i]));
    }
  }

  // If |output_filename| is provided, prepare the file descriptors that will
  // be used to tee the stdout/stderr of the test into the associated file.
  fbl::unique_fd fds[2];
  if (output_filename != nullptr) {
    int temp_fds[2] = {-1, -1};
    if (pipe(temp_fds)) {
      fprintf(stderr, "FAILURE: Failed to create pipe: %s\n", strerror(errno));
      return std::make_unique<Result>(test_name, FAILED_TO_LAUNCH, 0, 0);
    }
    fds[0].reset(temp_fds[0]);
    fds[1].reset(temp_fds[1]);

    fdio_actions.push_back(
        fdio_spawn_action{.action = FDIO_SPAWN_ACTION_CLONE_FD,
                          .fd = {.local_fd = fds[1].get(), .target_fd = STDOUT_FILENO}});
    fdio_actions.push_back(
        fdio_spawn_action{.action = FDIO_SPAWN_ACTION_TRANSFER_FD,
                          .fd = {.local_fd = fds[1].get(), .target_fd = STDERR_FILENO}});
  }
  zx::job test_job;
  status = zx::job::create(*zx::job::default_job(), 0, &test_job);
  if (status != ZX_OK) {
    fprintf(stderr, "FAILURE: zx::job::create() returned %d\n", status);
    return std::make_unique<Result>(test_name, FAILED_TO_LAUNCH, 0, 0);
  }
  auto auto_call_kill_job = fbl::MakeAutoCall([&test_job]() { test_job.kill(); });
  status = test_job.set_property(ZX_PROP_NAME, "run-test", sizeof("run-test"));
  if (status != ZX_OK) {
    fprintf(stderr, "FAILURE: set_property() returned %d\n", status);
    return std::make_unique<Result>(test_name, FAILED_TO_LAUNCH, 0, 0);
  }

  // The TEST_ROOT_DIR environment variable allows tests that could be stored in
  // "/system" or "/boot" to discern where they are running, and modify paths
  // accordingly.
  //
  // TODO(BLD-463): The hard-coded set of prefixes is not ideal. Ideally, this
  // would instead set the "root" to the parent directory of the "test/"
  // subdirectory where globbing was done to collect the set of tests in
  // DiscoverAndRunTests().  But then it's not clear what should happen if
  // using `-f` to provide a list of paths instead of directories to glob.
  const fbl::String root = RootName(path);
  // |root_var| must be kept alive for |env_vars| since |env_vars| may hold
  // a pointer into it.
  fbl::String root_var;
  fbl::Vector<const char*> env_vars;
  if (root == "/system" || root == "/boot") {
    for (size_t i = 0; environ[i] != nullptr; ++i) {
      env_vars.push_back(environ[i]);
    }
    root_var = fbl::String::Concat({"TEST_ROOT_DIR=", root});
    env_vars.push_back(root_var.c_str());
    env_vars.push_back(nullptr);
  }
  const char* const* env_vars_p = !env_vars.is_empty() ? env_vars.begin() : nullptr;

  fds[1].release();  // To avoid double close since fdio_spawn_etc() closes it.
  zx::process process;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  const zx::time start_time = zx::clock::get_monotonic();
  status = fdio_spawn_etc(test_job.get(), FDIO_SPAWN_CLONE_ALL & ~FDIO_SPAWN_CLONE_NAMESPACE,
                          args[0], args, env_vars_p, fdio_actions.size(), fdio_actions.get(),
                          process.reset_and_get_address(), err_msg);
  if (status != ZX_OK) {
    fprintf(stderr, "FAILURE: Failed to launch %s: %d (%s): %s\n", test_name, status,
            zx_status_get_string(status), err_msg);
    return std::make_unique<Result>(test_name, FAILED_TO_LAUNCH, 0, 0);
  }

  // Tee output.
  if (output_filename != nullptr) {
    FILE* output_file = fopen(output_filename, "w");
    if (output_file == nullptr) {
      fprintf(stderr, "FAILURE: Could not open output file at %s: %s\n", output_filename,
              strerror(errno));
      return std::make_unique<Result>(test_name, FAILED_DURING_IO, 0, 0);
    }
    char buf[1024];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(fds[0].get(), buf, sizeof(buf))) > 0) {
      fwrite(buf, 1, bytes_read, output_file);
      fwrite(buf, 1, bytes_read, stdout);
    }
    fflush(stdout);
    fflush(stderr);
    fflush(output_file);
    if (fclose(output_file)) {
      fprintf(stderr, "FAILURE:  Could not close %s: %s", output_filename, strerror(errno));
      return std::make_unique<Result>(test_name, FAILED_DURING_IO, 0, 0);
    }
  }

  status = process.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr);
  const zx::time end_time = zx::clock::get_monotonic();
  const int64_t duration_milliseconds = (end_time - start_time).to_msecs();
  if (status != ZX_OK) {
    fprintf(stderr, "FAILURE: Failed to wait for process exiting %s: %d\n", test_name, status);
    return std::make_unique<Result>(test_name, FAILED_TO_WAIT, 0, duration_milliseconds);
  }

  // Read the return code.
  zx_info_process_t proc_info;
  status = process.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr);

  if (status != ZX_OK) {
    fprintf(stderr, "FAILURE: Failed to get process return code %s: %d\n", test_name, status);
    return std::make_unique<Result>(test_name, FAILED_TO_RETURN_CODE, 0, duration_milliseconds);
  }

  std::unique_ptr<Result> result;
  if (proc_info.return_code == 0) {
    fprintf(stderr, "PASSED: %s passed\n", test_name);
    result = std::make_unique<Result>(test_name, SUCCESS, 0, duration_milliseconds);
  } else {
    fprintf(stderr, "FAILURE: %s exited with nonzero status: %" PRId64 "\n", test_name,
            proc_info.return_code);
    result = std::make_unique<Result>(test_name, FAILED_NONZERO_RETURN_CODE, proc_info.return_code,
                                      duration_milliseconds);
  }

  if (output_dir == nullptr) {
    return result;
  }

  // Make sure that all job processes are dead before touching any data.
  auto_call_kill_job.call();

  // Stop the loop.
  loop.Quit();

  // Wait for any unfinished work to be completed.
  loop.JoinThreads();

  // Run one more time until there are no unprocessed messages.
  loop.ResetQuit();
  loop.Run(zx::time(0));

  // Tear down the the VFS.
  vfs.reset();

  fbl::unique_fd data_sink_dir_fd{open(output_dir, O_RDONLY | O_DIRECTORY)};
  if (!data_sink_dir_fd) {
    printf("FAILURE: Could not open output directory %s: %s\n", output_dir, strerror(errno));
    return result;
  }

  for (auto& data : debug_data->GetData()) {
    if (auto dump_file = ProcessDataSinkDump(data, data_sink_dir_fd, path)) {
      result->data_sinks[data.sink_name].push_back(*dump_file);
    } else if (result->return_code == 0) {
      result->launch_status = FAILED_COLLECTING_SINK_DATA;
    }
  }

  return result;
}

}  // namespace runtests
