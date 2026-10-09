/*
   Copyright (c) 2026 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#include "Commands.h"

#include <iostream>

int main(int argc, char* argv[]) {
	std::vector<std::string> arguments(argv + 1, argv + argc);
	return kk::run(arguments, std::cout, std::cerr);
}
