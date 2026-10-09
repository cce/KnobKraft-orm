/*
   Copyright (c) 2026 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#pragma once

#include "SessionTransport.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kk {

	using Json = nlohmann::json;
	using midikraft::session::ServiceError;
	using midikraft::session::ServiceErrorCode;
	using midikraft::session::ServiceResult;

	// Where the running KnobKraft application publishes its session discovery
	// record. Mirrors recallDiscoveryFilePath() in The-Orm/PluginBridgeServer.cpp,
	// which uses JUCE's userApplicationDataDirectory.
	std::filesystem::path defaultDiscoveryFilePath();

	struct ClientOptions {
		std::filesystem::path discoveryFile;
		std::chrono::milliseconds connectTimeout { 2'000 };
		std::chrono::milliseconds requestTimeout { 10'000 };
		std::string clientId;
		std::string pluginInstanceId = "kk";
		// Must not exceed the server page limit (200 in KnobKraft, 100 in the fake service).
		std::uint32_t pageSize = 100;
		std::function<std::string()> requestIdGenerator;
	};

	// A blocking facade over the asynchronous SessionIpcClient. The CLI issues
	// one request at a time, so it simply waits on each future.
	class KkClient {
	public:
		explicit KkClient(ClientOptions options);
		~KkClient();

		KkClient(KkClient const&) = delete;
		KkClient& operator=(KkClient const&) = delete;

		// Returns false if no KnobKraft server could be reached in time.
		[[nodiscard]] bool connect();

		[[nodiscard]] ServiceResult<Json> call(std::string_view operation, Json body = Json::object());

		// Follows nextPageToken until every item has been collected, or maxItems is reached.
		[[nodiscard]] ServiceResult<std::vector<Json>> callAllPages(std::string_view operation, Json body,
			std::size_t maxItems = SIZE_MAX);

		[[nodiscard]] ClientOptions const& options() const noexcept { return options_; }

	private:
		ClientOptions options_;
		std::unique_ptr<midikraft::session::SessionIpcClient> client_;
	};

	std::string randomHex(std::size_t bytes);
	std::string errorCodeName(ServiceErrorCode code);

}
