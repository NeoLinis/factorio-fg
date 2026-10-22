#include "steam_overlay.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace steam_overlay
{
	namespace
	{
		// Steam's CCallbackBase as steam_api64.dll sees it (MSVC x64 layout): vtable, flags byte, callback id. The
		// vtable of the SDK's class is { Run(param, io_failure, call), Run(param), GetCallbackSizeBytes() } with MSVC
		// (overloads in reverse order); both Run entries here only read 'param', so either order works.
		struct callback_object
		{
			const void *const *vtable;
			uint8_t flags;
			int32_t id;
			uint8_t spare[48]; // room in case Steam keeps more of the derived class's members than the base
		};
		constexpr int32_t GAME_OVERLAY_ACTIVATED = 331; // k_iSteamFriendsCallbacks (300) + 31

		std::atomic<bool> g_steam_open{ false }, g_key_open{ false };
		std::atomic<uint32_t> g_notices{ 0 };
		bool g_registered = false, g_tried = false, g_no_library = false;
		HMODULE g_steam = nullptr;
		bool g_shift_tab_down = false, g_esc_down = false;
		callback_object g_cb = {};

		void run(void * /*self*/, void *param)
		{
			// GameOverlayActivated_t: uint8 m_bActive first
			const bool open = param != nullptr && *static_cast<const uint8_t *>(param) != 0;
			g_steam_open.store(open);
			g_key_open.store(false);
			g_notices.fetch_add(1);
		}
		void run_ex(void *self, void *param)
		{
			run(self, param);
		}
		int size_bytes(void *)
		{
			return 12; // uint8 active, bool user initiated, AppId_t, uint32 overlay pid (older SDKs: 1)
		}
		const void *const g_vtable[3] = { reinterpret_cast<const void *>(&run_ex), reinterpret_cast<const void *>(&run),
			reinterpret_cast<const void *>(&size_bytes) };

		using register_fn = void(__cdecl *)(void *callback, int id);

		void try_register()
		{
			if (g_registered || g_tried)
				return;
			g_steam = GetModuleHandleW(L"steam_api64.dll");
			if (g_steam == nullptr)
			{
				g_no_library = true;
				return; // not loaded (yet): try again next frame
			}
			g_tried = true;
			g_no_library = false;
			const auto reg = reinterpret_cast<register_fn>(reinterpret_cast<void *>(GetProcAddress(g_steam, "SteamAPI_RegisterCallback")));
			if (reg == nullptr)
				return;
			g_cb.vtable = g_vtable;
			g_cb.flags = 0;
			g_cb.id = GAME_OVERLAY_ACTIVATED;
			reg(&g_cb, GAME_OVERLAY_ACTIVATED);
			g_registered = true;
		}

		bool key_down(int vk)
		{
			return (GetAsyncKeyState(vk) & 0x8000) != 0;
		}
	}

	void update(HWND game_window, bool use_key)
	{
		try_register();
		// The key, only while Steam has not told us anything this session.
		if (!use_key || g_notices.load() > 0)
		{
			g_key_open.store(false);
			return;
		}
		const HWND front = GetForegroundWindow();
		const bool focused = game_window != nullptr && front != nullptr && (front == game_window || GetAncestor(front, GA_ROOT) == GetAncestor(game_window, GA_ROOT));
		const bool shift_tab = key_down(VK_SHIFT) && key_down(VK_TAB);
		const bool esc = key_down(VK_ESCAPE);
		if (shift_tab && !g_shift_tab_down && (focused || g_key_open.load()))
			g_key_open.store(!g_key_open.load());
		else if (esc && !g_esc_down && g_key_open.load())
			g_key_open.store(false);
		g_shift_tab_down = shift_tab;
		g_esc_down = esc;
	}

	bool active()
	{
		return g_steam_open.load() || g_key_open.load();
	}

	void status(char *buf, size_t size)
	{
		const uint32_t n = g_notices.load();
		if (n > 0)
			std::snprintf(buf, size, "Steam overlay: %s (Steam's notice, %u so far)", g_steam_open.load() ? "OPEN - frame generation hidden" : "closed", n);
		else if (g_registered)
			std::snprintf(buf, size, "Steam overlay: %s (no notice from Steam yet: Shift+Tab is watched)", g_key_open.load() ? "OPEN (by Shift+Tab) - frame generation hidden" : "closed");
		else if (g_no_library)
			std::snprintf(buf, size, "Steam overlay: Steam's library not loaded (not started from Steam?): Shift+Tab is watched%s", g_key_open.load() ? " - OPEN" : "");
		else
			std::snprintf(buf, size, "Steam overlay: could not register with Steam: Shift+Tab is watched%s", g_key_open.load() ? " - OPEN" : "");
	}

	void shutdown()
	{
		if (!g_registered)
			return;
		g_registered = false;
		// only if Steam's library is still there (at process exit it may be gone already)
		if (GetModuleHandleW(L"steam_api64.dll") != g_steam || g_steam == nullptr)
			return;
		const auto unreg = reinterpret_cast<void(__cdecl *)(void *)>(reinterpret_cast<void *>(GetProcAddress(g_steam, "SteamAPI_UnregisterCallback")));
		if (unreg != nullptr)
			unreg(&g_cb);
	}
}
