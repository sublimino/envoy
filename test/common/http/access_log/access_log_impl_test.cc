#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/upstream.h"

#include "common/common/thread.h"
#include "common/http/access_log/access_log_impl.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/json/json_loader.h"
#include "common/runtime/runtime_impl.h"
#include "common/runtime/uuid_util.h"
#include "common/stats/stats_impl.h"
#include "common/upstream/upstream_impl.h"

#include "test/mocks/api/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/upstream/mocks.h"

using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::SaveArg;

namespace Http {
namespace AccessLog {

class TestRequestInfo : public RequestInfo {
public:
  TestRequestInfo() {
    tm fake_time;
    memset(&fake_time, 0, sizeof(fake_time));
    fake_time.tm_mday = 1;
    start_time_ = std::chrono::system_clock::from_time_t(timegm(&fake_time));
  }

  SystemTime startTime() const override { return start_time_; }
  uint64_t bytesReceived() const override { return 1; }
  const std::string& protocol() const override { return protocol_; }
  const Optional<uint32_t>& responseCode() const override { return response_code_; }
  uint64_t bytesSent() const override { return 2; }
  std::chrono::milliseconds duration() const override {
    return std::chrono::milliseconds(duration_);
  }
  Http::AccessLog::FailureReason failureReason() const override { return failure_reason_; }
  void onFailedResponse(FailureReason failure_reason) override { failure_reason_ = failure_reason; }
  void onUpstreamHostSelected(Upstream::HostDescriptionPtr host) override { upstream_host_ = host; }
  Upstream::HostDescriptionPtr upstreamHost() const override { return upstream_host_; }
  bool healthCheck() const override { return hc_request_; }
  void healthCheck(bool is_hc) { hc_request_ = is_hc; }

  SystemTime start_time_;
  std::string protocol_{"HTTP/1.1"};
  Optional<uint32_t> response_code_;
  FailureReason failure_reason_{FailureReason::None};
  uint64_t duration_{3};
  Upstream::HostDescriptionPtr upstream_host_{};
  bool hc_request_{};
};

class AccessLogImplTest : public testing::Test {
public:
  AccessLogImplTest() : file_(new Filesystem::MockFile()) {
    EXPECT_CALL(api_, createFile_(_, _, _, _)).WillOnce(Return(file_));
    ON_CALL(*file_, write(_)).WillByDefault(SaveArg<0>(&output_));
  }

  NiceMock<Api::MockApi> api_;
  HeaderMapImpl request_headers_{{":method", "GET"}, {":path", "/"}};
  HeaderMapImpl response_headers_;
  TestRequestInfo request_info_;
  Filesystem::MockFile* file_;
  std::string output_;
  Event::MockDispatcher dispatcher_;
  Thread::MutexBasicLockable lock_;
};

TEST_F(AccessLogImplTest, LogMoreData) {
  std::string json = R"EOF(
  {
    "path": "/dev/null"
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  EXPECT_CALL(*file_, write(_));
  request_info_.failure_reason_ = FailureReason::UpstreamConnectionFailure;
  request_headers_.addViaCopy(Http::Headers::get().UserAgent, "user-agent-set");
  request_headers_.addViaCopy(Http::Headers::get().RequestId, "id");
  request_headers_.addViaCopy(Http::Headers::get().Host, "host");
  request_headers_.addViaCopy(Http::Headers::get().ForwardedFor, "x.x.x.x");

  log->log(&request_headers_, &response_headers_, request_info_);
  EXPECT_EQ("[1900-01-01T00:00:00.000Z] \"GET / HTTP/1.1\" 0 UF 1 2 3 - \"x.x.x.x\" "
            "\"user-agent-set\" \"id\" \"host\" \"-\"\n",
            output_);
}

TEST_F(AccessLogImplTest, EnvoyUpstreamServiceTime) {
  std::string json = R"EOF(
  {
    "path": "/dev/null"
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  EXPECT_CALL(*file_, write(_));
  response_headers_.addViaCopy(Http::Headers::get().EnvoyUpstreamServiceTime, "999");

  log->log(&request_headers_, &response_headers_, request_info_);
  EXPECT_EQ(
      "[1900-01-01T00:00:00.000Z] \"GET / HTTP/1.1\" 0 - 1 2 3 999 \"-\" \"-\" \"-\" \"-\" \"-\"\n",
      output_);
}

TEST_F(AccessLogImplTest, NoFilter) {
  std::string json = R"EOF(
    {
      "path": "/dev/null"
    }
    )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  EXPECT_CALL(*file_, write(_));
  log->log(&request_headers_, &response_headers_, request_info_);
  EXPECT_EQ(
      "[1900-01-01T00:00:00.000Z] \"GET / HTTP/1.1\" 0 - 1 2 3 - \"-\" \"-\" \"-\" \"-\" \"-\"\n",
      output_);
}

TEST_F(AccessLogImplTest, UpstreamHost) {
  Upstream::MockCluster cluster;
  request_info_.upstream_host_ =
      std::make_shared<Upstream::HostDescriptionImpl>(cluster, "tcp://10.0.0.5:1234", false, "");

  std::string json = R"EOF(
      {
        "path": "/dev/null"
      }
      )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  EXPECT_CALL(*file_, write(_));
  log->log(&request_headers_, &response_headers_, request_info_);
  EXPECT_EQ("[1900-01-01T00:00:00.000Z] \"GET / HTTP/1.1\" 0 - 1 2 3 - \"-\" \"-\" \"-\" \"-\" "
            "\"tcp://10.0.0.5:1234\"\n",
            output_);
}

TEST_F(AccessLogImplTest, WithFilterMiss) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type":"logical_or", "filters": [
        {"type": "status_code", "op": ">=", "value": 500},
        {"type": "duration", "op": ">=", "value": 1000000}
      ]
    }
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  EXPECT_CALL(*file_, write(_)).Times(0);
  log->log(&request_headers_, &response_headers_, request_info_);

  request_info_.response_code_.value(200);
  log->log(&request_headers_, &response_headers_, request_info_);
}

TEST_F(AccessLogImplTest, WithFilterHit) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "logical_or", "filters": [
        {"type": "status_code", "op": ">=", "value": 500},
        {"type": "status_code", "op": "=", "value": 0},
        {"type": "duration", "op": ">=", "value": 1000000}
      ]
    }
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  EXPECT_CALL(*file_, write(_)).Times(3);
  log->log(&request_headers_, &response_headers_, request_info_);

  request_info_.response_code_.value(500);
  log->log(&request_headers_, &response_headers_, request_info_);

  request_info_.response_code_.value(200);
  request_info_.duration_ = 1000000;
  log->log(&request_headers_, &response_headers_, request_info_);
}

TEST_F(AccessLogImplTest, RuntimeFilter) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "runtime", "key": "access_log.test_key"}
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  // Value is taken from random generator.
  EXPECT_CALL(runtime.snapshot_, featureEnabled("access_log.test_key", 0)).WillOnce(Return(true));
  EXPECT_CALL(*file_, write(_));
  log->log(&request_headers_, &response_headers_, request_info_);

  EXPECT_CALL(runtime.snapshot_, featureEnabled("access_log.test_key", 0)).WillOnce(Return(false));
  EXPECT_CALL(*file_, write(_)).Times(0);
  log->log(&request_headers_, &response_headers_, request_info_);

  // Value is taken from x-request-id.
  request_headers_.replaceViaMoveValue("x-request-id", "000000ff-0000-0000-0000-000000000000");
  EXPECT_CALL(runtime.snapshot_, getInteger("access_log.test_key", 0)).WillOnce(Return(56));
  EXPECT_CALL(*file_, write(_));
  log->log(&request_headers_, &response_headers_, request_info_);

  EXPECT_CALL(runtime.snapshot_, getInteger("access_log.test_key", 0)).WillOnce(Return(55));
  EXPECT_CALL(*file_, write(_)).Times(0);
  log->log(&request_headers_, &response_headers_, request_info_);
}

TEST_F(AccessLogImplTest, PathRewrite) {
  request_headers_ = {{":method", "GET"}, {":path", "/foo"}, {"x-envoy-original-path", "/bar"}};

  std::string json = R"EOF(
      {
        "path": "/dev/null"
      }
      )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  EXPECT_CALL(*file_, write(_));
  log->log(&request_headers_, &response_headers_, request_info_);
  EXPECT_EQ("[1900-01-01T00:00:00.000Z] \"GET /bar HTTP/1.1\" 0 - 1 2 3 - \"-\" \"-\" \"-\" \"-\" "
            "\"-\"\n",
            output_);
}

TEST_F(AccessLogImplTest, healthCheckTrue) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "not_healthcheck"}
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  HeaderMapImpl header_map{};
  request_info_.hc_request_ = true;
  EXPECT_CALL(*file_, write(_)).Times(0);

  log->log(&header_map, &response_headers_, request_info_);
}

TEST_F(AccessLogImplTest, healthCheckFalse) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "not_healthcheck"}
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  HeaderMapImpl header_map{};
  EXPECT_CALL(*file_, write(_));

  log->log(&request_headers_, &response_headers_, request_info_);
}

TEST_F(AccessLogImplTest, requestTracing) {
  Runtime::RandomGeneratorImpl random;
  std::string not_traceable_guid = random.uuid();

  std::string force_tracing_guid = random.uuid();
  UuidUtils::setTraceableUuid(force_tracing_guid, UuidTraceStatus::Forced);

  std::string sample_tracing_guid = random.uuid();
  UuidUtils::setTraceableUuid(sample_tracing_guid, UuidTraceStatus::Sampled);

  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "traceable_request"}
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);

  {
    HeaderMapImpl forced_header{{"x-request-id", force_tracing_guid}};
    EXPECT_CALL(*file_, write(_));
    log->log(&forced_header, &response_headers_, request_info_);
  }

  {
    HeaderMapImpl not_traceable{{"x-request-id", not_traceable_guid}};
    EXPECT_CALL(*file_, write(_)).Times(0);
    log->log(&not_traceable, &response_headers_, request_info_);
  }

  {
    HeaderMapImpl sampled_header{{"x-request-id", sample_tracing_guid}};
    EXPECT_CALL(*file_, write(_)).Times(0);
    log->log(&sampled_header, &response_headers_, request_info_);
  }
}

TEST(AccessLogImplTestCtor, OperatorIsNotSupported) {
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  Thread::MutexBasicLockable lock;
  NiceMock<Api::MockApi> api;
  Event::MockDispatcher dispatcher;

  std::vector<std::string> unsupported_operators = {"<", "<=", ">"};

  for (const auto& oper : unsupported_operators) {
    std::string json =
        "{ \"path\": \"/dev/null\", \"filter\": {\"type\": \"status_code\", \"op\": \"" + oper +
        "\", \"value\" : 500}}";

    Json::StringLoader loader(json);
    EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
                 EnvoyException);
  }
}

TEST(AccessLogImplTestCtor, FilterTypeNotSupported) {
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  Thread::MutexBasicLockable lock;
  NiceMock<Api::MockApi> api;
  Event::MockDispatcher dispatcher;

  std::string json = R"EOF(
    {
      "path": "/dev/null",
      "filter": {"type": "unknown"}
    }
  )EOF";
  Json::StringLoader loader(json);

  EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
               EnvoyException);
}

TEST(AccessLogImplTestCtor, FiltersMissingInOrAndFilter) {
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  Thread::MutexBasicLockable lock;
  NiceMock<Api::MockApi> api;
  Event::MockDispatcher dispatcher;

  {
    std::string json = R"EOF(
      {
        "path": "/dev/null",
        "filter": {"type": "logical_or"}
      }
    )EOF";
    Json::StringLoader loader(json);

    EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
                 EnvoyException);
  }

  {
    std::string json = R"EOF(
      {
        "path": "/dev/null",
        "filter": {"type": "logical_and"}
      }
    )EOF";
    Json::StringLoader loader(json);

    EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
                 EnvoyException);
  }
}

TEST(AccessLogImplTestCtor, lessThanTwoInFilterList) {
  Stats::IsolatedStoreImpl store;
  NiceMock<Runtime::MockLoader> runtime;
  Thread::MutexBasicLockable lock;
  NiceMock<Api::MockApi> api;
  Event::MockDispatcher dispatcher;

  {
    std::string json = R"EOF(
    {
      "path": "/dev/null",
      "filter": {"type": "logical_or", "filters" : []}
    }
    )EOF";
    Json::StringLoader loader(json);

    EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
                 EnvoyException);
  }

  {
    std::string json = R"EOF(
    {
      "path": "/dev/null",
      "filter": {"type": "logical_and", "filters" : []}
    }
    )EOF";
    Json::StringLoader loader(json);

    EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
                 EnvoyException);
  }

  {
    std::string json = R"EOF(
    {
      "path": "/dev/null",
      "filter": {"type": "logical_or", "filters" : [ {"type": "not_healthcheck"} ]}
    }
    )EOF";
    Json::StringLoader loader(json);

    EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
                 EnvoyException);
  }

  {
    std::string json = R"EOF(
    {
      "path": "/dev/null",
      "filter": {"type": "logical_and", "filters" : [ {"type": "not_healthcheck"} ]}
    }
    )EOF";
    Json::StringLoader loader(json);

    EXPECT_THROW(InstanceImpl::fromJson(loader, api, dispatcher, lock, store, runtime),
                 EnvoyException);
  }
}

TEST_F(AccessLogImplTest, andFilter) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "logical_and", "filters": [
        {"type": "status_code", "op": ">=", "value": 500},
        {"type": "not_healthcheck"}
      ]
    }
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);
  request_info_.response_code_.value(500);

  {
    EXPECT_CALL(*file_, write(_));
    HeaderMapImpl header_map{{"user-agent", "NOT/Envoy/HC"}};

    log->log(&header_map, &response_headers_, request_info_);
  }

  {
    EXPECT_CALL(*file_, write(_)).Times(0);
    HeaderMapImpl header_map{};
    request_info_.hc_request_ = true;
    log->log(&header_map, &response_headers_, request_info_);
  }
}

TEST_F(AccessLogImplTest, orFilter) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "logical_or", "filters": [
        {"type": "status_code", "op": ">=", "value": 500},
        {"type": "not_healthcheck"}
      ]
    }
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);
  request_info_.response_code_.value(500);

  {
    EXPECT_CALL(*file_, write(_));
    HeaderMapImpl header_map{{"user-agent", "NOT/Envoy/HC"}};

    log->log(&header_map, &response_headers_, request_info_);
  }

  {
    EXPECT_CALL(*file_, write(_));
    HeaderMapImpl header_map{{"user-agent", "Envoy/HC"}};
    log->log(&header_map, &response_headers_, request_info_);
  }
}

TEST_F(AccessLogImplTest, multipleOperators) {
  std::string json = R"EOF(
  {
    "path": "/dev/null",
    "filter": {"type": "logical_and", "filters": [
        {"type": "logical_or", "filters": [
            {"type": "duration", "op": ">=", "value": 10000},
            {"type": "status_code", "op": ">=", "value": 500}
          ]
        },
        {"type": "not_healthcheck"}
      ]
    }
  }
  )EOF";

  Json::StringLoader loader(json);
  Stats::IsolatedStoreImpl store;
  Runtime::MockLoader runtime;
  InstancePtr log = InstanceImpl::fromJson(loader, api_, dispatcher_, lock_, store, runtime);
  request_info_.response_code_.value(500);

  {
    EXPECT_CALL(*file_, write(_));
    HeaderMapImpl header_map{};

    log->log(&header_map, &response_headers_, request_info_);
  }

  {
    EXPECT_CALL(*file_, write(_)).Times(0);
    HeaderMapImpl header_map{};
    request_info_.hc_request_ = true;

    log->log(&header_map, &response_headers_, request_info_);
  }
}

TEST(AccessLogFilterTest, DurationWithRuntimeKey) {
  std::string filter_json = R"EOF(
    {
      "filter": {"type": "duration", "op": ">=", "value": 1000000, "runtime_key": "key"}
    }
    )EOF";

  Json::StringLoader loader(filter_json);
  NiceMock<Runtime::MockLoader> runtime;

  Json::Object filter_object = loader.getObject("filter");
  DurationFilter filter(filter_object, runtime);
  HeaderMapImpl request_headers{{":method", "GET"}, {":path", "/"}};
  TestRequestInfo request_info;

  request_info.duration_ = 100;

  EXPECT_CALL(runtime.snapshot_, getInteger("key", 1000000)).WillOnce(Return(1));
  EXPECT_TRUE(filter.evaluate(request_info, request_headers));

  EXPECT_CALL(runtime.snapshot_, getInteger("key", 1000000)).WillOnce(Return(1000));
  EXPECT_FALSE(filter.evaluate(request_info, request_headers));

  request_info.duration_ = 100000001;
  EXPECT_CALL(runtime.snapshot_, getInteger("key", 1000000)).WillOnce(Return(100000000));
  EXPECT_TRUE(filter.evaluate(request_info, request_headers));

  request_info.duration_ = 10;
  EXPECT_CALL(runtime.snapshot_, getInteger("key", 1000000)).WillOnce(Return(100000000));
  EXPECT_FALSE(filter.evaluate(request_info, request_headers));
}

TEST(AccessLogFilterTest, StatusCodeWithRuntimeKey) {
  std::string filter_json = R"EOF(
    {
      "filter": {"type": "status_code", "op": ">=", "value": 300, "runtime_key": "key"}
    }
    )EOF";

  Json::StringLoader loader(filter_json);
  NiceMock<Runtime::MockLoader> runtime;

  Json::Object filter_object = loader.getObject("filter");
  StatusCodeFilter filter(filter_object, runtime);

  HeaderMapImpl request_headers{{":method", "GET"}, {":path", "/"}};
  TestRequestInfo info;

  info.response_code_.value(400);
  EXPECT_CALL(runtime.snapshot_, getInteger("key", 300)).WillOnce(Return(350));
  EXPECT_TRUE(filter.evaluate(info, request_headers));

  EXPECT_CALL(runtime.snapshot_, getInteger("key", 300)).WillOnce(Return(500));
  EXPECT_FALSE(filter.evaluate(info, request_headers));
}

} // AccessLog
} // Http
