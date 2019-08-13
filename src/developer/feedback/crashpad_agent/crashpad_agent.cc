// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/developer/feedback/crashpad_agent/crashpad_agent.h"

#include <fuchsia/crash/cpp/fidl.h>
#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fsl/handles/object_info.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/zx/time.h>
#include <stdio.h>
#include <unistd.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>

#include <map>
#include <string>
#include <utility>

#include "src/developer/feedback/crashpad_agent/config.h"
#include "src/developer/feedback/crashpad_agent/crash_report_util.h"
#include "src/developer/feedback/crashpad_agent/crash_server.h"
#include "src/developer/feedback/crashpad_agent/feedback_data_provider_ptr.h"
#include "src/developer/feedback/crashpad_agent/report_annotations.h"
#include "src/developer/feedback/crashpad_agent/report_attachments.h"
#include "src/developer/feedback/crashpad_agent/scoped_unlink.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/file.h"
#include "src/lib/files/unique_fd.h"
#include "third_party/crashpad/client/crash_report_database.h"
#include "third_party/crashpad/client/prune_crash_reports.h"
#include "third_party/crashpad/client/settings.h"
#include "third_party/crashpad/handler/fuchsia/crash_report_exception_handler.h"
#include "third_party/crashpad/handler/minidump_to_upload_parameters.h"
#include "third_party/crashpad/snapshot/minidump/process_snapshot_minidump.h"
#include "third_party/crashpad/third_party/mini_chromium/mini_chromium/base/files/file_path.h"
#include "third_party/crashpad/third_party/mini_chromium/mini_chromium/base/files/scoped_file.h"
#include "third_party/crashpad/util/file/file_io.h"
#include "third_party/crashpad/util/file/file_reader.h"
#include "third_party/crashpad/util/misc/metrics.h"
#include "third_party/crashpad/util/misc/uuid.h"
#include "third_party/crashpad/util/net/http_body.h"
#include "third_party/crashpad/util/net/http_headers.h"
#include "third_party/crashpad/util/net/http_multipart_builder.h"
#include "third_party/crashpad/util/net/http_transport.h"
#include "third_party/crashpad/util/net/url.h"

namespace fuchsia {
namespace crash {
namespace {

using crashpad::CrashReportDatabase;
using fuchsia::feedback::CrashReport;
using fuchsia::feedback::CrashReporter_File_Result;
using fuchsia::feedback::Data;

const char kDefaultConfigPath[] = "/pkg/data/default_config.json";
const char kOverrideConfigPath[] = "/config/data/override_config.json";

const char kKernelProgramName[] = "kernel";

}  // namespace

std::unique_ptr<CrashpadAgent> CrashpadAgent::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<::sys::ServiceDirectory> services,
    InspectManager* inspect_manager) {
  Config config;

  // We use the default config included in the package of this component if no override config was
  // specified or if we failed to parse the override config.
  bool use_default_config = true;

  if (files::IsFile(kOverrideConfigPath)) {
    use_default_config = false;
    if (const zx_status_t status = ParseConfig(kOverrideConfigPath, &config); status != ZX_OK) {
      // We failed to parse the override config: fall back to the default config.
      use_default_config = true;
      FX_PLOGS(ERROR, status) << "Failed to read override config file at " << kOverrideConfigPath
                              << " - falling back to default config file";
    }
  }

  // Either there was no override config or we failed to parse it.
  if (use_default_config) {
    if (const zx_status_t status = ParseConfig(kDefaultConfigPath, &config); status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to read default config file at " << kDefaultConfigPath;

      FX_LOGS(FATAL) << "Failed to set up crash analyzer";
      return nullptr;
    }
  }

  return CrashpadAgent::TryCreate(dispatcher, std::move(services), std::move(config),
                                  inspect_manager);
}

std::unique_ptr<CrashpadAgent> CrashpadAgent::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<::sys::ServiceDirectory> services,
    Config config, InspectManager* inspect_manager) {
  std::unique_ptr<CrashServer> crash_server;
  if (config.crash_server.enable_upload && config.crash_server.url) {
    crash_server = std::make_unique<CrashServer>(*config.crash_server.url);
  }
  return CrashpadAgent::TryCreate(dispatcher, std::move(services), std::move(config),
                                  std::move(crash_server), inspect_manager);
}

std::unique_ptr<CrashpadAgent> CrashpadAgent::TryCreate(
    async_dispatcher_t* dispatcher, std::shared_ptr<::sys::ServiceDirectory> services,
    Config config, std::unique_ptr<CrashServer> crash_server, InspectManager* inspect_manager) {
  if (!files::IsDirectory(config.crashpad_database.path)) {
    files::CreateDirectory(config.crashpad_database.path);
  }

  std::unique_ptr<crashpad::CrashReportDatabase> database(
      crashpad::CrashReportDatabase::Initialize(base::FilePath(config.crashpad_database.path)));
  if (!database) {
    FX_LOGS(ERROR) << "error initializing local crash report database at "
                   << config.crashpad_database.path;
    FX_LOGS(FATAL) << "failed to set up crash analyzer";
    return nullptr;
  }

  // Today we enable uploads here. In the future, this will most likely be set in some external
  // settings.
  database->GetSettings()->SetUploadsEnabled(config.crash_server.enable_upload);

  return std::unique_ptr<CrashpadAgent>(
      new CrashpadAgent(dispatcher, std::move(services), std::move(config), std::move(database),
                        std::move(crash_server), inspect_manager));
}

CrashpadAgent::CrashpadAgent(async_dispatcher_t* dispatcher,
                             std::shared_ptr<::sys::ServiceDirectory> services, Config config,
                             std::unique_ptr<crashpad::CrashReportDatabase> database,
                             std::unique_ptr<CrashServer> crash_server,
                             InspectManager* inspect_manager)
    : dispatcher_(dispatcher),
      executor_(dispatcher),
      services_(services),
      config_(std::move(config)),
      database_(std::move(database)),
      crash_server_(std::move(crash_server)),
      inspect_manager_(inspect_manager) {
  FXL_DCHECK(dispatcher_);
  FXL_DCHECK(services_);
  FXL_DCHECK(database_);
  FXL_DCHECK(inspect_manager_);
  if (config.crash_server.enable_upload) {
    FXL_DCHECK(crash_server_);
  }
}

void CrashpadAgent::OnNativeException(zx::process process, zx::thread thread,
                                      OnNativeExceptionCallback callback) {
  auto promise = OnNativeException(std::move(process), std::move(thread))
                     .and_then([] {
                       Analyzer_OnNativeException_Result result;
                       Analyzer_OnNativeException_Response response;
                       result.set_response(response);
                       return fit::ok(std::move(result));
                     })
                     .or_else([] {
                       FX_LOGS(ERROR) << "Failed to handle native exception. Won't retry.";
                       Analyzer_OnNativeException_Result result;
                       result.set_err(ZX_ERR_INTERNAL);
                       return fit::ok(std::move(result));
                     })
                     .and_then([callback = std::move(callback),
                                this](Analyzer_OnNativeException_Result& result) {
                       callback(std::move(result));
                       PruneDatabase();
                     });

  executor_.schedule_task(std::move(promise));
}

void CrashpadAgent::OnManagedRuntimeException(std::string component_url,
                                              ManagedRuntimeException exception,
                                              OnManagedRuntimeExceptionCallback callback) {
  auto promise = OnManagedRuntimeException(component_url, std::move(exception))
                     .and_then([] {
                       Analyzer_OnManagedRuntimeException_Result result;
                       Analyzer_OnManagedRuntimeException_Response response;
                       result.set_response(response);
                       return fit::ok(std::move(result));
                     })
                     .or_else([] {
                       FX_LOGS(ERROR) << "Failed to handle managed runtime exception. Won't retry.";
                       Analyzer_OnManagedRuntimeException_Result result;
                       result.set_err(ZX_ERR_INTERNAL);
                       return fit::ok(std::move(result));
                     })
                     .and_then([callback = std::move(callback),
                                this](Analyzer_OnManagedRuntimeException_Result& result) {
                       callback(std::move(result));
                       PruneDatabase();
                     });

  executor_.schedule_task(std::move(promise));
}

void CrashpadAgent::OnKernelPanicCrashLog(fuchsia::mem::Buffer crash_log,
                                          OnKernelPanicCrashLogCallback callback) {
  auto promise = OnKernelPanicCrashLog(std::move(crash_log))
                     .and_then([] {
                       Analyzer_OnKernelPanicCrashLog_Result result;
                       Analyzer_OnKernelPanicCrashLog_Response response;
                       result.set_response(response);
                       return fit::ok(std::move(result));
                     })
                     .or_else([] {
                       FX_LOGS(ERROR) << "Failed to process kernel panic crash log. Won't retry.";
                       Analyzer_OnKernelPanicCrashLog_Result result;
                       result.set_err(ZX_ERR_INTERNAL);
                       return fit::ok(std::move(result));
                     })
                     .and_then([callback = std::move(callback),
                                this](Analyzer_OnKernelPanicCrashLog_Result& result) {
                       callback(std::move(result));
                       PruneDatabase();
                     });

  executor_.schedule_task(std::move(promise));
}

void CrashpadAgent::File(fuchsia::feedback::CrashReport report, FileCallback callback) {
  if (!IsValid(report)) {
    FX_LOGS(ERROR) << "Invalid crash report. Won't file.";
    CrashReporter_File_Result result;
    result.set_err(ZX_ERR_INVALID_ARGS);
    callback(std::move(result));
    return;
  }

  auto promise =
      File(std::move(report))
          .and_then([] {
            CrashReporter_File_Result result;
            fuchsia::feedback::CrashReporter_File_Response response;
            result.set_response(response);
            return fit::ok(std::move(result));
          })
          .or_else([] {
            FX_LOGS(ERROR) << "Failed to file crash report. Won't retry.";
            CrashReporter_File_Result result;
            result.set_err(ZX_ERR_INTERNAL);
            return fit::ok(std::move(result));
          })
          .and_then([callback = std::move(callback), this](CrashReporter_File_Result& result) {
            callback(std::move(result));
            PruneDatabase();
          });

  executor_.schedule_task(std::move(promise));
}

namespace {

std::map<std::string, fuchsia::mem::Buffer> MakeAttachments(Data* feedback_data) {
  std::map<std::string, fuchsia::mem::Buffer> attachments;
  if (feedback_data->has_attachments()) {
    for (auto& attachment : *feedback_data->mutable_attachments()) {
      attachments[attachment.key] = std::move(attachment.value);
    }
  }
  return attachments;
}

}  // namespace

fit::promise<void> CrashpadAgent::OnNativeException(zx::process process, zx::thread thread) {
  const std::string process_name = fsl::GetObjectName(process.get());
  FX_LOGS(INFO) << "generating crash report for exception thrown by " << process_name;

  // Prepare annotations and attachments.
  return GetFeedbackData(dispatcher_, services_,
                         zx::msec(config_.feedback_data_collection_timeout_in_milliseconds))
      .then([this, process = std::move(process), thread = std::move(thread),
             process_name](fit::result<Data>& result) mutable -> fit::result<void> {
        Data feedback_data;
        if (result.is_ok()) {
          feedback_data = result.take_value();
        }
        const std::map<std::string, std::string> annotations =
            MakeDefaultAnnotations(feedback_data, process_name);
        const std::map<std::string, fuchsia::mem::Buffer> attachments =
            MakeAttachments(&feedback_data);

        // Set minidump and create local crash report.
        //   * The annotations will be stored in the minidump of the report and augmented with
        //   modules'
        //     annotations.
        //   * The attachments will be stored in the report.
        // We don't pass an upload_thread so we can do the upload ourselves synchronously.
        crashpad::CrashReportExceptionHandler exception_handler(
            database_.get(), /*upload_thread=*/nullptr, &annotations, &attachments,
            /*user_stream_data_sources=*/nullptr);
        crashpad::UUID local_report_id;
        if (!exception_handler.HandleException(process, thread, &local_report_id)) {
          // TODO(DX-1654): attempt to generate a crash report without a minidump instead of just
          // bailing.
          FX_LOGS(ERROR) << "error writing local crash report";
          return fit::error();
        }

        // For userspace, we read back the annotations from the minidump instead of passing them as
        // argument like for kernel crashes because the Crashpad handler augmented them with the
        // modules' annotations.
        if (!UploadReport(local_report_id, process_name, /*annotations=*/nullptr,
                          /*read_annotations_from_minidump=*/true)) {
          return fit::error();
        }
        return fit::ok();
      });
}

fit::promise<void> CrashpadAgent::OnManagedRuntimeException(std::string component_url,
                                                            ManagedRuntimeException exception) {
  FX_LOGS(INFO) << "generating crash report for exception thrown by " << component_url;

  // Create local crash report.
  std::unique_ptr<crashpad::CrashReportDatabase::NewReport> report;
  const crashpad::CrashReportDatabase::OperationStatus database_status =
      database_->PrepareNewCrashReport(&report);
  if (database_status != crashpad::CrashReportDatabase::kNoError) {
    FX_LOGS(ERROR) << "error creating local crash report (" << database_status << ")";
    return fit::make_error_promise();
  }

  // Prepare annotations and attachments.
  return GetFeedbackData(dispatcher_, services_,
                         zx::msec(config_.feedback_data_collection_timeout_in_milliseconds))
      .then([this, component_url, exception = std::move(exception),
             report = std::move(report)](fit::result<Data>& result) mutable -> fit::result<void> {
        Data feedback_data;
        if (result.is_ok()) {
          feedback_data = result.take_value();
        }
        const std::map<std::string, std::string> annotations =
            MakeManagedRuntimeExceptionAnnotations(feedback_data, component_url, &exception);
        AddManagedRuntimeExceptionAttachments(report.get(), feedback_data, &exception);

        // Finish new local crash report.
        crashpad::UUID local_report_id;
        const crashpad::CrashReportDatabase::OperationStatus database_status =
            database_->FinishedWritingCrashReport(std::move(report), &local_report_id);
        if (database_status != crashpad::CrashReportDatabase::kNoError) {
          FX_LOGS(ERROR) << "error writing local crash report (" << database_status << ")";
          return fit::error();
        }

        if (!UploadReport(local_report_id, component_url, &annotations,
                          /*read_annotations_from_minidump=*/false)) {
          return fit::error();
        }
        return fit::ok();
      });
}

fit::promise<void> CrashpadAgent::OnKernelPanicCrashLog(fuchsia::mem::Buffer crash_log) {
  FX_LOGS(INFO) << "generating crash report for previous kernel panic";

  // Create local crash report.
  std::unique_ptr<crashpad::CrashReportDatabase::NewReport> report;
  const crashpad::CrashReportDatabase::OperationStatus database_status =
      database_->PrepareNewCrashReport(&report);
  if (database_status != crashpad::CrashReportDatabase::kNoError) {
    FX_LOGS(ERROR) << "error creating local crash report (" << database_status << ")";
    return fit::make_error_promise();
  }

  // Prepare annotations and attachments.
  return GetFeedbackData(dispatcher_, services_,
                         zx::msec(config_.feedback_data_collection_timeout_in_milliseconds))
      .then([this, crash_log = std::move(crash_log),
             report = std::move(report)](fit::result<Data>& result) mutable -> fit::result<void> {
        Data feedback_data;
        if (result.is_ok()) {
          feedback_data = result.take_value();
        }
        const std::map<std::string, std::string> annotations =
            MakeDefaultAnnotations(feedback_data,
                                   /*program_name=*/kKernelProgramName);
        AddKernelPanicAttachments(report.get(), feedback_data, std::move(crash_log));

        // Finish new local crash report.
        crashpad::UUID local_report_id;
        const crashpad::CrashReportDatabase::OperationStatus database_status =
            database_->FinishedWritingCrashReport(std::move(report), &local_report_id);
        if (database_status != crashpad::CrashReportDatabase::kNoError) {
          FX_LOGS(ERROR) << "error writing local crash report (" << database_status << ")";
          return fit::error();
        }

        if (!UploadReport(local_report_id, kKernelProgramName, &annotations,
                          /*read_annotations_from_minidump=*/false)) {
          return fit::error();
        }
        return fit::ok();
      });
}

fit::promise<void> CrashpadAgent::File(fuchsia::feedback::CrashReport report) {
  const std::string program_name = ExtractProgramName(report);
  FX_LOGS(INFO) << "generating crash report for " << program_name;

  // Create local Crashpad report.
  std::unique_ptr<crashpad::CrashReportDatabase::NewReport> crashpad_report;
  if (CrashReportDatabase::OperationStatus status =
          database_->PrepareNewCrashReport(&crashpad_report);
      status != crashpad::CrashReportDatabase::kNoError) {
    FX_LOGS(ERROR) << "error creating local Crashpad report (" << status << ")";
    return fit::make_error_promise();
  }

  return GetFeedbackData(dispatcher_, services_,
                         zx::msec(config_.feedback_data_collection_timeout_in_milliseconds))
      .then([this, report = std::move(report), program_name = std::move(program_name),
             crashpad_report = std::move(crashpad_report)](
                fit::result<Data>& result) mutable -> fit::result<void> {
        Data feedback_data;
        if (result.is_ok()) {
          feedback_data = result.take_value();
        }

        const std::map<std::string, std::string> annotations =
            BuildAnnotations(report, feedback_data);
        BuildAttachments(report, feedback_data, crashpad_report.get());

        // Finish new local crash report.
        crashpad::UUID local_report_id;
        if (CrashReportDatabase::OperationStatus status =
                database_->FinishedWritingCrashReport(std::move(crashpad_report), &local_report_id);
            status != crashpad::CrashReportDatabase::kNoError) {
          FX_LOGS(ERROR) << "error writing local Crashpad report (" << status << ")";
          return fit::error();
        }

        if (!UploadReport(local_report_id, program_name, &annotations,
                          /*read_annotations_from_minidump=*/false)) {
          return fit::error();
        }
        return fit::ok();
      });
}

bool CrashpadAgent::UploadReport(const crashpad::UUID& local_report_id,
                                 const std::string& program_name,
                                 const std::map<std::string, std::string>* annotations,
                                 bool read_annotations_from_minidump) {
  InspectManager::Report* inspect_report =
      inspect_manager_->AddReport(program_name, local_report_id);

  bool uploads_enabled;
  if ((!database_->GetSettings()->GetUploadsEnabled(&uploads_enabled) || !uploads_enabled)) {
    FX_LOGS(INFO) << "upload to remote crash server disabled. Local crash report, ID "
                  << local_report_id.ToString() << ", available under "
                  << config_.crashpad_database.path;
    database_->SkipReportUpload(local_report_id,
                                crashpad::Metrics::CrashSkippedReason::kUploadsDisabled);
    return true;
  }

  // Read local crash report as an "upload" report.
  std::unique_ptr<const crashpad::CrashReportDatabase::UploadReport> report;
  const crashpad::CrashReportDatabase::OperationStatus database_status =
      database_->GetReportForUploading(local_report_id, &report);
  if (database_status != crashpad::CrashReportDatabase::kNoError) {
    FX_LOGS(ERROR) << "error loading local crash report, ID " << local_report_id.ToString() << " ("
                   << database_status << ")";
    return false;
  }

  // Set annotations, either from argument or from minidump.
  //
  // TODO(DX-1785): remove minidump annotation support here once BuildAnnotations() supports it.
  FXL_CHECK((annotations != nullptr) ^ read_annotations_from_minidump);
  const std::map<std::string, std::string>* final_annotations = annotations;
  std::map<std::string, std::string> minidump_annotations;
  if (read_annotations_from_minidump) {
    crashpad::FileReader* reader = report->Reader();
    crashpad::FileOffset start_offset = reader->SeekGet();
    crashpad::ProcessSnapshotMinidump minidump_process_snapshot;
    if (!minidump_process_snapshot.Initialize(reader)) {
      report.reset();
      database_->SkipReportUpload(local_report_id,
                                  crashpad::Metrics::CrashSkippedReason::kPrepareForUploadFailed);
      FX_LOGS(ERROR) << "error processing minidump for local crash report, ID "
                     << local_report_id.ToString();
      return false;
    }
    minidump_annotations =
        crashpad::BreakpadHTTPFormParametersFromMinidump(&minidump_process_snapshot);
    final_annotations = &minidump_annotations;
    if (!reader->SeekSet(start_offset)) {
      report.reset();
      database_->SkipReportUpload(local_report_id,
                                  crashpad::Metrics::CrashSkippedReason::kPrepareForUploadFailed);
      FX_LOGS(ERROR) << "error processing minidump for local crash report, ID "
                     << local_report_id.ToString();
      return false;
    }
  }

  // We have to build the MIME multipart message ourselves as all the public Crashpad helpers are
  // asynchronous and we won't be able to know the upload status nor the server report ID.
  crashpad::HTTPMultipartBuilder http_multipart_builder;
  http_multipart_builder.SetGzipEnabled(true);
  for (const auto& kv : *final_annotations) {
    http_multipart_builder.SetFormData(kv.first, kv.second);
  }
  for (const auto& kv : report->GetAttachments()) {
    http_multipart_builder.SetFileAttachment(kv.first, kv.first, kv.second,
                                             "application/octet-stream");
  }
  http_multipart_builder.SetFileAttachment("uploadFileMinidump", report->uuid.ToString() + ".dmp",
                                           report->Reader(), "application/octet-stream");
  crashpad::HTTPHeaders content_headers;
  http_multipart_builder.PopulateContentHeaders(&content_headers);

  std::string server_report_id;
  if (!crash_server_->MakeRequest(content_headers, http_multipart_builder.GetBodyStream(),
                                  &server_report_id)) {
    report.reset();
    database_->SkipReportUpload(local_report_id,
                                crashpad::Metrics::CrashSkippedReason::kUploadFailed);
    FX_LOGS(ERROR) << "error uploading local crash report, ID " << local_report_id.ToString();
    return false;
  }
  database_->RecordUploadComplete(std::move(report), server_report_id);
  inspect_report->MarkUploaded(server_report_id);
  FX_LOGS(INFO) << "successfully uploaded crash report at "
                   "https://crash.corp.google.com/"
                << server_report_id;

  return true;
}

void CrashpadAgent::PruneDatabase() {
  // We need to create a new condition every time we prune as it internally maintains a cumulated
  // total size as it iterates over the reports in the database and we want to reset that cumulated
  // total size every time we prune.
  crashpad::DatabaseSizePruneCondition pruning_condition(config_.crashpad_database.max_size_in_kb);
  crashpad::PruneCrashReportDatabase(database_.get(), &pruning_condition);
}

}  // namespace crash
}  // namespace fuchsia
