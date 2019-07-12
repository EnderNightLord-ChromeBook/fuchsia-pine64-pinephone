// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "inspect.h"

#include "lib/inspect_deprecated/hierarchy.h"

using inspect_deprecated::hierarchy::Metric;
using inspect_deprecated::hierarchy::Node;
using inspect_deprecated::hierarchy::Property;

namespace {}  // namespace

namespace inspect_deprecated {

namespace hierarchy {
void PrintTo(const Metric& metric, std::ostream* os) {
  if (metric.format() == MetricFormat::INT)
    *os << "Int";
  if (metric.format() == MetricFormat::UINT)
    *os << "UInt";
  if (metric.format() == MetricFormat::DOUBLE)
    *os << "Double";
  if (metric.format() == MetricFormat::INT_ARRAY)
    *os << "IntArray";
  if (metric.format() == MetricFormat::UINT_ARRAY)
    *os << "UIntArray";
  if (metric.format() == MetricFormat::DOUBLE_ARRAY)
    *os << "DoubleArray";
  *os << "Metric(" << ::testing::PrintToString(metric.name()) << ", ";
  if (metric.format() == MetricFormat::INT)
    *os << ::testing::PrintToString(metric.Get<IntMetric>().value());
  if (metric.format() == MetricFormat::UINT)
    *os << ::testing::PrintToString(metric.Get<UIntMetric>().value());
  if (metric.format() == MetricFormat::DOUBLE)
    *os << ::testing::PrintToString(metric.Get<DoubleMetric>().value());
  if (metric.format() == MetricFormat::INT_ARRAY)
    *os << ::testing::PrintToString(metric.Get<IntArray>().value());
  if (metric.format() == MetricFormat::UINT_ARRAY)
    *os << ::testing::PrintToString(metric.Get<UIntArray>().value());
  if (metric.format() == MetricFormat::DOUBLE_ARRAY)
    *os << ::testing::PrintToString(metric.Get<DoubleArray>().value());
  *os << ")";
}

void PrintTo(const Property& property, std::ostream* os) {
  if (property.format() == PropertyFormat::STRING)
    *os << "String";
  if (property.format() == PropertyFormat::BYTES)
    *os << "ByteVector";
  *os << "Property(" << ::testing::PrintToString(property.name()) << ", ";
  if (property.format() == PropertyFormat::STRING)
    *os << ::testing::PrintToString(property.Get<StringProperty>().value());
  if (property.format() == PropertyFormat::BYTES)
    *os << ::testing::PrintToString(property.Get<ByteVectorProperty>().value());
  *os << ")";
}

void PrintTo(const Node& node, std::ostream* os) {
  *os << "Node(" << ::testing::PrintToString(node.name()) << ", "
      << ::testing::PrintToString(node.metrics().size()) << " metrics, "
      << ::testing::PrintToString(node.properties().size()) << " properties)";
}
}  // namespace hierarchy

void PrintTo(const ::inspect_deprecated::ObjectHierarchy& hierarchy, std::ostream* os) {
  *os << "ObjectHierarchy(" << ::testing::PrintToString(hierarchy.node()) << ", "
      << ::testing::PrintToString(hierarchy.children().size()) << " children)";
}

namespace testing {

internal::NameMatchesMatcher::NameMatchesMatcher(std::string name) : name_(std::move(name)) {}

bool internal::NameMatchesMatcher::MatchAndExplain(const hierarchy::Node& node,
                                                   ::testing::MatchResultListener* listener) const {
  if (node.name() != name_) {
    *listener << "expected name \"" << name_ << "\" but found \"" << node.name() << "\"";
    return false;
  }
  return true;
}

void internal::NameMatchesMatcher::DescribeTo(::std::ostream* os) const {
  *os << "name matches \"" << name_ << "\"";
}

void internal::NameMatchesMatcher::DescribeNegationTo(::std::ostream* os) const {
  *os << "name does not match \"" << name_ << "\"";
}

internal::MetricListMatcher::MetricListMatcher(MetricsMatcher matcher)
    : matcher_(std::move(matcher)) {}

bool internal::MetricListMatcher::MatchAndExplain(const hierarchy::Node& node,
                                                  ::testing::MatchResultListener* listener) const {
  return ::testing::ExplainMatchResult(matcher_, node.metrics(), listener);
}

void internal::MetricListMatcher::DescribeTo(::std::ostream* os) const {
  *os << "metric list ";
  matcher_.DescribeTo(os);
}

void internal::MetricListMatcher::DescribeNegationTo(::std::ostream* os) const {
  *os << "metric list ";
  matcher_.DescribeNegationTo(os);
}

internal::PropertyListMatcher::PropertyListMatcher(PropertiesMatcher matcher)
    : matcher_(std::move(matcher)) {}

bool internal::PropertyListMatcher::MatchAndExplain(
    const hierarchy::Node& node, ::testing::MatchResultListener* listener) const {
  return ::testing::ExplainMatchResult(matcher_, node.properties(), listener);
}

void internal::PropertyListMatcher::DescribeTo(::std::ostream* os) const {
  *os << "property list ";
  matcher_.DescribeTo(os);
}

void internal::PropertyListMatcher::DescribeNegationTo(::std::ostream* os) const {
  *os << "property list ";
  matcher_.DescribeNegationTo(os);
}

::testing::Matcher<const hierarchy::Node&> NameMatches(std::string name) {
  return ::testing::MakeMatcher(new internal::NameMatchesMatcher(std::move(name)));
}

::testing::Matcher<const hierarchy::Node&> MetricList(MetricsMatcher matcher) {
  return ::testing::MakeMatcher(new internal::MetricListMatcher(std::move(matcher)));
}

::testing::Matcher<const hierarchy::Node&> PropertyList(PropertiesMatcher matcher) {
  return ::testing::MakeMatcher(new internal::PropertyListMatcher(std::move(matcher)));
}

::testing::Matcher<const Property&> StringPropertyIs(const std::string& name,
                                                     const std::string& value) {
  return ::testing::AllOf(::testing::Property(&Property::name, ::testing::StrEq(name)),
                          ::testing::Property(&Property::format, hierarchy::PropertyFormat::STRING),
                          ::testing::Property(&Property::Get<hierarchy::StringProperty>,
                                              ::testing::Property(&hierarchy::StringProperty::value,
                                                                  ::testing::StrEq(value))));
}

::testing::Matcher<const Property&> ByteVectorPropertyIs(const std::string& name,
                                                         const VectorValue& value) {
  return ::testing::AllOf(
      ::testing::Property(&Property::name, ::testing::StrEq(name)),
      ::testing::Property(&Property::format, hierarchy::PropertyFormat::BYTES),
      ::testing::Property(
          &Property::Get<hierarchy::ByteVectorProperty>,
          ::testing::Property(&hierarchy::ByteVectorProperty::value, ::testing::Eq(value))));
}

::testing::Matcher<const Metric&> IntMetricIs(const std::string& name, int64_t value) {
  return ::testing::AllOf(
      ::testing::Property(&Metric::name, ::testing::StrEq(name)),
      ::testing::Property(&Metric::format, hierarchy::MetricFormat::INT),
      ::testing::Property(&Metric::Get<hierarchy::IntMetric>,
                          ::testing::Property(&hierarchy::IntMetric::value, ::testing::Eq(value))));
}

::testing::Matcher<const Metric&> UIntMetricIs(const std::string& name, uint64_t value) {
  return ::testing::AllOf(::testing::Property(&Metric::name, ::testing::StrEq(name)),
                          ::testing::Property(&Metric::format, hierarchy::MetricFormat::UINT),
                          ::testing::Property(&Metric::Get<hierarchy::UIntMetric>,
                                              ::testing::Property(&hierarchy::UIntMetric::value,
                                                                  ::testing::Eq(value))));
}

::testing::Matcher<const Metric&> DoubleMetricIs(const std::string& name, double value) {
  return ::testing::AllOf(::testing::Property(&Metric::name, ::testing::StrEq(name)),
                          ::testing::Property(&Metric::format, hierarchy::MetricFormat::DOUBLE),
                          ::testing::Property(&Metric::Get<hierarchy::DoubleMetric>,
                                              ::testing::Property(&hierarchy::DoubleMetric::value,
                                                                  ::testing::Eq(value))));
}

::testing::Matcher<const hierarchy::Metric&> IntArrayIs(
    const std::string& name, ::testing::Matcher<std::vector<int64_t>> matcher) {
  return ::testing::AllOf(
      ::testing::Property(&Metric::name, ::testing::StrEq(name)),
      ::testing::Property(&Metric::format, hierarchy::MetricFormat::INT_ARRAY),
      ::testing::Property(&Metric::Get<hierarchy::IntArray>,
                          ::testing::Property(&hierarchy::IntArray::value, std::move(matcher))));
}

::testing::Matcher<const hierarchy::Metric&> UIntArrayIs(
    const std::string& name, ::testing::Matcher<std::vector<uint64_t>> matcher) {
  return ::testing::AllOf(
      ::testing::Property(&Metric::name, ::testing::StrEq(name)),
      ::testing::Property(&Metric::format, hierarchy::MetricFormat::UINT_ARRAY),
      ::testing::Property(&Metric::Get<hierarchy::UIntArray>,
                          ::testing::Property(&hierarchy::UIntArray::value, std::move(matcher))));
}

::testing::Matcher<const hierarchy::Metric&> DoubleArrayIs(
    const std::string& name, ::testing::Matcher<std::vector<double>> matcher) {
  return ::testing::AllOf(
      ::testing::Property(&Metric::name, ::testing::StrEq(name)),
      ::testing::Property(&Metric::format, hierarchy::MetricFormat::DOUBLE_ARRAY),
      ::testing::Property(&Metric::Get<hierarchy::DoubleArray>,
                          ::testing::Property(&hierarchy::DoubleArray::value, std::move(matcher))));
}

::testing::Matcher<const hierarchy::Metric&> ArrayDisplayFormatIs(
    hierarchy::ArrayDisplayFormat format) {
  return ::testing::AnyOf(
      ::testing::AllOf(
          ::testing::Property(&Metric::format, hierarchy::MetricFormat::INT_ARRAY),
          ::testing::Property(&Metric::Get<hierarchy::IntArray>,
                              ::testing::Property(&hierarchy::IntArray::GetDisplayFormat, format))),
      ::testing::AllOf(::testing::Property(&Metric::format, hierarchy::MetricFormat::UINT_ARRAY),
                       ::testing::Property(
                           &Metric::Get<hierarchy::UIntArray>,
                           ::testing::Property(&hierarchy::UIntArray::GetDisplayFormat, format))),
      ::testing::AllOf(
          ::testing::Property(&Metric::format, hierarchy::MetricFormat::DOUBLE_ARRAY),
          ::testing::Property(
              &Metric::Get<hierarchy::DoubleArray>,
              ::testing::Property(&hierarchy::DoubleArray::GetDisplayFormat, format))));
}

::testing::Matcher<const ObjectHierarchy&> NodeMatches(NodeMatcher matcher) {
  return ::testing::Property(&ObjectHierarchy::node, std::move(matcher));
}

::testing::Matcher<const ObjectHierarchy&> ObjectMatches(NodeMatcher matcher) {
  return NodeMatches(std::move(matcher));
}

::testing::Matcher<const ObjectHierarchy&> ChildrenMatch(ChildrenMatcher matcher) {
  return ::testing::Property(&ObjectHierarchy::children, std::move(matcher));
}

}  // namespace testing
}  // namespace inspect_deprecated
