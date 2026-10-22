// Steam overlay (1.2): with frame generation running, the picture on screen is the bridge's own window over Factorio's.
// Steam draws its overlay into Factorio's frames, under that window, so it opens but cannot be seen. While it is open
// the bridge window is hidden (as for the ReShade overlay): the overlay works, without frame generation meanwhile.
//
// Detection: Steam's GameOverlayActivated notice (steam_api64.dll, the callback Factorio's own Steam integration
// dispatches). If Steam never sends it (a game may dispatch callbacks itself), the overlay key is watched instead
// (Shift+Tab, Steam's default; Esc or Shift+Tab again close it).

#pragma once

#include <windows.h>
#include <cstddef>

namespace steam_overlay
{
	// Once per frame on Factorio's present thread: registers with Steam when its library is there, watches the key.
	// 'game_window': Factorio's window (the key only counts while it is in front).
	void update(HWND game_window, bool use_key);
	bool active();                 // the overlay is open (by Steam's notice, or by the key)
	void status(char *buf, size_t size);
	void shutdown();               // unregisters (before the add-on is unloaded)
}
