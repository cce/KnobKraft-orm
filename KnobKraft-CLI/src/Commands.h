/*
   Copyright (c) 2026 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#pragma once

#include "KkClient.h"

#include <chrono>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace kk {

	enum ExitCode : int {
		ExitOk = 0,
		ExitUsage = 1,
		ExitServiceError = 2,
		ExitNotConnected = 3,
		ExitTransferFailed = 4
	};

	struct Arguments {
		std::string command;
		std::vector<std::string> positional;
		bool json = false;
		bool wait = true;
		std::optional<std::string> synth;
		std::optional<std::string> search;
		std::optional<std::string> patch;
		std::optional<std::size_t> limit;
		std::chrono::milliseconds waitTimeout { 60'000 };
		std::chrono::milliseconds pollInterval { 100 };
	};

	// Parses everything after the program name. Returns an error message on failure.
	std::optional<std::string> parseArguments(std::vector<std::string> const& argv, Arguments& result, ClientOptions& options);

	void printUsage(std::ostream& out);

	// Runs one command against an already connected client.
	int runCommand(KkClient& client, Arguments const& args, std::ostream& out, std::ostream& err);

	// Connects (reporting ExitNotConnected if KnobKraft is unreachable) and runs the command.
	int run(std::vector<std::string> const& argv, std::ostream& out, std::ostream& err);

}
