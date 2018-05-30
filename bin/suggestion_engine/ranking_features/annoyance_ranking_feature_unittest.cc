// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/suggestion_engine/ranking_features/annoyance_ranking_feature.h"
#include "gtest/gtest.h"

namespace fuchsia {
namespace modular {
namespace {

class AnnoyanceRankingFeatureTest : public ::testing::Test {
 protected:
  RankedSuggestion GetSuggestion(AnnoyanceType annoyance) {
    SuggestionDisplay display;
    display.annoyance = annoyance;
    Proposal proposal;
    proposal.display = std::move(display);
    SuggestionPrototype prototype;
    prototype.proposal = std::move(proposal);
    RankedSuggestion suggestion;
    suggestion.prototype = &prototype;
    return suggestion;
  }
  AnnoyanceRankingFeature annoyance_ranking_feature;
  UserInput query;
};

TEST_F(AnnoyanceRankingFeatureTest, ComputeFeatureAnnoyance) {
  auto suggestion = GetSuggestion(AnnoyanceType::INTERRUPT);
  double value = annoyance_ranking_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, kMaxConfidence);
}

TEST_F(AnnoyanceRankingFeatureTest, ComputeFeatureNonAnnoyance)  {
  auto suggestion = GetSuggestion(AnnoyanceType::NONE);
  double value = annoyance_ranking_feature.ComputeFeature(query, suggestion);
  EXPECT_EQ(value, kMinConfidence);
}

}  // namespace
}  // namespace modular
}  // namespace fuchsia
