// Setup check: what frame generation needs outside the add-on itself (Windows settings, Streamline and
// dlssg_for_sm86 files, Factorio version). Runs at start and on request; results are shown in the overlay and logged.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace setup
{
	enum class state { ok, warning, problem };

	struct item
	{
		std::string name;
		state result = state::ok;
		std::string detail;
	};

	// 'addon' is this add-on's module (to find its folder). Safe to call again.
	void run(HMODULE addon);
	const std::vector<item> &items();
	// Factorio's file version ("2.0.69.0"), empty if unknown.
	const std::string &factorio_version();
	// Number of items that are problems (not warnings).
	int problems();
}
