// SPDX-FileCopyrightText: 2024 Contributors to the Eclipse Foundation
//
// See the NOTICE file(s) distributed with this work for additional
// information regarding copyright ownership.
//
// This program and the accompanying materials are made available under the
// terms of the Apache License Version 2.0 which is available at
// https://www.apache.org/licenses/LICENSE-2.0
//
// SPDX-License-Identifier: Apache-2.0

#include "up-cpp/communication/RpcClient.h"

#include <google/protobuf/util/message_differencer.h>

#include <chrono>
#include <queue>

namespace {
namespace detail {

using namespace uprotocol;

struct PendingRequest {
	std::chrono::steady_clock::time_point when_expire;
	transport::UTransport::ListenHandle response_listener;
	std::function<void(communication::RpcClient::Status)> expire;
	size_t instance_id;

	auto operator>(const PendingRequest& other) const;
};

struct ScrubablePendingQueue
    : public std::priority_queue<PendingRequest, std::vector<PendingRequest>,
                                 std::greater<PendingRequest>> {
	~ScrubablePendingQueue();
	auto scrub(size_t instance_id);
	PendingRequest& top();
};

struct ExpireWorker {
	ExpireWorker();
	~ExpireWorker();
	void enqueue(PendingRequest&& request);
	void scrub(size_t instance_id);
	void doWork();

private:
	std::mutex pending_mtx_;
	ScrubablePendingQueue pending_;
	std::thread worker_;
	std::atomic<bool> stop_{false};
	std::condition_variable wake_worker_;
};

}  // namespace detail
}  // namespace

namespace uprotocol::communication {

////////////////////////////////////////////////////////////////////////////////
struct RpcClient::ExpireService {
	ExpireService() : instance_id_(next_instance_id++) {}

	~ExpireService() { worker.scrub(instance_id_); }

	void enqueue(std::chrono::steady_clock::time_point when_expire,
	             transport::UTransport::ListenHandle&& response_listener,
	             std::function<void(RpcClient::Status)> expire) {
		detail::PendingRequest pending{
		    .when_expire = when_expire,
		    .response_listener = std::move(response_listener),
		    .expire = std::move(expire),
		    .instance_id = instance_id_};

		worker.enqueue(std::move(pending));
	}

private:
	static inline std::atomic<size_t> next_instance_id{0};
	static inline detail::ExpireWorker worker;
	size_t instance_id_;
};

////////////////////////////////////////////////////////////////////////////////
RpcClient::RpcClient(std::shared_ptr<transport::UTransport> transport,
                     v1::UUri&& method, v1::UPriority priority,
                     std::chrono::milliseconds ttl,
                     std::optional<v1::UPayloadFormat> payload_format,
                     std::optional<uint32_t> permission_level,
                     std::optional<std::string> token)
    : transport_(transport),
      ttl_(ttl),
      builder_(datamodel::builder::UMessageBuilder::request(
          std::move(method), v1::UUri(transport_->getDefaultSource()), priority,
          ttl_)),
      expire_service_(std::make_unique<ExpireService>()) {
	if (payload_format) {
		builder_.withPayloadFormat(*payload_format);
	}

	if (permission_level) {
		builder_.withPermissionLevel(*permission_level);
	}

	if (token) {
		builder_.withToken(*token);
	}
}

void RpcClient::invokeMethod(v1::UMessage&& request, Callback&& callback) {
	auto when_expire = std::chrono::steady_clock::now() + ttl_;
	auto reqid = request.attributes().id();
	// Used to ensure that, no matter what, the callback is only called once
	auto callback_once = std::make_shared<std::once_flag>();

	///////////////////////////////////////////////////////////////////////////
	// Wraps the callback to handle receive filtering and commstatus checking
	auto wrapper = [callback, reqid = std::move(reqid),
	                callback_once](const v1::UMessage& m) {
		using MsgDiff = google::protobuf::util::MessageDifferencer;
		if (MsgDiff::Equals(m.attributes().reqid(), reqid)) {
			if (m.attributes().commstatus() == v1::UCode::OK) {
				std::call_once(*callback_once,
				               [&callback, &m]() { callback(m); });
			} else {
				std::call_once(*callback_once, [&callback, &m]() {
					callback(
					    utils::Unexpected<Status>(m.attributes().commstatus()));
				});
			}
		}
	};
	///////////////////////////////////////////////////////////////////////////

	///////////////////////////////////////////////////////////////////////////
	// Called locally when the request has expired. Will be handed off to the
	// expiration monitoring service.
	auto expire = [callback = std::move(callback), callback_once](
	                  std::variant<v1::UStatus, Commstatus>&& reason) {
		std::call_once(*callback_once, [callback = std::move(callback),
		                                reason = std::move(reason)]() {
			callback(utils::Unexpected<Status>(std::move(reason)));
		});
	};
	///////////////////////////////////////////////////////////////////////////

	auto maybe_handle = transport_->registerListener(
	    request.attributes().source(), std::move(wrapper),
	    request.attributes().sink());

	if (!maybe_handle) {
		expire(maybe_handle.error());
	} else {
		auto send_result = transport_->send(request);
		if (send_result.code() != v1::UCode::OK) {
			expire(send_result);
		}

		expire_service_->enqueue(when_expire, std::move(maybe_handle).value(),
		                         std::move(expire));
	}
}

void RpcClient::invokeMethod(datamodel::builder::Payload&& payload,
                             Callback&& callback) {
	invokeMethod(builder_.build(std::move(payload)), std::move(callback));
}

void RpcClient::invokeMethod(Callback&& callback) {
	invokeMethod(builder_.build(), std::move(callback));
}

std::future<RpcClient::MessageOrStatus> RpcClient::invokeMethod(
    datamodel::builder::Payload&& payload) {
	// Note: functors need to be copy constructable. We work around this by
	// wrapping the promise in a shared_ptr. Unique access to it will be
	// assured by the implementation at the core of invokeMethod - it only
	// allows exactly one call to the callback via std::call_once.
	auto promise = std::make_shared<std::promise<MessageOrStatus>>();
	auto future = promise->get_future();
	invokeMethod(std::move(payload),
	             [promise](MessageOrStatus maybe_message) mutable {
		             promise->set_value(maybe_message);
	             });

	return future;
}

std::future<RpcClient::MessageOrStatus> RpcClient::invokeMethod() {
	// Note: functors need to be copy constructable. We work around this by
	// wrapping the promise in a shared_ptr. Unique access to it will be
	// assured by the implementation at the core of invokeMethod - it only
	// allows exactly one call to the callback via std::call_once.
	auto promise = std::make_shared<std::promise<MessageOrStatus>>();
	auto future = promise->get_future();
	invokeMethod([promise](MessageOrStatus maybe_message) mutable {
		promise->set_value(maybe_message);
	});

	return future;
}

RpcClient::RpcClient(RpcClient&&) = default;
RpcClient::~RpcClient() = default;

}  // namespace uprotocol::communication

///////////////////////////////////////////////////////////////////////////////
namespace {
namespace detail {

using namespace uprotocol;
using namespace std::chrono_literals;

auto PendingRequest::operator>(const PendingRequest& other) const {
	return when_expire > other.when_expire;
}

ScrubablePendingQueue::~ScrubablePendingQueue() {
	const v1::UStatus cancel_reason = []() {
		v1::UStatus reason;
		reason.set_code(v1::UCode::CANCELLED);
		reason.set_message("ExpireWorker shutting down");
		return reason;
	}();

	for (auto& pending : c) {
		pending.expire(cancel_reason);
	}
}

auto ScrubablePendingQueue::scrub(size_t instance_id) {
	// Collect all the expire lambdas so they can be called without the
	// lock held.
	std::vector<std::function<void(communication::RpcClient::Status)>>
	    all_expired;

	c.erase(
	    std::remove_if(c.begin(), c.end(),
	                   [instance_id, &all_expired](const PendingRequest& p) {
		                   if (instance_id == p.instance_id) {
			                   all_expired.push_back(p.expire);
			                   return true;
		                   }
		                   return false;
	                   }),
	    c.end());

	// TODO - is there a better way to shrink the internal container?
	// Maybe instead we should enforce a capacity limit
	constexpr size_t capacity_shrink_threshold = 16;
	if ((c.capacity() > capacity_shrink_threshold) &&
	    (c.size() < c.capacity() / 2)) {
		c.shrink_to_fit();
	}

	return all_expired;
}

// Exposing non-const version so the listen handle can be moved out
PendingRequest& ScrubablePendingQueue::top() { return c.front(); }

ExpireWorker::ExpireWorker() {
	worker_ = std::thread([this]() { doWork(); });
}

ExpireWorker::~ExpireWorker() {
	stop_ = true;
	{
		std::lock_guard lock(pending_mtx_);
		wake_worker_.notify_one();
	}
	worker_.join();
}

void ExpireWorker::enqueue(PendingRequest&& pending) {
	std::lock_guard lock(pending_mtx_);
	pending_.emplace(std::move(pending));
	wake_worker_.notify_one();
}

void ExpireWorker::scrub(size_t instance_id) {
	std::vector<std::function<void(communication::RpcClient::Status)>>
	    all_expired;
	{
		std::lock_guard lock(pending_mtx_);
		all_expired = pending_.scrub(instance_id);
		wake_worker_.notify_one();
	}

	static const v1::UStatus cancel_reason = []() {
		v1::UStatus reason;
		reason.set_code(v1::UCode::CANCELLED);
		reason.set_message("RpcClient for this request was discarded");
		return reason;
	}();

	for (auto& expire : all_expired) {
		expire(cancel_reason);
	}
}

void ExpireWorker::doWork() {
	while (!stop_) {
		const auto now = std::chrono::steady_clock::now();
		std::optional<decltype(PendingRequest::expire)> maybe_expire;

		{
			transport::UTransport::ListenHandle expired_handle;
			std::lock_guard lock(pending_mtx_);
			if (!pending_.empty()) {
				const auto when_expire = pending_.top().when_expire;
				if (when_expire <= now) {
					maybe_expire = pending_.top().expire;
					expired_handle =
					    std::move(pending_.top().response_listener);
					pending_.pop();
				}
			}
		}

		if (maybe_expire) {
			auto& expire = *maybe_expire;

			static const v1::UStatus expire_reason = []() {
				v1::UStatus reason;
				reason.set_code(v1::UCode::DEADLINE_EXCEEDED);
				reason.set_message("Request expired before response received");
				return reason;
			}();

			expire(expire_reason);

		} else {
			std::unique_lock lock(pending_mtx_);
			if (pending_.empty()) {
				wake_worker_.wait(
				    lock, [this]() { return stop_ || !pending_.empty(); });
			} else {
				auto wake_when = pending_.top().when_expire;
				wake_worker_.wait_until(lock, wake_when, [this, &wake_when]() {
					auto when_next_wake = wake_when;
					if (!pending_.empty()) {
						when_next_wake =
						    std::min(wake_when, pending_.top().when_expire);
					}
					return stop_ ||
					       (std::chrono::steady_clock::now() >= when_next_wake);
				});
			}
		}
	}
}

}  // namespace detail
}  // namespace
