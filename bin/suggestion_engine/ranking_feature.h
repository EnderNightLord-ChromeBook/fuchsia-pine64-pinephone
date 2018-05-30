// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURE_H_
#define PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURE_H_

#include <fuchsia/modular/cpp/fidl.h>
#include "peridot/bin/suggestion_engine/ranked_suggestion.h"
#include "third_party/rapidjson/rapidjson/document.h"

namespace fuchsia {
namespace modular {

constexpr double kMaxConfidence = 1.0;
constexpr double kMinConfidence = 0.0;

class RankingFeature {
 public:
  RankingFeature();
  virtual ~RankingFeature();

  // Compute the numeric value for a feature, ensuring bounds on the result
  // in the range of [0.0,1.0]
  double ComputeFeature(const UserInput& query,
                        const RankedSuggestion& suggestion);

  // Fills the context selector with the values and meta the feature needs to
  // request from the context. Returns true if it filled anything, false
  // otherwise.
  ContextSelectorPtr CreateContextSelector();

  // Updates the context that the feature needs.
  void UpdateContext(const fidl::VectorPtr<ContextValue>& context_update_values);

 protected:
  // Compute the numeric feature for a feature, to be overridden by subclasses
  virtual double ComputeFeatureInternal(const UserInput& query,
                                        const RankedSuggestion& suggestion) = 0;

  // Create the context selector. Returns nullptr if the feature doesn't require
  // context.
  virtual ContextSelectorPtr CreateContextSelectorInternal();

  // Loads a json file into the second value of the pair. The first value
  // indicates if the file could be loaded and parsed.
  std::pair<bool, rapidjson::Document> FetchJsonObject(const std::string& path);

  // Returns current context values the ranking feature has.
  fidl::VectorPtr<ContextValue>& ContextValues();

 private:
  static int instances_;
  fidl::VectorPtr<ContextValue> context_values_;
  const int id_;
};

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURE_H_
