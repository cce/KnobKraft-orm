/*
   Copyright (c) 2026 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#include "KkClient.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <random>

namespace kk {

	using namespace midikraft::session;

	namespace {
		std::int64_t nowUnixMillis() {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

		std::filesystem::path environmentPath(char const* name) {
			auto const* value = std::getenv(name);
			return value && *value ? std::filesystem::path(value) : std::filesystem::path {};
		}
	}

	std::filesystem::path defaultDiscoveryFilePath() {
#if defined(_WIN32)
		auto base = environmentPath("APPDATA");
#elif defined(__APPLE__)
		auto base = environmentPath("HOME") / "Library";
#else
		auto base = environmentPath("XDG_CONFIG_HOME");
		if (base.empty()) base = environmentPath("HOME") / ".config";
#endif
		return base / "KnobKraftOrm" / "recall-session-v1.json";
	}

	std::string randomHex(std::size_t bytes) {
		static constexpr char digits[] = "0123456789abcdef";
		std::random_device device;
		std::string result;
		result.reserve(bytes * 2);
		for (std::size_t i = 0; i < bytes; ++i) {
			auto const value = static_cast<unsigned>(device()) & 0xffU;
			result.push_back(digits[value >> 4]);
			result.push_back(digits[value & 0x0fU]);
		}
		return result;
	}

	std::string errorCodeName(ServiceErrorCode code) {
		static constexpr std::array names {
			"Unavailable", "ProtocolIncompatible", "AuthenticationFailed", "InvalidRequest", "DeadlineExceeded",
			"ConfiguredSynthMissing", "SynthOffline", "MidiPortBusy", "PatchIncompatible", "PatchDataInvalid",
			"AdaptationError", "TransferTimedOut", "TransferCancelled", "VerificationMismatch", "PatchNotFound",
			"TransferNotFound", "CancelNotAllowed", "NavigationUnavailable", "InternalError"
		};
		auto const index = static_cast<std::size_t>(code);
		return index < names.size() ? names[index] : "Error" + std::to_string(index);
	}

	KkClient::KkClient(ClientOptions options) : options_(std::move(options)) {
		if (options_.discoveryFile.empty()) options_.discoveryFile = defaultDiscoveryFilePath();
		if (options_.clientId.empty()) options_.clientId = "kk-" + randomHex(8);
		if (!options_.requestIdGenerator) options_.requestIdGenerator = [] { return "kk-" + randomHex(16); };

		SessionIpcClientConfig config;
		config.discoveryFile = std::make_shared<DiscoveryFile>(options_.discoveryFile);
		config.nowUnixMillis = nowUnixMillis;
		client_ = std::make_unique<SessionIpcClient>(std::move(config));
	}

	KkClient::~KkClient() {
		client_->stop();
	}

	bool KkClient::connect() {
		std::mutex mutex;
		std::condition_variable changed;
		bool connected = false;
		client_->setConnectionObserver([&](bool value) {
			std::lock_guard lock(mutex);
			connected = value;
			changed.notify_all();
		});
		client_->start();
		std::unique_lock lock(mutex);
		auto const result = changed.wait_for(lock, options_.connectTimeout, [&] { return connected; });
		lock.unlock();
		// The observer captures locals, so detach it before they go out of scope.
		client_->setConnectionObserver({});
		return result || client_->isConnected();
	}

	ServiceResult<Json> KkClient::call(std::string_view operation, Json body) {
		RequestContext context { options_.requestIdGenerator(), options_.clientId, options_.pluginInstanceId,
			nowUnixMillis() + options_.requestTimeout.count() };
		auto future = client_->request(std::string(operation), context, body.dump());
		// The client expires the request at its deadline; the extra second only
		// guards against a worker that never answers at all.
		if (future.wait_for(options_.requestTimeout + std::chrono::seconds(1)) != std::future_status::ready) {
			client_->cancelPending(context.requestId);
			return ServiceResult<Json>::failure({ ServiceErrorCode::DeadlineExceeded, "KnobKraft did not answer in time", true });
		}
		auto response = future.get();
		if (response.error) return ServiceResult<Json>::failure(*response.error);
		if (!response.payloadJson) return ServiceResult<Json>::failure({ ServiceErrorCode::InternalError, "Empty response from KnobKraft", false });
		try {
			return ServiceResult<Json>::success(Json::parse(*response.payloadJson));
		}
		catch (std::exception const& ex) {
			return ServiceResult<Json>::failure({ ServiceErrorCode::InternalError, std::string("Malformed response: ") + ex.what(), false });
		}
	}

	ServiceResult<std::vector<Json>> KkClient::callAllPages(std::string_view operation, Json body, std::size_t maxItems) {
		std::vector<Json> items;
		body["pageSize"] = options_.pageSize;
		while (items.size() < maxItems) {
			auto page = call(operation, body);
			if (!page) return ServiceResult<std::vector<Json>>::failure(page.error());
			for (auto const& item : page.value().value("items", Json::array())) {
				if (items.size() == maxItems) break;
				items.push_back(item);
			}
			if (!page.value().contains("nextPageToken")) break;
			body["pageToken"] = page.value().at("nextPageToken");
		}
		return ServiceResult<std::vector<Json>>::success(std::move(items));
	}

}
