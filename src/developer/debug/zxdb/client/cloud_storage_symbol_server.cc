// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/cloud_storage_symbol_server.h"

#include <cstdio>
#include <filesystem>

#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/writer.h"
#include "src/developer/debug/shared/message_loop.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/setting_schema_definition.h"
#include "src/developer/debug/zxdb/common/string_util.h"
#include "src/lib/fxl/logging.h"

namespace zxdb {
namespace {

constexpr char kClientId[] =
    "446450136466-2hr92jrq8e6i4tnsa56b52vacp7t3936"
    ".apps.googleusercontent.com";
constexpr char kClientSecret[] = "uBfbay2KCy9t4QveJ-dOqHtp";

constexpr char kAuthServer[] = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr char kScope[] = "https://www.googleapis.com/auth/devstorage.read_only";
constexpr char kTokenServer[] = "https://www.googleapis.com/oauth2/v4/token";

bool DocIsAuthInfo(const rapidjson::Document& document) {
  return !document.HasParseError() && document.IsObject() && document.HasMember("access_token");
}

std::string ToDebugFileName(const std::string& name, DebugSymbolFileType file_type) {
  if (file_type == DebugSymbolFileType::kDebugInfo) {
    return name + ".debug";
  }

  return name;
}

std::filesystem::path GetCachePath() {
  static std::filesystem::path ret;

  if (!ret.empty()) {
    return ret;
  }

  auto home = std::getenv("HOME");

#ifdef __APPLE__
  if (home) {
    ret = std::filesystem::path(home) / "Library" / "Caches" / "com.googlesource.fuchsia.zxdb";
  }
#else
  auto xdg = getenv("XDG_CACHE_HOME");

  if (xdg) {
    ret = std::filesystem::path(xdg) / "zxdb";
  } else if (home) {
    ret = std::filesystem::path(home) / ".cache" / "zxdb";
  }
#endif

  return ret;
}

FILE* GetGoogleApiAuthCache(const char* mode) {
  auto path = GetCachePath();

  std::error_code ec;
  std::filesystem::create_directories(path, ec);

  if (!std::filesystem::is_directory(path, ec)) {
    return nullptr;
  }

  return fopen(std::string(path / "googleapi_auth").c_str(), mode);
}

class CloudStorageSymbolServerImpl : public CloudStorageSymbolServer {
 public:
  CloudStorageSymbolServerImpl(Session* session, const std::string& url)
      : CloudStorageSymbolServer(session, url) {
    DoInit();
  }

  void Fetch(const std::string& build_id, DebugSymbolFileType file_type,
             SymbolServer::FetchCallback cb) override;
  void CheckFetch(const std::string& build_id, DebugSymbolFileType file_type,
                  SymbolServer::CheckFetchCallback cb) override;

 private:
  void DoAuthenticate(const std::map<std::string, std::string>& data,
                      fit::callback<void(const Err&)> cb) override;
  std::shared_ptr<Curl> PrepareCurl(const std::string& build_id, DebugSymbolFileType file_type);
  void FetchWithCurl(const std::string& build_id, DebugSymbolFileType file_type,
                     std::shared_ptr<Curl> curl, SymbolServer::FetchCallback cb);
};

}  // namespace

CloudStorageSymbolServer::CloudStorageSymbolServer(Session* session, const std::string& url)
    : SymbolServer(session, url) {
  // Strip off the protocol identifier, yielding only the bucket name.
  bucket_ = url.substr(5);

  if (bucket_.back() != '/') {
    bucket_ += "/";
  }
}

std::unique_ptr<CloudStorageSymbolServer> CloudStorageSymbolServer::Impl(Session* session,
                                                                         const std::string& url) {
  return std::make_unique<CloudStorageSymbolServerImpl>(session, url);
}

bool CloudStorageSymbolServer::HandleRequestResult(Curl::Error result, long response_code,
                                                   size_t previous_ready_count, Err* out_err) {
  if (!result && response_code == 200) {
    return true;
  }

  if (state() != SymbolServer::State::kReady || previous_ready_count != ready_count_) {
    return false;
  }

  if (result) {
    *out_err = Err("Could not contact server: " + result.ToString());
  } else if (response_code == 401) {
    *out_err = Err("Authentication expired.");
    return false;
  } else if (response_code == 404 || response_code == 410) {
    return false;
  } else {
    *out_err = Err("Unexpected response: " + std::to_string(response_code));
  }

  error_log_.push_back(out_err->msg());
  IncrementRetries();

  return false;
}

std::string CloudStorageSymbolServer::AuthInfo() const {
  static std::string result;
  static const std::string kEmpty;

  if (state() != SymbolServer::State::kAuth) {
    return kEmpty;
  }

  if (!result.empty()) {
    return result;
  }

  Curl curl;

  result = kAuthServer;
  result += "?client_id=";
  result += curl.Escape(kClientId);
  result += "&redirect_uri=urn:ietf:wg:oauth:2.0:oob";
  result += "&response_type=code";
  result += "&scope=";
  result += curl.Escape(kScope);

  return result;
}

void CloudStorageSymbolServerImpl::DoAuthenticate(
    const std::map<std::string, std::string>& post_data, fit::callback<void(const Err&)> cb) {
  ChangeState(SymbolServer::State::kBusy);

  auto curl = Curl::MakeShared();

  curl->SetURL(kTokenServer);
  curl->set_post_data(post_data);

  auto document = std::make_shared<rapidjson::Document>();
  curl->set_data_callback([document](const std::string& data) {
    document->Parse(data);
    return data.size();
  });

  curl->Perform([this, cb = std::move(cb), document](Curl*, Curl::Error result) mutable {
    if (result) {
      std::string error = "Could not contact authentication server: ";
      error += result.ToString();

      error_log_.push_back(error);
      ChangeState(SymbolServer::State::kAuth);
      cb(Err(error));
      return;
    }

    if (!DocIsAuthInfo(*document)) {
      error_log_.push_back("Authentication failed");
      ChangeState(SymbolServer::State::kAuth);
      cb(Err("Authentication failed"));
      return;
    }

    access_token_ = (*document)["access_token"].GetString();

    bool new_refresh = false;
    if (document->HasMember("refresh_token")) {
      new_refresh = true;
      refresh_token_ = (*document)["refresh_token"].GetString();
    }

    if (document->HasMember("expires_in")) {
      constexpr int kMilli = 1000;
      int time = (*document)["expires_in"].GetInt();

      if (time > 1000) {
        time -= 100;
      }

      time *= kMilli;
      debug_ipc::MessageLoop::Current()->PostTimer(FROM_HERE, time, [this]() { AuthRefresh(); });
    }

    ChangeState(SymbolServer::State::kReady);
    cb(Err());

    if (!new_refresh) {
      return;
    }

    if (FILE* fp = GetGoogleApiAuthCache("wb")) {
      fwrite(refresh_token_.data(), 1, refresh_token_.size(), fp);
      fclose(fp);
    }
  });
}

void CloudStorageSymbolServer::Authenticate(const std::string& data,
                                            fit::callback<void(const Err&)> cb) {
  if (state() != SymbolServer::State::kAuth) {
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb)]() mutable { cb(Err("Authentication not required.")); });
    return;
  }

  std::map<std::string, std::string> post_data;
  post_data["code"] = data;
  post_data["client_id"] = kClientId;
  post_data["client_secret"] = kClientSecret;
  post_data["redirect_uri"] = "urn:ietf:wg:oauth:2.0:oob";
  post_data["grant_type"] = "authorization_code";

  DoAuthenticate(post_data, std::move(cb));
}

void CloudStorageSymbolServer::AuthRefresh() {
  std::map<std::string, std::string> post_data;
  post_data["refresh_token"] = refresh_token_;
  post_data["client_id"] = kClientId;
  post_data["client_secret"] = kClientSecret;
  post_data["grant_type"] = "refresh_token";

  DoAuthenticate(post_data, [](const Err& err) {});
}

void CloudStorageSymbolServer::LoadCachedAuth() {
  if (state() != SymbolServer::State::kAuth && state() != SymbolServer::State::kInitializing) {
    return;
  }

  FILE* fp = GetGoogleApiAuthCache("rb");

  if (!fp) {
    ChangeState(SymbolServer::State::kAuth);
    return;
  }

  std::vector<char> buf(65536);
  buf.resize(fread(buf.data(), 1, buf.size(), fp));
  bool success = feof(fp);
  fclose(fp);

  if (!success) {
    ChangeState(SymbolServer::State::kAuth);
    return;
  }

  refresh_token_ = std::string(buf.data(), buf.data() + buf.size());

  ChangeState(SymbolServer::State::kBusy);

  AuthRefresh();
}

std::shared_ptr<Curl> CloudStorageSymbolServerImpl::PrepareCurl(const std::string& build_id,
                                                                DebugSymbolFileType file_type) {
  if (state() != SymbolServer::State::kReady) {
    return nullptr;
  }

  std::string url = "https://storage.googleapis.com/";
  url += bucket_ + ToDebugFileName(build_id, file_type);

  auto curl = Curl::MakeShared();
  FXL_DCHECK(curl);

  curl->SetURL(url);
  curl->headers().push_back(std::string("Authorization: Bearer ") + access_token_);

  return curl;
}

void CloudStorageSymbolServerImpl::CheckFetch(const std::string& build_id,
                                              DebugSymbolFileType file_type,
                                              SymbolServer::CheckFetchCallback cb) {
  auto curl = PrepareCurl(build_id, file_type);

  if (!curl) {
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb)]() mutable { cb(Err("Server not ready."), nullptr); });
    return;
  }

  curl->get_body() = false;

  size_t previous_ready_count = ready_count_;

  curl->Perform([this, build_id, file_type, curl, cb = std::move(cb), previous_ready_count](
                    Curl*, Curl::Error result) mutable {
    Err err;
    auto code = curl->ResponseCode();

    if (HandleRequestResult(result, code, previous_ready_count, &err)) {
      curl->get_body() = true;
      cb(Err(), [this, build_id, file_type, curl](SymbolServer::FetchCallback fcb) {
        FetchWithCurl(build_id, file_type, curl, std::move(fcb));
      });
      return;
    }

    cb(err, nullptr);
  });
}

void CloudStorageSymbolServerImpl::Fetch(const std::string& build_id, DebugSymbolFileType file_type,
                                         SymbolServer::FetchCallback cb) {
  auto curl = PrepareCurl(build_id, file_type);

  if (!curl) {
    debug_ipc::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb)]() mutable { cb(Err("Server not ready."), ""); });
    return;
  }

  FetchWithCurl(build_id, file_type, curl, std::move(cb));
}

void CloudStorageSymbolServerImpl::FetchWithCurl(const std::string& build_id,
                                                 DebugSymbolFileType file_type,
                                                 std::shared_ptr<Curl> curl, FetchCallback cb) {
  auto cache_path = session()->system().settings().GetString(ClientSettings::System::kSymbolCache);
  std::string path;

  if (!cache_path.empty()) {
    std::error_code ec;
    auto path_obj = std::filesystem::path(cache_path) / ".build-id";

    if (std::filesystem::is_directory(path_obj, ec)) {
      // Download to a temporary file, so if we get cancelled (or we get sent
      // a 404 page instead of the real symbols) we don't pollute the build ID
      // folder.
      std::string name = ToDebugFileName(build_id, file_type) + ".part";

      path = path_obj / name;
    }
  }

  FILE* file = nullptr;

  // We don't have a folder specified where downloaded symbols go. We'll just
  // drop it in tmp and at least you'll be able to use them for this session.
  if (path.empty()) {
    path = "/tmp/zxdb_downloaded_symbolsXXXXXX\0";
    int fd = mkstemp(path.data());
    path.pop_back();

    if (fd >= 0) {
      // Ownership of the fd is absorbed by fdopen.
      file = fdopen(fd, "wb");
    }
  } else {
    file = std::fopen(path.c_str(), "wb");
  }

  if (!file) {
    debug_ipc::MessageLoop::Current()->PostTask(FROM_HERE, [cb = std::move(cb)]() mutable {
      cb(Err("Error opening temporary file."), "");
    });
    return;
  }

  auto cleanup = [file, path, cache_path, build_id, file_type](bool valid,
                                                               Err* result) -> std::string {
    fclose(file);

    std::error_code ec;
    if (!valid) {
      std::filesystem::remove(path, ec);
      return "";
    }

    if (cache_path.empty()) {
      *result = Err("No symbol cache specified.");
      return path;
    }

    auto target_path = std::filesystem::path(cache_path) / ".build-id" / build_id.substr(0, 2);
    auto target_name = ToDebugFileName(build_id.substr(2), file_type);

    std::filesystem::create_directory(target_path, ec);
    if (std::filesystem::is_directory(target_path, ec)) {
      std::filesystem::rename(path, target_path / target_name, ec);
      return target_path / target_name;
    } else {
      *result = Err("Could not move file in to cache.");
      return path;
    }
  };

  curl->set_data_callback(
      [file](const std::string& data) { return std::fwrite(data.data(), 1, data.size(), file); });

  size_t previous_ready_count = ready_count_;

  curl->Perform([this, cleanup, build_id, cb = std::move(cb), previous_ready_count](
                    Curl* curl, Curl::Error result) mutable {
    Err err;
    bool valid = HandleRequestResult(result, curl->ResponseCode(), previous_ready_count, &err);

    std::string final_path = cleanup(valid, &err);
    cb(err, final_path);
  });
}

}  // namespace zxdb
