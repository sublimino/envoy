#include "async_client_impl.h"

#include "common/common/empty_string.h"
//#include "common/common/enum_to_int.h"
//#include "common/http/codes.h"
//#include "common/http/headers.h"

namespace Http {

/*const HeaderMapImpl AsyncRequestImpl::SERVICE_UNAVAILABLE_HEADER{
    {Headers::get().Status, std::to_string(enumToInt(Code::ServiceUnavailable))}};

const HeaderMapImpl AsyncRequestImpl::REQUEST_TIMEOUT_HEADER{
    {Headers::get().Status, std::to_string(enumToInt(Code::GatewayTimeout))}};*/

AsyncClientImpl::AsyncClientImpl(const Upstream::Cluster& cluster, Stats::Store& stats_store,
                                 Event::Dispatcher& dispatcher, const std::string& local_zone_name,
                                 Upstream::ClusterManager& cm, Runtime::Loader& runtime,
                                 Runtime::RandomGenerator& random,
                                 Router::ShadowWriterPtr&& shadow_writer)
    : cluster_(cluster), config_("fixfix", local_zone_name, stats_store, cm, runtime, random,
                                 std::move(shadow_writer)),
      dispatcher_(dispatcher) {}

AsyncClientImpl::~AsyncClientImpl() {
  while (!active_requests_.empty()) {
    active_requests_.front()->cancel();
  }
}

AsyncClient::Request* AsyncClientImpl::send(MessagePtr&& request, AsyncClient::Callbacks& callbacks,
                                            const Optional<std::chrono::milliseconds>& timeout) {
  // For now we use default priority for all requests. We could eventually expose priority out of
  // send if needed.
  /*ConnectionPool::Instance* conn_pool = factory_.connPool(Upstream::ResourcePriority::Default);
  if (!conn_pool) {
    callbacks.onFailure(AsyncClient::FailureReason::Reset);
    return nullptr;
  }*/

  std::unique_ptr<AsyncRequestImpl> new_request{
      new AsyncRequestImpl(std::move(request), *this, callbacks, timeout)};

  // The request may get immediately failed. If so, we will return nullptr.
  if (!new_request->response_) {
    new_request->moveIntoList(std::move(new_request), active_requests_);
    return active_requests_.front().get();
  } else {
    return nullptr;
  }
}

AsyncRequestImpl::AsyncRequestImpl(MessagePtr&& request, AsyncClientImpl& parent,
                                   AsyncClient::Callbacks& callbacks,
                                   const Optional<std::chrono::milliseconds>& /*timeout*/)
    : request_(std::move(request)), parent_(parent), callbacks_(callbacks), router_(parent.config_),
      request_info_(EMPTY_STRING) {

  router_.setDecoderFilterCallbacks(*this);

  router_.decodeHeaders(request_->headers(), !request_->body());
  if (!response_ && request_->body()) {
    router_.decodeData(*request_->body(), true);
  }

  // FIXFIX trailers

  /*stream_encoder_.reset(new PooledStreamEncoder(conn_pool, *this, *this, 0, 0, *this));
  stream_encoder_->encodeHeaders(request_->headers(), !request_->body());

  // We might have been immediately failed.
  if (stream_encoder_ && request_->body()) {
    stream_encoder_->encodeData(*request_->body(), true);
  }*/

  /*if (stream_encoder_ && timeout.valid()) {
    request_timeout_ = dispatcher.createTimer([this]() -> void { onRequestTimeout(); });
    request_timeout_->enableTimer(timeout.value());
  }*/
}

AsyncRequestImpl::~AsyncRequestImpl() { /*ASSERT(!stream_encoder_);*/
}

void AsyncRequestImpl::encodeHeaders(HeaderMapPtr&& headers, bool end_stream) {
  response_.reset(new ResponseMessageImpl(std::move(headers)));
#ifndef NDEBUG
  log_debug("async http request response headers (end_stream={}):", end_stream);
  response_->headers().iterate([](const LowerCaseString& key, const std::string& value)
                                   -> void { log_debug("  '{}':'{}'", key.get(), value); });
#endif

  if (end_stream) {
    onComplete();
  }
}

void AsyncRequestImpl::encodeData(Buffer::Instance& data, bool end_stream) {
  log_trace("async http request response data (length={} end_stream={})", data.length(),
            end_stream);
  if (!response_->body()) {
    response_->body(Buffer::InstancePtr{new Buffer::OwnedImpl(data)});
  } else {
    response_->body()->add(data);
  }

  if (end_stream) {
    onComplete();
  }
}

void AsyncRequestImpl::encodeTrailers(HeaderMapPtr&& trailers) {
  response_->trailers(std::move(trailers));
#ifndef NDEBUG
  log_debug("async http request response trailers:");
  response_->trailers()->iterate([](const LowerCaseString& key, const std::string& value)
                                     -> void { log_debug("  '{}':'{}'", key.get(), value); });
#endif

  onComplete();
}

/*const std::string& AsyncRequestImpl::upstreamZone() {
  return upstream_host_ ? upstream_host_->zone() : EMPTY_STRING;
}*/

/*bool AsyncRequestImpl::isUpstreamCanary() {
  return (response_ ? (response_->headers().get(Headers::get().EnvoyUpstreamCanary) == "true")
                    : false) ||
         (upstream_host_ ? upstream_host_->canary() : false);
}*/

void AsyncRequestImpl::cancel() {
  reset_callback_();
  cleanup();
}

/*void AsyncRequestImpl::decodeHeaders(HeaderMapPtr&& headers, bool end_stream) {

}*/

/*void AsyncRequestImpl::decodeData(const Buffer::Instance& data, bool end_stream) {

}*/

/*void AsyncRequestImpl::decodeTrailers(HeaderMapPtr&& trailers) {

}*/

void AsyncRequestImpl::onComplete() { callbacks_.onSuccess(std::move(response_)); }

/*void AsyncRequestImpl::onResetStream(StreamResetReason) {
  CodeUtility::ResponseStatInfo info{parent_.stats_store_, parent_.stat_prefix_,
                                     SERVICE_UNAVAILABLE_HEADER, true, EMPTY_STRING, EMPTY_STRING,
                                     parent_.local_zone_name_, upstreamZone(), isUpstreamCanary()};
  CodeUtility::chargeResponseStat(info);
  callbacks_.onFailure(AsyncClient::FailureReason::Reset);
  cleanup();
}*/

/*void AsyncRequestImpl::onRequestTimeout() {
  CodeUtility::ResponseStatInfo info{parent_.stats_store_, parent_.stat_prefix_,
                                     REQUEST_TIMEOUT_HEADER, true, EMPTY_STRING, EMPTY_STRING,
                                     parent_.local_zone_name_, upstreamZone(), isUpstreamCanary()};
  CodeUtility::chargeResponseStat(info);
  parent_.cluster_.stats().upstream_rq_timeout_.inc();
  stream_encoder_->resetStream();
  callbacks_.onFailure(AsyncClient::FailureReason::RequestTimeout);
  cleanup();
}*/

void AsyncRequestImpl::cleanup() {
  // This will destroy us, but only do so if we are actually in a list. This does not happen in
  // the immediate failure case.
  if (inserted()) {
    removeFromList(parent_.active_requests_);
  }
}

} // Http
