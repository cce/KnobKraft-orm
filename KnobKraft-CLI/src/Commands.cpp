/*
   Copyright (c) 2026 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#include "Commands.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <thread>

namespace kk {

	using namespace midikraft::session;

	namespace {
		std::string lowerCase(std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		bool contains(std::string const& haystack, std::string const& needle) {
			return lowerCase(haystack).find(lowerCase(needle)) != std::string::npos;
		}

		std::string text(Json const& value, char const* key) {
			auto found = value.find(key);
			if (found == value.end() || found->is_null()) return {};
			return found->is_string() ? found->get<std::string>() : found->dump();
		}

		std::string transferStateName(int state) {
			static constexpr std::array names { "accepted", "queued", "preparing", "sending", "verifying", "succeeded", "failed", "cancelled" };
			return state >= 0 && static_cast<std::size_t>(state) < names.size() ? names[static_cast<std::size_t>(state)] : "state " + std::to_string(state);
		}

		std::string verificationName(int state) {
			static constexpr std::array names { "not attempted", "unverified", "verified", "mismatch" };
			return state >= 0 && static_cast<std::size_t>(state) < names.size() ? names[static_cast<std::size_t>(state)] : "unknown";
		}

		bool isTerminal(int state) {
			return state == static_cast<int>(TransferState::Succeeded) || state == static_cast<int>(TransferState::Failed)
				|| state == static_cast<int>(TransferState::Cancelled);
		}

		int reportError(std::ostream& err, ServiceError const& error) {
			err << "kk: " << errorCodeName(error.code) << ": " << error.message << '\n';
			return ExitServiceError;
		}

		// Column-aligned plain text table; the last column is never padded.
		void printTable(std::ostream& out, std::vector<std::string> const& header, std::vector<std::vector<std::string>> const& rows) {
			std::vector<std::size_t> widths(header.size());
			for (std::size_t i = 0; i < header.size(); ++i) widths[i] = header[i].size();
			for (auto const& row : rows)
				for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) widths[i] = std::max(widths[i], row[i].size());
			auto printRow = [&](std::vector<std::string> const& row) {
				for (std::size_t i = 0; i < row.size(); ++i) {
					if (i + 1 == row.size()) out << row[i];
					else out << std::left << std::setw(static_cast<int>(widths[i] + 2)) << row[i];
				}
				out << '\n';
			};
			printRow(header);
			for (auto const& row : rows) printRow(row);
		}

		std::string capabilities(Json const& synth) {
			std::vector<std::string> names;
			auto const caps = synth.value("capabilities", Json::object());
			if (caps.value("editBuffer", false)) names.emplace_back("edit-buffer");
			if (caps.value("programDump", false)) names.emplace_back("program-dump");
			if (caps.value("customProgramChange", false)) names.emplace_back("program-change");
			if (caps.value("verification", false)) names.emplace_back("verify");
			std::string result;
			for (auto const& name : names) result += (result.empty() ? "" : ",") + name;
			return result;
		}

		std::string location(Json const& patch) {
			auto const source = patch.value("source", Json::object());
			if (!source.contains("bank") && !source.contains("program")) return {};
			return text(source, "bank") + "/" + text(source, "program");
		}

		std::string describeTransfer(Json const& transfer) {
			auto const status = transfer.value("status", Json::object());
			std::ostringstream result;
			result << transferStateName(status.value("state", 0));
			if (status.contains("progress")) result << " " << static_cast<int>(status.value("progress", 0.0) * 100.0) << "%";
			auto const detail = text(status, "detail");
			if (!detail.empty()) result << " - " << detail;
			return result.str();
		}

		// Matches by configured-synth id, then exact name, then name substring.
		ServiceResult<Json> resolveSynth(std::vector<Json> const& synths, std::string const& spec) {
			auto match = [&](auto&& predicate) {
				std::vector<Json> found;
				std::copy_if(synths.begin(), synths.end(), std::back_inserter(found), predicate);
				return found;
			};
			std::vector<std::vector<Json>> tiers {
				match([&](Json const& s) { return text(s, "configuredSynthInstanceId") == spec; }),
				match([&](Json const& s) { return lowerCase(text(s, "displayName")) == lowerCase(spec) || lowerCase(text(s, "adaptationId")) == lowerCase(spec); }),
				match([&](Json const& s) { return contains(text(s, "displayName"), spec) || contains(text(s, "adaptationId"), spec); })
			};
			for (auto const& tier : tiers) {
				if (tier.size() == 1) return ServiceResult<Json>::success(tier.front());
				if (tier.size() > 1) {
					std::string names;
					for (auto const& s : tier) names += "\n  " + text(s, "configuredSynthInstanceId") + "  " + text(s, "displayName");
					return ServiceResult<Json>::failure({ ServiceErrorCode::InvalidRequest, "'" + spec + "' matches several synths, use the id:" + names, false });
				}
			}
			return ServiceResult<Json>::failure({ ServiceErrorCode::ConfiguredSynthMissing, "No configured synth matches '" + spec + "' (see kk synths)", false });
		}

		int usageError(std::ostream& err, std::string const& message) {
			err << "kk: " << message << '\n';
			return ExitUsage;
		}

		int cmdInfo(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err) {
			auto info = client.call(ipc_operation::GET_SERVER_INFO);
			if (!info) return reportError(err, info.error());
			if (args.json) { out << info.value().dump(2) << '\n'; return ExitOk; }
			auto const& v = info.value();
			out << text(v, "productName") << ' ' << text(v, "productVersion") << '\n'
				<< "protocol " << text(v, "protocolMajor") << '.' << text(v, "protocolMinor") << ", generation " << text(v, "generationId") << '\n';
			return ExitOk;
		}

		int cmdSynths(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err) {
			auto synths = client.callAllPages(ipc_operation::LIST_CONFIGURED_SYNTH_INSTANCES, Json::object());
			if (!synths) return reportError(err, synths.error());
			if (args.json) { out << Json(synths.value()).dump(2) << '\n'; return ExitOk; }
			std::vector<std::vector<std::string>> rows;
			for (auto const& s : synths.value())
				rows.push_back({ text(s, "displayName"), s.value("online", false) ? "online" : "offline", capabilities(s),
					text(s, "configuredSynthInstanceId") });
			printTable(out, { "SYNTH", "STATUS", "CAPABILITIES", "ID" }, rows);
			return ExitOk;
		}

		ServiceResult<std::vector<Json>> searchPatches(KkClient& client, std::string const& query,
			std::optional<std::string> const& synthSpec, std::size_t limit) {
			Json body { { "query", query } };
			if (synthSpec) {
				auto synths = client.callAllPages(ipc_operation::LIST_CONFIGURED_SYNTH_INSTANCES, Json::object());
				if (!synths) return ServiceResult<std::vector<Json>>::failure(synths.error());
				auto synth = resolveSynth(synths.value(), *synthSpec);
				if (!synth) return ServiceResult<std::vector<Json>>::failure(synth.error());
				body["adaptationId"] = text(synth.value(), "adaptationId");
			}
			return client.callAllPages(ipc_operation::SEARCH_PATCHES, body, limit);
		}

		void printPatches(std::ostream& out, std::vector<Json> const& patches) {
			std::vector<std::vector<std::string>> rows;
			for (auto const& p : patches) rows.push_back({ text(p, "name"), text(p, "adaptationId"), location(p), text(p, "patchId") });
			printTable(out, { "NAME", "SYNTH", "BANK/PROG", "PATCH ID" }, rows);
		}

		int cmdSearch(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err) {
			if (args.positional.size() > 1) return usageError(err, "search takes one search text (quote it if it has spaces)");
			auto const query = args.positional.empty() ? std::string {} : args.positional.front();
			auto patches = searchPatches(client, query, args.synth, args.limit.value_or(50));
			if (!patches) return reportError(err, patches.error());
			if (args.json) { out << Json(patches.value()).dump(2) << '\n'; return ExitOk; }
			if (patches.value().empty()) { err << "kk: no patches match '" << query << "'\n"; return ExitOk; }
			printPatches(out, patches.value());
			return ExitOk;
		}

		int cmdShow(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err) {
			if (args.positional.size() != 1) return usageError(err, "show needs exactly one patch id");
			auto patch = client.call(ipc_operation::GET_PATCH, { { "patchId", args.positional.front() } });
			if (!patch) return reportError(err, patch.error());
			auto const& p = patch.value();
			if (args.json) { out << p.dump(2) << '\n'; return ExitOk; }
			// The payload is base64; report its decoded size without decoding.
			auto const encoded = text(p, "payload");
			auto const padding = static_cast<std::size_t>(std::count(encoded.end() - std::min<std::ptrdiff_t>(2, static_cast<std::ptrdiff_t>(encoded.size())), encoded.end(), '='));
			out << "name         " << text(p, "name") << '\n'
				<< "synth        " << text(p, "adaptationId") << '\n'
				<< "data type    " << text(p, "dataTypeId") << '\n'
				<< "fingerprint  " << text(p, "fingerprint") << '\n'
				<< "size         " << (encoded.size() / 4 * 3 - padding) << " bytes\n";
			if (!location(p).empty()) out << "bank/prog    " << location(p) << '\n';
			return ExitOk;
		}

		int printTransfer(Json const& transfer, Arguments const& args, std::ostream& out) {
			if (args.json) { out << transfer.dump(2) << '\n'; return ExitOk; }
			auto const status = transfer.value("status", Json::object());
			out << text(status, "transferId") << "  " << text(transfer, "patchName") << "  " << describeTransfer(transfer) << '\n';
			if (transfer.contains("error")) out << "error: " << text(transfer.at("error"), "message") << '\n';
			return ExitOk;
		}

		int cmdSend(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err) {
			std::string patchId;
			if (args.search) {
				if (!args.positional.empty()) return usageError(err, "give either a patch id or --search, not both");
				auto matches = searchPatches(client, *args.search, args.synth, 50);
				if (!matches) return reportError(err, matches.error());
				auto const& found = matches.value();
				std::vector<Json> exact;
				std::copy_if(found.begin(), found.end(), std::back_inserter(exact),
					[&](Json const& p) { return lowerCase(text(p, "name")) == lowerCase(*args.search); });
				if (found.size() == 1) patchId = text(found.front(), "patchId");
				else if (exact.size() == 1) patchId = text(exact.front(), "patchId");
				else if (found.empty()) { err << "kk: no patches match '" << *args.search << "'\n"; return ExitUsage; }
				else {
					err << "kk: '" << *args.search << "' matches " << found.size() << " patches, be more specific or pass a patch id:\n";
					printPatches(err, found);
					return ExitUsage;
				}
			}
			else if (args.positional.size() == 1) patchId = args.positional.front();
			else return usageError(err, "send needs a patch id or --search TEXT");

			auto patch = client.call(ipc_operation::GET_PATCH, { { "patchId", patchId } });
			if (!patch) return reportError(err, patch.error());
			auto const adaptationId = text(patch.value(), "adaptationId");

			auto synths = client.callAllPages(ipc_operation::LIST_CONFIGURED_SYNTH_INSTANCES, Json::object());
			if (!synths) return reportError(err, synths.error());
			Json target;
			if (args.synth) {
				auto synth = resolveSynth(synths.value(), *args.synth);
				if (!synth) return reportError(err, synth.error());
				target = synth.value();
				if (text(target, "adaptationId") != adaptationId)
					return usageError(err, "patch is for " + adaptationId + ", but " + text(target, "displayName") + " is a " + text(target, "adaptationId"));
			}
			else {
				std::vector<Json> candidates;
				std::copy_if(synths.value().begin(), synths.value().end(), std::back_inserter(candidates),
					[&](Json const& s) { return text(s, "adaptationId") == adaptationId && s.value("online", false); });
				if (candidates.empty()) return usageError(err, "no online " + adaptationId + " is configured in KnobKraft");
				if (candidates.size() > 1) return usageError(err, "several online " + adaptationId + " synths are configured, choose one with --synth");
				target = candidates.front();
			}

			Json body { { "configuredSynthInstanceId", text(target, "configuredSynthInstanceId") },
				{ "expectedAdaptationId", adaptationId }, { "patch", patch.value() } };
			auto transfer = client.call(ipc_operation::APPLY_TO_EDIT_BUFFER, body);
			if (!transfer) return reportError(err, transfer.error());
			auto const transferId = text(transfer.value().value("status", Json::object()), "transferId");
			if (!args.wait) return printTransfer(transfer.value(), args, out);

			auto record = transfer.value();
			std::string lastReported;
			auto const giveUp = std::chrono::steady_clock::now() + args.waitTimeout;
			while (!isTerminal(record.value("status", Json::object()).value("state", 0))) {
				if (!args.json) {
					auto const now = describeTransfer(record);
					if (now != lastReported) err << "  " << now << '\n';
					lastReported = now;
				}
				if (std::chrono::steady_clock::now() > giveUp) {
					err << "kk: transfer " << transferId << " still running after " << args.waitTimeout.count() / 1000
						<< "s, check it with: kk status " << transferId << '\n';
					return ExitTransferFailed;
				}
				std::this_thread::sleep_for(args.pollInterval);
				auto status = client.call(ipc_operation::GET_TRANSFER_STATUS, { { "transferId", transferId } });
				if (!status) return reportError(err, status.error());
				record = status.value();
			}

			auto const status = record.value("status", Json::object());
			auto const state = status.value("state", 0);
			if (args.json) out << record.dump(2) << '\n';
			if (state == static_cast<int>(TransferState::Succeeded)) {
				if (!args.json)
					out << "Sent '" << text(patch.value(), "name") << "' to " << text(target, "displayName")
						<< " (" << verificationName(status.value("verification", 0)) << ")\n";
				return ExitOk;
			}
			if (!args.json) {
				err << "kk: transfer " << transferStateName(state);
				if (record.contains("error")) err << ": " << text(record.at("error"), "message");
				err << '\n';
			}
			return ExitTransferFailed;
		}

		int cmdTransfer(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err, std::string_view operation) {
			if (args.positional.size() != 1) return usageError(err, args.command + " needs exactly one transfer id");
			auto transfer = client.call(operation, { { "transferId", args.positional.front() } });
			if (!transfer) return reportError(err, transfer.error());
			return printTransfer(transfer.value(), args, out);
		}

		int cmdOpen(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err) {
			Json body { { "target", static_cast<int>(NavigationTargetKind::Application) } };
			if (args.synth && args.patch) return usageError(err, "give either --synth or --patch, not both");
			if (args.synth) {
				auto synths = client.callAllPages(ipc_operation::LIST_CONFIGURED_SYNTH_INSTANCES, Json::object());
				if (!synths) return reportError(err, synths.error());
				auto synth = resolveSynth(synths.value(), *args.synth);
				if (!synth) return reportError(err, synth.error());
				body = { { "target", static_cast<int>(NavigationTargetKind::ConfiguredSynth) }, { "targetId", text(synth.value(), "configuredSynthInstanceId") } };
			}
			else if (args.patch) {
				body = { { "target", static_cast<int>(NavigationTargetKind::Patch) }, { "targetId", *args.patch } };
			}
			auto result = client.call(ipc_operation::OPEN_KNOBKRAFT, body);
			if (!result) return reportError(err, result.error());
			if (args.json) out << result.value().dump(2) << '\n';
			return ExitOk;
		}

		std::optional<std::chrono::milliseconds> parseMillis(std::string const& value) {
			try {
				std::size_t used = 0;
				auto const number = std::stod(value, &used);
				if (used != value.size() || number <= 0) return std::nullopt;
				return std::chrono::milliseconds(static_cast<long long>(number * 1000.0));
			}
			catch (std::exception const&) { return std::nullopt; }
		}
	}

	void printUsage(std::ostream& out) {
		out << "kk - command-line client for a running KnobKraft Orm\n\n"
			"usage: kk <command> [options]\n\n"
			"commands:\n"
			"  info                          show the KnobKraft server version\n"
			"  synths                        list the synths configured in KnobKraft\n"
			"  search [TEXT] [--synth S] [--limit N]\n"
			"                                find patches whose name contains TEXT\n"
			"  show PATCH_ID                 show one patch\n"
			"  send PATCH_ID | --search TEXT [--synth S] [--no-wait]\n"
			"                                send a patch to the synth's edit buffer\n"
			"  status TRANSFER_ID            show a transfer\n"
			"  cancel TRANSFER_ID            cancel a transfer\n"
			"  open [--synth S | --patch PATCH_ID]\n"
			"                                bring the KnobKraft window to the front\n\n"
			"options:\n"
			"  --synth S             configured synth id, name, or part of its name (e.g. Rev2)\n"
			"  --json                print raw JSON\n"
			"  --timeout SECONDS     per-request timeout (default 10)\n"
			"  --wait-timeout SECS   how long send waits for the transfer (default 60)\n"
			"  --discovery-file F    KnobKraft discovery file (default from KK_DISCOVERY_FILE or the app data folder)\n\n"
			"exit codes: 0 ok, 1 usage, 2 KnobKraft error, 3 KnobKraft not running, 4 transfer failed\n"
			"KnobKraft must be running; kk talks to it through its local session bridge.\n";
	}

	std::optional<std::string> parseArguments(std::vector<std::string> const& argv, Arguments& result, ClientOptions& options) {
		for (std::size_t i = 0; i < argv.size(); ++i) {
			auto const& arg = argv[i];
			auto value = [&]() -> std::optional<std::string> {
				if (i + 1 >= argv.size()) return std::nullopt;
				return argv[++i];
			};
			auto requireValue = [&](std::optional<std::string>& target) -> std::optional<std::string> {
				target = value();
				if (!target) return arg + " needs a value";
				return std::nullopt;
			};
			std::optional<std::string> problem;
			if (arg == "--json") result.json = true;
			else if (arg == "--no-wait") result.wait = false;
			else if (arg == "--synth") problem = requireValue(result.synth);
			else if (arg == "--search") problem = requireValue(result.search);
			else if (arg == "--patch") problem = requireValue(result.patch);
			else if (arg == "--limit") {
				auto v = value();
				try { if (v) result.limit = static_cast<std::size_t>(std::stoul(*v)); }
				catch (std::exception const&) { v.reset(); }
				if (!v || result.limit == 0u) problem = "--limit needs a positive number";
			}
			else if (arg == "--timeout" || arg == "--wait-timeout" || arg == "--connect-timeout") {
				auto v = value();
				auto millis = v ? parseMillis(*v) : std::nullopt;
				if (!millis) problem = arg + " needs a number of seconds";
				else if (arg == "--timeout") options.requestTimeout = *millis;
				else if (arg == "--wait-timeout") result.waitTimeout = *millis;
				else options.connectTimeout = *millis;
			}
			else if (arg == "--discovery-file") {
				auto v = value();
				if (!v) problem = "--discovery-file needs a path";
				else options.discoveryFile = *v;
			}
			else if (arg == "-h" || arg == "--help") result.command = "help";
			else if (arg.size() > 1 && arg[0] == '-') problem = "unknown option " + arg;
			else if (result.command.empty()) result.command = arg;
			else result.positional.push_back(arg);
			if (problem) return problem;
		}
		if (result.command.empty()) result.command = "help";
		return std::nullopt;
	}

	int runCommand(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err) {
		if (args.command == "info") return cmdInfo(client, args, out, err);
		if (args.command == "synths") return cmdSynths(client, args, out, err);
		if (args.command == "search") return cmdSearch(client, args, out, err);
		if (args.command == "show") return cmdShow(client, args, out, err);
		if (args.command == "send") return cmdSend(client, args, out, err);
		if (args.command == "status") return cmdTransfer(client, args, out, err, ipc_operation::GET_TRANSFER_STATUS);
		if (args.command == "cancel") return cmdTransfer(client, args, out, err, ipc_operation::CANCEL_TRANSFER);
		if (args.command == "open") return cmdOpen(client, args, out, err);
		return usageError(err, "unknown command '" + args.command + "' (try kk --help)");
	}

	int run(std::vector<std::string> const& argv, std::ostream& out, std::ostream& err) {
		Arguments args;
		ClientOptions options;
		if (auto const* path = std::getenv("KK_DISCOVERY_FILE"); path && *path) options.discoveryFile = path;
		if (auto problem = parseArguments(argv, args, options)) return usageError(err, *problem + " (try kk --help)");
		if (args.command == "help") { printUsage(out); return ExitOk; }

		KkClient client(options);
		if (!client.connect()) {
			err << "kk: KnobKraft is not running, or its session bridge is inactive\n"
				<< "    (looked for " << client.options().discoveryFile.string() << ")\n";
			return ExitNotConnected;
		}
		return runCommand(client, args, out, err);
	}

}
