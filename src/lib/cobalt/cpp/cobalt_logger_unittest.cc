// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/cobalt/cpp/cobalt_logger.h"

#include <lib/async/default.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/svc/cpp/service_provider_bridge.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <src/lib/fxl/macros.h>

#include "src/lib/cobalt/cpp/cobalt_logger_impl.h"

using fuchsia::cobalt::ReleaseStage;

namespace cobalt {
namespace {

constexpr char kFakeCobaltConfig[] = "FakeConfig";
constexpr int32_t kFakeCobaltMetricId = 2;

bool Equals(const OccurrenceEvent* e1, const OccurrenceEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->event_code() == e2->event_code();
}

bool Equals(const CountEvent* e1, const CountEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->event_code() == e2->event_code() &&
         e1->component() == e2->component() &&
         e1->period_duration_micros() == e2->period_duration_micros() && e1->count() == e2->count();
}

bool Equals(const ElapsedTimeEvent* e1, const ElapsedTimeEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->event_code() == e2->event_code() &&
         e1->component() == e2->component() && e1->elapsed_micros() == e2->elapsed_micros();
}

bool Equals(const FrameRateEvent* e1, const FrameRateEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->event_code() == e2->event_code() &&
         e1->component() == e2->component() && e1->fps() == e2->fps();
}

bool Equals(const MemoryUsageEvent* e1, const MemoryUsageEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->event_code() == e2->event_code() &&
         e1->component() == e2->component() && e1->bytes() == e2->bytes();
}

bool Equals(const StringUsedEvent* e1, const StringUsedEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->s() == e2->s();
}

bool Equals(const StartTimerEvent* e1, const StartTimerEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->event_code() == e2->event_code() &&
         e1->component() == e2->component() && e1->timer_id() == e2->timer_id() &&
         e1->timestamp() == e2->timestamp() && e1->timeout_s() == e2->timeout_s();
}

bool Equals(const EndTimerEvent* e1, const EndTimerEvent* e2) {
  return e1->timer_id() == e2->timer_id() && e1->timestamp() == e2->timestamp() &&
         e1->timeout_s() == e2->timeout_s();
}

bool Equals(const IntHistogramEvent* e1, const IntHistogramEvent* e2) {
  return e1->metric_id() == e2->metric_id() && e1->event_code() == e2->event_code() &&
         e1->component() == e2->component() && fidl::Equals(e1->histogram(), e2->histogram());
}

bool Equals(const CustomEvent* e1, const CustomEvent* e2) {
  return e1->metric_id() == e2->metric_id() && fidl::Equals(e1->event_values(), e2->event_values());
}

enum EventType {
  EVENT_OCCURRED,
  EVENT_COUNT,
  ELAPSED_TIME,
  FRAME_RATE,
  MEMORY_USAGE,
  STRING_USED,
  START_TIMER,
  END_TIMER,
  INT_HISTOGRAM,
  CUSTOM,
};

class FakeLoggerImpl : public fuchsia::cobalt::Logger {
 public:
  FakeLoggerImpl() {}

  void LogEvent(uint32_t metric_id, uint32_t event_code, LogEventCallback callback) override {
    RecordCall(EventType::EVENT_OCCURRED, std::make_unique<OccurrenceEvent>(metric_id, event_code));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogEventCount(uint32_t metric_id, uint32_t event_code, std::string component,
                     int64_t period_duration_micros, int64_t count,
                     LogEventCountCallback callback) override {
    RecordCall(EventType::EVENT_COUNT,
               std::make_unique<CountEvent>(metric_id, event_code, component,
                                            period_duration_micros, count));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogElapsedTime(uint32_t metric_id, uint32_t event_code, std::string component,
                      int64_t elapsed_micros, LogElapsedTimeCallback callback) override {
    RecordCall(EventType::ELAPSED_TIME, std::make_unique<ElapsedTimeEvent>(
                                            metric_id, event_code, component, elapsed_micros));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogFrameRate(uint32_t metric_id, uint32_t event_code, std::string component, float fps,
                    LogFrameRateCallback callback) override {
    RecordCall(EventType::FRAME_RATE,
               std::make_unique<FrameRateEvent>(metric_id, event_code, component, fps));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogMemoryUsage(uint32_t metric_id, uint32_t event_code, std::string component, int64_t bytes,
                      LogMemoryUsageCallback callback) override {
    RecordCall(EventType::MEMORY_USAGE,
               std::make_unique<MemoryUsageEvent>(metric_id, event_code, component, bytes));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogString(uint32_t metric_id, std::string s, LogStringCallback callback) override {
    RecordCall(EventType::STRING_USED, std::make_unique<StringUsedEvent>(metric_id, s));
    callback(fuchsia::cobalt::Status::OK);
  }

  void StartTimer(uint32_t metric_id, uint32_t event_code, std::string component,
                  std::string timer_id, uint64_t timestamp, uint32_t timeout_s,
                  StartTimerCallback callback) override {
    RecordCall(EventType::START_TIMER,
               std::make_unique<StartTimerEvent>(metric_id, event_code, component, timer_id,
                                                 timestamp, timeout_s));
    callback(fuchsia::cobalt::Status::OK);
  }

  void EndTimer(std::string timer_id, uint64_t timestamp, uint32_t timeout_s,
                EndTimerCallback callback) override {
    RecordCall(EventType::END_TIMER,
               std::make_unique<EndTimerEvent>(timer_id, timestamp, timeout_s));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogIntHistogram(uint32_t metric_id, uint32_t event_code, std::string component,
                       std::vector<fuchsia::cobalt::HistogramBucket> histogram,
                       LogIntHistogramCallback callback) override {
    RecordCall(EventType::INT_HISTOGRAM,
               std::make_unique<IntHistogramEvent>(metric_id, event_code, component,
                                                   std::move(histogram)));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogCustomEvent(uint32_t metric_id,
                      std::vector<fuchsia::cobalt::CustomEventValue> event_values,
                      LogCustomEventCallback callback) override {
    RecordCall(EventType::CUSTOM,
               std::make_unique<CustomEvent>(metric_id, std::move(event_values)));
    callback(fuchsia::cobalt::Status::OK);
  }

  void LogCobaltEvent(fuchsia::cobalt::CobaltEvent event,
                      LogCobaltEventCallback callback) override {
    if (event.payload.is_event_count()) {
      LogEventCount(event.metric_id, event.event_codes[0], event.component.value_or(""),
                    event.payload.event_count().period_duration_micros,
                    event.payload.event_count().count, std::move(callback));
    } else if (event.payload.is_int_histogram()) {
      LogIntHistogram(event.metric_id, event.event_codes[0], event.component.value_or(""),
                      std::move(event.payload.int_histogram()), std::move(callback));

    } else {
      callback(fuchsia::cobalt::Status::INVALID_ARGUMENTS);
    }
  }

  void LogCobaltEvents(std::vector<fuchsia::cobalt::CobaltEvent> events,
                       LogCobaltEventCallback callback) override {
    auto failures = 0;

    auto end = std::make_move_iterator(events.end());

    for (auto it = std::make_move_iterator(events.begin()); it != end; it++) {
      LogCobaltEvent(std::move(*it), [failures](fuchsia::cobalt::Status status) mutable {
        if (status != fuchsia::cobalt::Status::OK) {
          failures += 1;
        }
      });
    }

    if (failures == 0) {
      callback(fuchsia::cobalt::Status::OK);
    } else {
      callback(fuchsia::cobalt::Status::INTERNAL_ERROR);
    }
  }

  void ExpectCalledOnceWith(EventType type, const BaseEvent* expected) {
    ASSERT_EQ(1U, calls_[type].size());
    switch (type) {
      case EventType::EVENT_OCCURRED:
        EXPECT_TRUE(Equals(static_cast<const OccurrenceEvent*>(expected),
                           static_cast<OccurrenceEvent*>(calls_[type][0].get())));
        break;
      case EventType::EVENT_COUNT:
        EXPECT_TRUE(Equals(static_cast<const CountEvent*>(expected),
                           static_cast<CountEvent*>(calls_[type][0].get())));
        break;
      case EventType::ELAPSED_TIME:
        EXPECT_TRUE(Equals(static_cast<const ElapsedTimeEvent*>(expected),
                           static_cast<ElapsedTimeEvent*>(calls_[type][0].get())));
        break;
      case EventType::FRAME_RATE:
        EXPECT_TRUE(Equals(static_cast<const FrameRateEvent*>(expected),
                           static_cast<FrameRateEvent*>(calls_[type][0].get())));
        break;
      case EventType::MEMORY_USAGE:
        EXPECT_TRUE(Equals(static_cast<const MemoryUsageEvent*>(expected),
                           static_cast<MemoryUsageEvent*>(calls_[type][0].get())));
        break;
      case EventType::STRING_USED:
        EXPECT_TRUE(Equals(static_cast<const StringUsedEvent*>(expected),
                           static_cast<StringUsedEvent*>(calls_[type][0].get())));
        break;
      case EventType::START_TIMER:
        EXPECT_TRUE(Equals(static_cast<const StartTimerEvent*>(expected),
                           static_cast<StartTimerEvent*>(calls_[type][0].get())));
        break;
      case EventType::END_TIMER:
        EXPECT_TRUE(Equals(static_cast<const EndTimerEvent*>(expected),
                           static_cast<EndTimerEvent*>(calls_[type][0].get())));
        break;
      case EventType::INT_HISTOGRAM:
        EXPECT_TRUE(Equals(static_cast<const IntHistogramEvent*>(expected),
                           static_cast<IntHistogramEvent*>(calls_[type][0].get())));
        break;
      case EventType::CUSTOM:
        EXPECT_TRUE(Equals(static_cast<const CustomEvent*>(expected),
                           static_cast<CustomEvent*>(calls_[type][0].get())));
        break;
      default:
        ASSERT_TRUE(false);
    }
  }

 private:
  void RecordCall(EventType type, std::unique_ptr<BaseEvent> event) {
    calls_[type].push_back(std::move(event));
  }

  std::map<EventType, std::vector<std::unique_ptr<BaseEvent>>> calls_;
};

class FakeLoggerFactoryImpl : public fuchsia::cobalt::LoggerFactory {
 public:
  FakeLoggerFactoryImpl() {}

  void CreateLogger(fuchsia::cobalt::ProjectProfile profile,
                    fidl::InterfaceRequest<fuchsia::cobalt::Logger> request,
                    CreateLoggerCallback callback) override {
    received_project_name_ = "";
    received_release_stage_ = fuchsia::cobalt::ReleaseStage::GA;
    logger_.reset(new FakeLoggerImpl());
    logger_bindings_.AddBinding(logger_.get(), std::move(request));
    callback(fuchsia::cobalt::Status::OK);
  }

  void CreateLoggerSimple(fuchsia::cobalt::ProjectProfile profile,
                          fidl::InterfaceRequest<fuchsia::cobalt::LoggerSimple> request,
                          CreateLoggerSimpleCallback callback) override {
    callback(fuchsia::cobalt::Status::OK);
  }

  void CreateLoggerFromProjectName(std::string project_name, fuchsia::cobalt::ReleaseStage stage,
                                   fidl::InterfaceRequest<fuchsia::cobalt::Logger> request,
                                   CreateLoggerFromProjectNameCallback callback) override {
    received_project_name_ = project_name;
    received_release_stage_ = stage;
    logger_.reset(new FakeLoggerImpl());
    logger_bindings_.AddBinding(logger_.get(), std::move(request));
    callback(fuchsia::cobalt::Status::OK);
  }

  void CreateLoggerSimpleFromProjectName(
      std::string project_name, fuchsia::cobalt::ReleaseStage stage,
      fidl::InterfaceRequest<fuchsia::cobalt::LoggerSimple> request,
      CreateLoggerSimpleFromProjectNameCallback callback) override {}

  FakeLoggerImpl* logger() { return logger_.get(); }
  std::string received_project_name() { return received_project_name_; }
  ReleaseStage received_release_stage() { return received_release_stage_; }

 private:
  std::string received_project_name_;
  ReleaseStage received_release_stage_;
  std::unique_ptr<FakeLoggerImpl> logger_;
  fidl::BindingSet<fuchsia::cobalt::Logger> logger_bindings_;
};

class CobaltLoggerTest : public gtest::TestLoopFixture {
 public:
  CobaltLoggerTest() {}
  ~CobaltLoggerTest() override {}

  sys::ComponentContext* context() { return context_provider_.context(); }

  FakeLoggerFactoryImpl* logger_factory() { return factory_impl_.get(); }

  FakeLoggerImpl* logger() { return factory_impl_->logger(); }

  CobaltLogger* cobalt_logger() { return cobalt_logger_.get(); }

 private:
  virtual void SetUp() override {
    auto service_provider = context_provider_.service_directory_provider();
    factory_impl_.reset(new FakeLoggerFactoryImpl());
    service_provider->AddService<fuchsia::cobalt::LoggerFactory>(
        [this](fidl::InterfaceRequest<fuchsia::cobalt::LoggerFactory> request) {
          factory_bindings_.AddBinding(factory_impl_.get(), std::move(request));
        });
    service_provider->AddService<fuchsia::sys::Environment>(
        [this](fidl::InterfaceRequest<fuchsia::sys::Environment> request) {
          app_environment_request_ = std::move(request);
        });
    service_provider->AddService<fuchsia::sys::Launcher>(
        [this](fidl::InterfaceRequest<fuchsia::sys::Launcher> request) {
          launcher_request_ = std::move(request);
        });

    fsl::SizedVmo fake_cobalt_config;
    ASSERT_TRUE(fsl::VmoFromString(kFakeCobaltConfig, &fake_cobalt_config));
    fuchsia::cobalt::ProjectProfile profile;
    profile.config = std::move(fake_cobalt_config).ToTransport();
    cobalt_logger_ = NewCobaltLogger(async_get_default_dispatcher(), context(), std::move(profile));
    RunLoopUntilIdle();
  }

  std::unique_ptr<FakeLoggerFactoryImpl> factory_impl_;
  std::unique_ptr<CobaltLogger> cobalt_logger_;
  fidl::BindingSet<fuchsia::cobalt::LoggerFactory> factory_bindings_;
  fidl::InterfaceRequest<fuchsia::sys::Launcher> launcher_request_;
  fidl::InterfaceRequest<fuchsia::sys::Environment> app_environment_request_;
  sys::testing::ComponentContextProvider context_provider_;

  FXL_DISALLOW_COPY_AND_ASSIGN(CobaltLoggerTest);
};

TEST_F(CobaltLoggerTest, InitializeCobalt) {
  EXPECT_NE(cobalt_logger(), nullptr);
  EXPECT_EQ("", logger_factory()->received_project_name());
  EXPECT_EQ(fuchsia::cobalt::ReleaseStage::GA, logger_factory()->received_release_stage());
  NewCobaltLoggerFromProjectName(async_get_default_dispatcher(), context(), "MyProject",
                                 fuchsia::cobalt::ReleaseStage::DEBUG);
  RunLoopUntilIdle();
  EXPECT_EQ("MyProject", logger_factory()->received_project_name());
  EXPECT_EQ(fuchsia::cobalt::ReleaseStage::DEBUG, logger_factory()->received_release_stage());
}

TEST_F(CobaltLoggerTest, LogEvent) {
  OccurrenceEvent event(kFakeCobaltMetricId, 123);
  cobalt_logger()->LogEvent(event.metric_id(), event.event_code());
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::EVENT_OCCURRED, &event);
}

TEST_F(CobaltLoggerTest, LogEventCount) {
  CountEvent event(kFakeCobaltMetricId, 123, "some_component", 2, 321);
  cobalt_logger()->LogEventCount(event.metric_id(), event.event_code(), event.component(),
                                 zx::duration(event.period_duration_micros() * ZX_USEC(1)),
                                 event.count());
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::EVENT_COUNT, &event);
}

TEST_F(CobaltLoggerTest, LogCobaltEventEventCount) {
  CountEvent count_event(kFakeCobaltMetricId, 123, "some_component", 2, 322);
  fuchsia::cobalt::CobaltEvent event;
  event.metric_id = count_event.metric_id();
  event.event_codes.push_back(count_event.event_code());
  event.component = count_event.component();
  fuchsia::cobalt::CountEvent fuchsia_count_event;
  fuchsia_count_event.period_duration_micros = count_event.period_duration_micros();
  fuchsia_count_event.count = count_event.count();
  event.payload.set_event_count(fuchsia_count_event);
  cobalt_logger()->LogCobaltEvent(std::move(event));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::EVENT_COUNT, &count_event);
}

TEST_F(CobaltLoggerTest, LogCobaltEventsEventCount) {
  CountEvent count_event(kFakeCobaltMetricId, 123, "some_component", 2, 322);
  fuchsia::cobalt::CobaltEvent event;
  event.metric_id = count_event.metric_id();
  event.event_codes.push_back(count_event.event_code());
  event.component = count_event.component();
  fuchsia::cobalt::CountEvent fuchsia_count_event;
  fuchsia_count_event.period_duration_micros = count_event.period_duration_micros();
  fuchsia_count_event.count = count_event.count();
  event.payload.set_event_count(fuchsia_count_event);

  std::vector<fuchsia::cobalt::CobaltEvent> events;
  events.push_back(std::move(event));

  cobalt_logger()->LogCobaltEvents(std::move(events));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::EVENT_COUNT, &count_event);
}

TEST_F(CobaltLoggerTest, LogElapsedTime) {
  ElapsedTimeEvent event(kFakeCobaltMetricId, 123, "some_component", 321);
  cobalt_logger()->LogElapsedTime(event.metric_id(), event.event_code(), event.component(),
                                  zx::duration(event.elapsed_micros() * ZX_USEC(1)));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::ELAPSED_TIME, &event);
}

TEST_F(CobaltLoggerTest, LogFrameRate) {
  FrameRateEvent event(kFakeCobaltMetricId, 123, "some_component", 321.5f);
  cobalt_logger()->LogFrameRate(event.metric_id(), event.event_code(), event.component(),
                                event.fps());
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::FRAME_RATE, &event);
}

TEST_F(CobaltLoggerTest, LogMemoryUsage) {
  MemoryUsageEvent event(kFakeCobaltMetricId, 123, "some_component", 534582);
  cobalt_logger()->LogMemoryUsage(event.metric_id(), event.event_code(), event.component(),
                                  event.bytes());
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::MEMORY_USAGE, &event);
}

TEST_F(CobaltLoggerTest, LogString) {
  StringUsedEvent event(kFakeCobaltMetricId, "some_string");
  cobalt_logger()->LogString(event.metric_id(), event.s());
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::STRING_USED, &event);
}

TEST_F(CobaltLoggerTest, StartTimer) {
  zx::time timestamp = zx::clock::get_monotonic();
  StartTimerEvent event(kFakeCobaltMetricId, 123, "some_component", "timer_1",
                        timestamp.get() / ZX_USEC(1), 3);
  cobalt_logger()->StartTimer(event.metric_id(), event.event_code(), event.component(),
                              event.timer_id(), timestamp,
                              zx::duration(event.timeout_s() * ZX_SEC(1)));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::START_TIMER, &event);
}

TEST_F(CobaltLoggerTest, EndTimer) {
  zx::time timestamp = zx::clock::get_monotonic();
  EndTimerEvent event("timer_1", timestamp.get() / ZX_USEC(1), 3);
  cobalt_logger()->EndTimer(event.timer_id(), timestamp,
                            zx::duration(event.timeout_s() * ZX_SEC(1)));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::END_TIMER, &event);
}

TEST_F(CobaltLoggerTest, LogIntHistogram) {
  std::vector<fuchsia::cobalt::HistogramBucket> histogram(1);
  histogram.at(0).index = 1;
  histogram.at(0).count = 234;

  std::vector<fuchsia::cobalt::HistogramBucket> histogram_clone;
  ASSERT_EQ(ZX_OK, fidl::Clone(histogram, &histogram_clone));

  IntHistogramEvent event(kFakeCobaltMetricId, 123, "some_component", std::move(histogram_clone));
  cobalt_logger()->LogIntHistogram(event.metric_id(), event.event_code(), event.component(),
                                   std::move(histogram));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::INT_HISTOGRAM, &event);
}

TEST_F(CobaltLoggerTest, LogCobaltEventIntHistogram) {
  std::vector<fuchsia::cobalt::HistogramBucket> histogram(1);
  histogram.at(0).index = 1;
  histogram.at(0).count = 234;

  std::vector<fuchsia::cobalt::HistogramBucket> histogram_clone;
  ASSERT_EQ(ZX_OK, fidl::Clone(histogram, &histogram_clone));

  IntHistogramEvent histogram_event(kFakeCobaltMetricId, 123, "some_component",
                                    std::move(histogram_clone));

  fuchsia::cobalt::CobaltEvent event;
  event.metric_id = histogram_event.metric_id();
  event.event_codes.push_back(histogram_event.event_code());
  event.component = histogram_event.component();
  event.payload.set_int_histogram(std::move(histogram));

  cobalt_logger()->LogCobaltEvent(std::move(event));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::INT_HISTOGRAM, &histogram_event);
}

TEST_F(CobaltLoggerTest, LogCobaltEventsIntHistogram) {
  std::vector<fuchsia::cobalt::HistogramBucket> histogram(1);
  histogram.at(0).index = 1;
  histogram.at(0).count = 234;

  std::vector<fuchsia::cobalt::HistogramBucket> histogram_clone;
  ASSERT_EQ(ZX_OK, fidl::Clone(histogram, &histogram_clone));

  IntHistogramEvent histogram_event(kFakeCobaltMetricId, 123, "some_component",
                                    std::move(histogram_clone));

  fuchsia::cobalt::CobaltEvent event;
  event.metric_id = histogram_event.metric_id();
  event.event_codes.push_back(histogram_event.event_code());
  event.component = histogram_event.component();
  event.payload.set_int_histogram(std::move(histogram));

  std::vector<fuchsia::cobalt::CobaltEvent> events;
  events.push_back(std::move(event));

  cobalt_logger()->LogCobaltEvents(std::move(events));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::INT_HISTOGRAM, &histogram_event);
}

TEST_F(CobaltLoggerTest, LogCustomEvent) {
  std::vector<fuchsia::cobalt::CustomEventValue> event_values(1);
  event_values.at(0).dimension_name = "some_dimension";
  event_values.at(0).value.set_int_value(234);

  std::vector<fuchsia::cobalt::CustomEventValue> event_values_clone;
  ASSERT_EQ(ZX_OK, fidl::Clone(event_values, &event_values_clone));

  CustomEvent event(kFakeCobaltMetricId, std::move(event_values_clone));
  cobalt_logger()->LogCustomEvent(event.metric_id(), std::move(event_values));
  RunLoopUntilIdle();
  logger()->ExpectCalledOnceWith(EventType::CUSTOM, &event);
}

}  // namespace
}  // namespace cobalt
