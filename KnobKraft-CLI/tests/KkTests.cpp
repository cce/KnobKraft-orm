/*
   Copyright (c) 2026 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

// End-to-end tests for kk: the real CLI code talks over loopback IPC to an
// in-process SessionIpcServer backed by MidiKraft's FakeSessionService.

#include "Commands.h"

#include "FakeSessionService.h"
#include "SessionTransport.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>

using namespace midikraft::session;
using namespace std::chrono_literals;

namespace {
	int failures = 0;

	void check(bool condition, char const* expression, int line) {
		if (!condition) {
			std::cerr << "line " << line << ": check failed: " << expression << '\n';
			++failures;
		}
	}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

	std::filesystem::path uniqueTestDirectory(std::string const& name) {
		auto const value = std::chrono::steady_clock::now().time_since_epoch().count();
		auto path = std::filesystem::temp_directory_path() / ("kk-test-" + name + "-" + std::to_string(value));
		std::filesystem::create_directories(path);
		return path;
	}

	// FakeSessionService only moves transfers forward when told to. Doing that
	// from inside getTransferStatus keeps every call on the server's executor,
	// because the fake is not thread-safe. It can also plan a failure for the
	// next transfer, whose request ID is generated inside kk.
	class SteppingService final : public SessionService {
	public:
		FakeSessionService fake;
		std::optional<FakeTransferBehavior> nextTransferBehavior;

		ServiceResult<ServiceResponse<ServerInfo>> getServerInfo(RequestContext const& r) override { return fake.getServerInfo(r); }
		ServiceResult<ServiceResponse<PagedItems<SessionSynthInfo>>> listConfiguredSynthInstances(ListSynthsRequest const& r) override { return fake.listConfiguredSynthInstances(r); }
		ServiceResult<ServiceResponse<SessionSynthInfo>> getConfiguredSynthInstance(GetSynthRequest const& r) override { return fake.getConfiguredSynthInstance(r); }
		ServiceResult<ServiceResponse<PagedItems<PatchSummary>>> searchPatches(SearchPatchesRequest const& r) override { return fake.searchPatches(r); }
		ServiceResult<ServiceResponse<SessionPatch>> getPatch(GetPatchRequest const& r) override { return fake.getPatch(r); }
		ServiceResult<ServiceResponse<TransferRecord>> applyToEditBuffer(ApplyToEditBufferRequest const& r) override {
			if (nextTransferBehavior) fake.setTransferBehavior(r.context.requestId, *std::exchange(nextTransferBehavior, std::nullopt));
			return fake.applyToEditBuffer(r);
		}
		ServiceResult<ServiceResponse<TransferRecord>> getTransferStatus(GetTransferStatusRequest const& r) override {
			fake.advanceTransfers();
			return fake.getTransferStatus(r);
		}
		ServiceResult<ServiceResponse<TransferRecord>> cancelTransfer(CancelTransferRequest const& r) override { return fake.cancelTransfer(r); }
		ServiceResult<ServiceResponse<NavigationResult>> openKnobKraft(OpenKnobKraftRequest const& r) override { return fake.openKnobKraft(r); }
		ServiceResult<ServiceResponse<SessionSnapshot>> publishSession(PublishSessionRequest const& r) override { return fake.publishSession(r); }
		ServiceResult<ServiceResponse<SessionSnapshot>> disconnectSession(DisconnectSessionRequest const& r) override { return fake.disconnectSession(r); }
		SessionSnapshot currentSnapshot() const override { return fake.currentSnapshot(); }
		ObserverId subscribe(SessionObserver observer, bool emitCurrent) override { return fake.subscribe(std::move(observer), emitCurrent); }
		void unsubscribe(ObserverId id) override { fake.unsubscribe(id); }
	};

	struct Harness {
		std::filesystem::path directory = uniqueTestDirectory("server");
		std::filesystem::path discoveryPath = directory / "bridge.json";
		SteppingService service;
		SessionIpcServer server;

		Harness() : server(service, config(discoveryPath)) {
			CHECK(server.start());
		}

		~Harness() {
			server.stop();
			std::error_code ignored;
			std::filesystem::remove_all(directory, ignored);
		}

		static SessionIpcServerConfig config(std::filesystem::path const& path) {
			static std::atomic<int> counter { 0 };
			SessionIpcServerConfig result;
			result.discoveryFile = std::make_shared<DiscoveryFile>(path);
			result.tokenGenerator = [] { return "0123456789abcdef-kk-test-" + std::to_string(counter.fetch_add(1)); };
			return result;
		}

		struct Result {
			int code;
			std::string out;
			std::string err;
		};

		Result run(std::vector<std::string> args) {
			args.insert(args.end(), { "--discovery-file", discoveryPath.string(), "--wait-timeout", "5" });
			std::ostringstream out;
			std::ostringstream err;
			auto const code = kk::run(args, out, err);
			return { code, out.str(), err.str() };
		}
	};

	bool has(std::string const& text, std::string const& part) {
		return text.find(part) != std::string::npos;
	}

	void infoAndSynths(Harness& h) {
		auto info = h.run({ "info" });
		CHECK(info.code == kk::ExitOk);
		CHECK(has(info.out, "KnobKraft Fake Session Service"));

		auto synths = h.run({ "synths" });
		CHECK(synths.code == kk::ExitOk);
		CHECK(has(synths.out, "Studio Matrix-1000"));
		CHECK(has(synths.out, "offline"));

		auto json = h.run({ "synths", "--json" });
		CHECK(json.code == kk::ExitOk);
		auto parsed = kk::Json::parse(json.out);
		CHECK(parsed.is_array() && parsed.size() == 3);
	}

	void pagingCollectsEveryItem(Harness& h) {
		kk::ClientOptions options;
		options.discoveryFile = h.discoveryPath;
		options.pageSize = 1;
		kk::KkClient client(options);
		CHECK(client.connect());
		auto all = client.callAllPages(ipc_operation::LIST_CONFIGURED_SYNTH_INSTANCES, kk::Json::object());
		CHECK(all && all.value().size() == 3);
		auto limited = client.callAllPages(ipc_operation::SEARCH_PATCHES, { { "query", "" } }, 2);
		CHECK(limited && limited.value().size() == 2);
	}

	void searchAndShow(Harness& h) {
		auto pad = h.run({ "search", "pad" });
		CHECK(pad.code == kk::ExitOk);
		CHECK(has(pad.out, "Cloud Pad"));
		CHECK(!has(pad.out, "Warm Bass"));

		auto bySynth = h.run({ "search", "--synth", "Prophet" });
		CHECK(bySynth.code == kk::ExitOk);
		CHECK(has(bySynth.out, "Cloud Pad"));
		CHECK(!has(bySynth.out, "Brass Stack"));

		auto ambiguousSynth = h.run({ "search", "--synth", "Matrix" });
		CHECK(ambiguousSynth.code == kk::ExitServiceError);
		CHECK(has(ambiguousSynth.err, "matches several synths"));

		auto show = h.run({ "show", "patch-warm-bass" });
		CHECK(show.code == kk::ExitOk);
		CHECK(has(show.out, "Warm Bass"));
		CHECK(has(show.out, "4 bytes"));

		auto missing = h.run({ "show", "patch-nope" });
		CHECK(missing.code == kk::ExitServiceError);
		CHECK(has(missing.err, "PatchNotFound"));
	}

	void sendSucceeds(Harness& h) {
		auto bySearch = h.run({ "send", "--search", "cloud pad" });
		CHECK(bySearch.code == kk::ExitOk);
		CHECK(has(bySearch.out, "Sent 'Cloud Pad' to Desk Prophet-6"));

		// Two Matrix-1000s are configured, but only one is online.
		auto byId = h.run({ "send", "patch-warm-bass" });
		CHECK(byId.code == kk::ExitOk);
		CHECK(has(byId.out, "to Studio Matrix-1000"));

		auto noWait = h.run({ "send", "patch-brass-stack", "--no-wait", "--json" });
		CHECK(noWait.code == kk::ExitOk);
		auto transfer = kk::Json::parse(noWait.out);
		auto const transferId = transfer.at("status").at("transferId").get<std::string>();
		auto status = h.run({ "status", transferId });
		CHECK(status.code == kk::ExitOk);
		CHECK(has(status.out, transferId));
	}

	void sendRefusesBadInput(Harness& h) {
		auto ambiguous = h.run({ "send", "--search", "a" });
		CHECK(ambiguous.code == kk::ExitUsage);
		CHECK(has(ambiguous.err, "matches 3 patches"));

		auto offline = h.run({ "send", "patch-warm-bass", "--synth", "Touring" });
		CHECK(offline.code == kk::ExitServiceError);
		CHECK(has(offline.err, "SynthOffline"));

		auto wrongSynth = h.run({ "send", "patch-warm-bass", "--synth", "Prophet" });
		CHECK(wrongSynth.code == kk::ExitUsage);

		auto unknown = h.run({ "frobnicate" });
		CHECK(unknown.code == kk::ExitUsage);
	}

	void sendReportsFailedTransfer(Harness& h) {
		h.service.nextTransferBehavior = FakeTransferBehavior { TransferState::Sending,
			{ ServiceErrorCode::MidiPortBusy, "MIDI port is busy", true } };
		auto failed = h.run({ "send", "patch-cloud-pad" });
		CHECK(failed.code == kk::ExitTransferFailed);
		CHECK(has(failed.err, "failed"));
		CHECK(has(failed.err, "MIDI port is busy"));
	}

	void notRunningIsReported() {
		auto directory = uniqueTestDirectory("missing");
		std::ostringstream out;
		std::ostringstream err;
		auto const code = kk::run({ "synths", "--discovery-file", (directory / "none.json").string(), "--connect-timeout", "0.3" }, out, err);
		CHECK(code == kk::ExitNotConnected);
		CHECK(has(err.str(), "KnobKraft is not running"));
		std::filesystem::remove_all(directory);
	}
}

int main() {
	{
		Harness harness;
		infoAndSynths(harness);
		pagingCollectsEveryItem(harness);
		searchAndShow(harness);
		sendSucceeds(harness);
		sendRefusesBadInput(harness);
		sendReportsFailedTransfer(harness);
	}
	notRunningIsReported();
	if (failures == 0) std::cout << "kk tests passed\n";
	return failures == 0 ? 0 : 1;
}
