// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/system_monitor/dockyard/dockyard.h"
#include "garnet/lib/system_monitor/dockyard/dockyard_service_impl.h"
#include "garnet/lib/system_monitor/gt_log.h"

// The code below is not the real Dockyard. They are test/mock functions.
// See //garnet/lib/system_monitor/dockyard/dockyard.cc for the actual code.
namespace dockyard {

uint64_t RequestId::next_request_id_;

Dockyard::Dockyard() {}

Dockyard::~Dockyard() {}

void Dockyard::AddSample(DockyardId dockyard_id, Sample sample) {}

void Dockyard::AddSamples(DockyardId dockyard_id, std::vector<Sample> samples) {}

SampleTimeNs Dockyard::DeviceDeltaTimeNs() const { return 0; }

void Dockyard::SetDeviceTimeDeltaNs(SampleTimeNs delta_ns) {}

SampleTimeNs Dockyard::LatestSampleTimeNs() const { return 0; }

DockyardId Dockyard::GetDockyardId(const std::string& dockyard_path) { return 0; }
bool Dockyard::GetDockyardPath(DockyardId dockyard_id, std::string* dockyard_path) const {
  return false;
}
DockyardPathToIdMap Dockyard::MatchPaths(const std::string& starting,
                                         const std::string& ending) const {
  DockyardPathToIdMap result;
  return result;
}
bool Dockyard::HasDockyardPath(const std::string& dockyard_path, DockyardId* dockyard_id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto search = dockyard_path_to_id_.find(dockyard_path);
  if (search != dockyard_path_to_id_.end()) {
    *dockyard_id = search->second;
    return true;
  }
  return false;
}

void Dockyard::ResetHarvesterData() {
  std::lock_guard<std::mutex> guard(mutex_);
  device_time_delta_ns_ = 0;
  latest_sample_time_ns_ = 0;

  // Maybe send error responses.
  pending_get_requests_.clear();
  pending_discard_requests_.clear();

  sample_streams_.clear();
  sample_stream_low_high_.clear();

  dockyard_path_to_id_.clear();
  dockyard_id_to_path_.clear();

  DockyardId dockyard_id = GetDockyardId("<INVALID>");
  if (dockyard_id != INVALID_DOCKYARD_ID) {
    GT_LOG(ERROR) << "INVALID_DOCKYARD_ID string allocation failed. Exiting.";
    exit(1);
  }
}

void Dockyard::GetStreamSets(StreamSetsRequest* request) {}

void Dockyard::OnConnection() {}

void Dockyard::StartCollectingFrom(const std::string& device) {}

void Dockyard::StopCollectingFromDevice() {}

OnConnectionCallback Dockyard::SetConnectionHandler(OnConnectionCallback callback) {
  on_connection_handler_ = callback;
  return nullptr;
}

OnPathsCallback Dockyard::SetDockyardPathsHandler(OnPathsCallback callback) {
  on_paths_handler_ = callback;
  return nullptr;
}

OnStreamSetsCallback Dockyard::SetStreamSetsHandler(OnStreamSetsCallback callback) {
  on_stream_sets_handler_ = callback;
  return nullptr;
}

void Dockyard::ProcessRequests() {}

std::ostringstream Dockyard::DebugDump() const {
  std::ostringstream out;
  out << "Fake Dockyard::DebugDump" << std::endl;
  return out;
}

std::ostream& operator<<(std::ostream& out, const StreamSetsRequest& request) { return out; }

std::ostream& operator<<(std::ostream& out, const StreamSetsResponse& response) { return out; }

std::ostringstream DebugPrintQuery(const Dockyard& dockyard, const StreamSetsRequest& request,
                                   const StreamSetsResponse& response) {
  std::ostringstream out;
  return out;
}

}  // namespace dockyard
