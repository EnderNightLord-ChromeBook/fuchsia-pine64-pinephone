// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/cloud_provider_firestore/bin/firestore/testing/test_firestore_service.h"

#include <lib/fit/function.h>

namespace cloud_provider_firestore {

namespace {
class TestListenCallHandler : public ListenCallHandler {
 public:
  TestListenCallHandler() {}

  ~TestListenCallHandler() override {}

  void Write(google::firestore::v1beta1::ListenRequest /*request*/) override {
    // do nothing
  }

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(TestListenCallHandler);
};

}  // namespace

TestFirestoreService::TestFirestoreService() : db_path_(), root_path_() {}
TestFirestoreService::~TestFirestoreService() {}

const std::string& TestFirestoreService::GetDatabasePath() { return db_path_; }

const std::string& TestFirestoreService::GetRootPath() { return root_path_; }

void TestFirestoreService::GetDocument(
    google::firestore::v1beta1::GetDocumentRequest request,
    std::shared_ptr<grpc::CallCredentials> /*call_credentials*/,
    fit::function<void(grpc::Status, google::firestore::v1beta1::Document)> callback) {
  FXL_DCHECK(!shutdown_callback);
  get_document_records.push_back({std::move(request), std::move(callback)});
}

void TestFirestoreService::ListDocuments(
    google::firestore::v1beta1::ListDocumentsRequest request,
    std::shared_ptr<grpc::CallCredentials> /*call_credentials*/,
    fit::function<void(grpc::Status, google::firestore::v1beta1::ListDocumentsResponse)> callback) {
  FXL_DCHECK(!shutdown_callback);
  list_documents_records.push_back({std::move(request), std::move(callback)});
}

void TestFirestoreService::CreateDocument(
    google::firestore::v1beta1::CreateDocumentRequest request,
    std::shared_ptr<grpc::CallCredentials> /*call_credentials*/,
    fit::function<void(grpc::Status, google::firestore::v1beta1::Document)> callback) {
  FXL_DCHECK(!shutdown_callback);
  create_document_records.push_back({std::move(request), std::move(callback)});
}

void TestFirestoreService::DeleteDocument(
    google::firestore::v1beta1::DeleteDocumentRequest request,
    std::shared_ptr<grpc::CallCredentials> /*call_credentials*/,
    fit::function<void(grpc::Status)> callback) {
  FXL_DCHECK(!shutdown_callback);
  delete_document_records.push_back({std::move(request), std::move(callback)});
}

void TestFirestoreService::RunQuery(
    google::firestore::v1beta1::RunQueryRequest request,
    std::shared_ptr<grpc::CallCredentials> call_credentials,
    fit::function<void(grpc::Status, std::vector<google::firestore::v1beta1::RunQueryResponse>)>
        callback) {
  FXL_DCHECK(!shutdown_callback);
  run_query_records.push_back({std::move(request), std::move(callback)});
}

void TestFirestoreService::Commit(
    google::firestore::v1beta1::CommitRequest request,
    std::shared_ptr<grpc::CallCredentials> /*call_credentials*/,
    fit::function<void(grpc::Status, google::firestore::v1beta1::CommitResponse)> callback) {
  FXL_DCHECK(!shutdown_callback);
  commit_records.push_back({std::move(request), std::move(callback)});
}

std::unique_ptr<ListenCallHandler> TestFirestoreService::Listen(
    std::shared_ptr<grpc::CallCredentials> /*call_credentials*/, ListenCallClient* client) {
  FXL_DCHECK(!shutdown_callback);
  listen_clients.push_back(client);
  return std::make_unique<TestListenCallHandler>();
}

void TestFirestoreService::ShutDown(fit::closure callback) {
  FXL_DCHECK(!shutdown_callback);
  shutdown_callback = std::move(callback);
}

}  // namespace cloud_provider_firestore
