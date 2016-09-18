#pragma once

#include "message_impl.h"

#include "envoy/event/dispatcher.h"
#include "envoy/http/async_client.h"
#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"
#include "envoy/http/message.h"
#include "envoy/router/shadow_writer.h"

#include "common/common/linked_object.h"
#include "common/http/access_log/request_info_impl.h"
#include "common/router/router.h"

namespace Http {

class AsyncRequestImpl;

class AsyncClientImpl final : public AsyncClient {
public:
  AsyncClientImpl(const Upstream::Cluster& cluster, Stats::Store& stats_store,
                  Event::Dispatcher& dispatcher, const std::string& local_zone_name,
                  Upstream::ClusterManager& cm, Runtime::Loader& runtime,
                  Runtime::RandomGenerator& random, Router::ShadowWriterPtr&& shadow_writer);
  ~AsyncClientImpl();

  // Http::AsyncClient
  Request* send(MessagePtr&& request, Callbacks& callbacks,
                const Optional<std::chrono::milliseconds>& timeout) override;

private:
  const Upstream::Cluster& cluster_;
  Router::FilterConfig config_;
  Event::Dispatcher& dispatcher_;
  std::list<std::unique_ptr<AsyncRequestImpl>> active_requests_;

  friend class AsyncRequestImpl;
};

/**
 * Implementation of AsyncRequest. This implementation is capable of sending HTTP requests to a
 * ConnectionPool asynchronously.
 */
class AsyncRequestImpl final : public AsyncClient::Request,
                               StreamDecoderFilterCallbacks,
                               Router::StableRouteTable,
                               Logger::Loggable<Logger::Id::http>,
                               LinkedObject<AsyncRequestImpl> {
public:
  AsyncRequestImpl(MessagePtr&& request, AsyncClientImpl& parent, AsyncClient::Callbacks& callbacks,
                   const Optional<std::chrono::milliseconds>& timeout);
  ~AsyncRequestImpl();

  // Http::AsyncHttpRequest
  void cancel() override;

private:
  void cleanup();
  void onComplete();

  // Http::StreamDecoderFilterCallbacks
  void addResetStreamCallback(std::function<void()> callback) override {
    reset_callback_ = callback;
  }
  uint64_t connectionId() override { return 0; } // FIXFIX
  Event::Dispatcher& dispatcher() override { NOT_IMPLEMENTED; }
  void resetStream() override { NOT_IMPLEMENTED; }
  const Router::StableRouteTable& routeTable() { return *this; }
  uint64_t streamId() override { return 0; } // FIXFIX
  AccessLog::RequestInfo& requestInfo() override { return request_info_; }
  void continueDecoding() override { NOT_IMPLEMENTED; }
  const Buffer::Instance* decodingBuffer() override { NOT_IMPLEMENTED; }
  void encodeHeaders(HeaderMapPtr&& headers, bool end_stream) override;
  void encodeData(Buffer::Instance& data, bool end_stream) override;
  void encodeTrailers(HeaderMapPtr&& trailers) override;

  // Router::StableRouteTable
  const Router::RedirectEntry* redirectRequest(const Http::HeaderMap&) const override {
    return nullptr;
  }
  const Router::RouteEntry* routeForRequest(const Http::HeaderMap&) const override {
    return nullptr;
  }

  MessagePtr request_;
  AsyncClientImpl& parent_;
  AsyncClient::Callbacks& callbacks_;
  std::unique_ptr<MessageImpl> response_;
  Router::ProdFilter router_;
  std::function<void()> reset_callback_;
  AccessLog::RequestInfoImpl request_info_;

  friend class AsyncClientImpl;
};

} // Http
