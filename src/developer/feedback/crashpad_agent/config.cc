// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/developer/feedback/crashpad_agent/config.h"

#include <lib/syslog/cpp/logger.h>
#include <zircon/errors.h>

#include "src/lib/files/file.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "third_party/rapidjson/include/rapidjson/schema.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"

namespace fuchsia {
namespace crash {
namespace {

const char kSchema[] = R"({
  "type": "object",
  "properties": {
    "crashpad_database": {
      "type": "object",
      "properties": {
        "path": {
          "type": "string"
        },
        "max_size_in_kb": {
          "type": "integer"
        }
      },
      "required": [
        "path",
        "max_size_in_kb"
      ],
      "additionalProperties": false
    },
    "crash_server": {
      "type": "object",
      "properties": {
        "enable_upload": {
          "type": "boolean"
        },
        "url": {
          "type": "string"
        }
      },
      "required": [
        "enable_upload"
      ],
      "additionalProperties": false
    },
    "feedback_data_collection_timeout_in_milliseconds": {
      "type": "integer"
    }
  },
  "required": [
    "crashpad_database",
    "crash_server",
    "feedback_data_collection_timeout_in_milliseconds"
  ],
  "additionalProperties": false
})";

const char kCrashpadDatabaseKey[] = "crashpad_database";
const char kCrashpadDatabasePathKey[] = "path";
const char kCrashpadDatabaseMaxSizeInKbKey[] = "max_size_in_kb";
const char kCrashServerKey[] = "crash_server";
const char kCrashServerEnableUploadKey[] = "enable_upload";
const char kCrashServerUrlKey[] = "url";
const char kFeedbackDataCollectionTimeoutInSecondsKey[] =
    "feedback_data_collection_timeout_in_milliseconds";

bool CheckAgainstSchema(rapidjson::Document& doc) {
  // Check that the schema is actually valid.
  rapidjson::Document sd;
  rapidjson::ParseResult ok = sd.Parse(kSchema);
  if (!ok) {
    FX_LOGS(ERROR) << "invalid JSON schema for config at offset " << ok.Offset() << " "
                   << rapidjson::GetParseError_En(ok.Code());
    return false;
  }

  // Check the document against the schema.
  rapidjson::SchemaDocument schema(sd);
  rapidjson::SchemaValidator validator(schema);
  if (!doc.Accept(validator)) {
    rapidjson::StringBuffer sb;
    validator.GetInvalidSchemaPointer().StringifyUriFragment(sb);
    FX_LOGS(ERROR) << "config does not match schema, violating '"
                   << validator.GetInvalidSchemaKeyword() << "' rule";
    return false;
  }
  return true;
}

template <typename JsonObject>
CrashpadDatabaseConfig ParseCrashpadDatabaseConfig(const JsonObject& obj) {
  CrashpadDatabaseConfig config;
  config.path = obj[kCrashpadDatabasePathKey].GetString();
  config.max_size_in_kb = obj[kCrashpadDatabaseMaxSizeInKbKey].GetUint();
  return config;
}

template <typename JsonObject>
bool ParseCrashServerConfig(const JsonObject& obj, CrashServerConfig* config) {
  CrashServerConfig local_config;
  local_config.enable_upload = obj[kCrashServerEnableUploadKey].GetBool();

  if (local_config.enable_upload) {
    if (!obj.HasMember(kCrashServerUrlKey)) {
      FX_LOGS(ERROR) << "missing crash server URL in config with upload enabled";
      return false;
    }
    local_config.url = std::make_unique<std::string>(obj[kCrashServerUrlKey].GetString());
  } else if (obj.HasMember(kCrashServerUrlKey)) {
    FX_LOGS(WARNING) << "crash server URL set in config with upload disabled, "
                        "ignoring value";
  }

  *config = std::move(local_config);
  return true;
}

}  // namespace

zx_status_t ParseConfig(const std::string& filepath, Config* config) {
  std::string json;
  if (!files::ReadFileToString(filepath, &json)) {
    FX_LOGS(ERROR) << "error reading config file at " << filepath;
    return ZX_ERR_IO;
  }

  rapidjson::Document doc;
  rapidjson::ParseResult ok = doc.Parse(json.c_str());
  if (!ok) {
    FX_LOGS(ERROR) << "error parsing config as JSON at offset " << ok.Offset() << " "
                   << rapidjson::GetParseError_En(ok.Code());
    return ZX_ERR_INTERNAL;
  }

  if (!CheckAgainstSchema(doc)) {
    return ZX_ERR_INTERNAL;
  }

  // We use a local config to only set the out argument after all the checks.
  Config local_config;
  // It is safe to directly access the fields for which the keys are marked as required as we have
  // checked the config against the schema.

  local_config.crashpad_database =
      ParseCrashpadDatabaseConfig(doc[kCrashpadDatabaseKey].GetObject());
  if (!ParseCrashServerConfig(doc[kCrashServerKey].GetObject(), &local_config.crash_server)) {
    return ZX_ERR_INTERNAL;
  }
  local_config.feedback_data_collection_timeout_in_milliseconds =
      doc[kFeedbackDataCollectionTimeoutInSecondsKey].GetUint();

  *config = std::move(local_config);
  return ZX_OK;
}

}  // namespace crash
}  // namespace fuchsia
