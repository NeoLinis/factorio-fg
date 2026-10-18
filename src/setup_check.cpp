// Setup check. See setup_check.h.

#include "setup_check.h"

#include <winver.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>

namespace
{
	std::vector<setup::item> g_items;
	std::string g_factorio_version;
	uint32_t g_gpu_vendor = 0; // of the GPU the checks looked at (NVIDIA preferred)

	std::wstring folder_of(const std::wstring &path)
	{
		const size_t slash = path.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
	}

	std::wstring module_path(HMODULE module)
	{
		wchar_t buf[MAX_PATH * 2] = {};
		const DWORD n = GetModuleFileNameW(module, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
		return std::wstring(buf, n);
	}

	bool exists(const std::wstring &path)
	{
		return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	bool is_directory(const std::wstring &path)
	{
		const DWORD a = GetFileAttributesW(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	std::string narrow(const std::wstring &s)
	{
		std::string out;
		for (wchar_t c : s)
			out += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
		return out;
	}

	// File version "a.b.c.d" through version.dll, resolved at run time (in Factorio's folder version.dll is the
	// dlssg_for_sm86 proxy, which forwards these).
	std::string file_version(const std::wstring &path)
	{
		HMODULE ver = LoadLibraryW(L"version.dll");
		if (ver == nullptr)
			return {};
		using size_fn = DWORD(WINAPI *)(LPCWSTR, LPDWORD);
		using info_fn = BOOL(WINAPI *)(LPCWSTR, DWORD, DWORD, LPVOID);
		using query_fn = BOOL(WINAPI *)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
		const auto get_size = reinterpret_cast<size_fn>(reinterpret_cast<void *>(GetProcAddress(ver, "GetFileVersionInfoSizeW")));
		const auto get_info = reinterpret_cast<info_fn>(reinterpret_cast<void *>(GetProcAddress(ver, "GetFileVersionInfoW")));
		const auto query = reinterpret_cast<query_fn>(reinterpret_cast<void *>(GetProcAddress(ver, "VerQueryValueW")));
		std::string result;
		if (get_size != nullptr && get_info != nullptr && query != nullptr)
		{
			DWORD handle = 0;
			const DWORD size = get_size(path.c_str(), &handle);
			if (size != 0)
			{
				std::vector<unsigned char> data(size);
				VS_FIXEDFILEINFO *fixed = nullptr;
				UINT len = 0;
				if (get_info(path.c_str(), 0, size, data.data()) && query(data.data(), L"\\", reinterpret_cast<LPVOID *>(&fixed), &len) && fixed != nullptr)
				{
					char buf[64];
					std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
						HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
					result = buf;
				}
			}
		}
		FreeLibrary(ver);
		return result;
	}

	// 1 = HAGS on for the main GPU (NVIDIA preferred), 0 = off, -1 = unknown. Queries D3DKMT_WDDM_2_7_CAPS through gdi32
	// (the structures are declared here; d3dkmthk.h is a driver-kit header).
	int hags_from_kernel(const LUID &luid)
	{
		using D3DKMT_HANDLE = UINT;
		struct open_from_luid { LUID AdapterLuid; D3DKMT_HANDLE hAdapter; };
		struct query_info { D3DKMT_HANDLE hAdapter; UINT Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; };
		struct close_adapter { D3DKMT_HANDLE hAdapter; };
		constexpr UINT KMTQAITYPE_WDDM_2_7_CAPS = 70;
		using open_fn = LONG(WINAPI *)(open_from_luid *);
		using query_fn = LONG(WINAPI *)(const query_info *);
		using close_fn = LONG(WINAPI *)(const close_adapter *);

		HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
		if (gdi == nullptr)
			gdi = LoadLibraryW(L"gdi32.dll");
		if (gdi == nullptr)
			return -1;
		const auto open = reinterpret_cast<open_fn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")));
		const auto query = reinterpret_cast<query_fn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")));
		const auto close = reinterpret_cast<close_fn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTCloseAdapter")));
		if (open == nullptr || query == nullptr || close == nullptr)
			return -1;
		open_from_luid o = {};
		o.AdapterLuid = luid;
		if (open(&o) != 0)
			return -1;
		UINT caps = 0; // bit 0 HwSchSupported, bit 1 HwSchEnabled, bit 2 HwSchEnabledByDefault
		query_info q = {};
		q.hAdapter = o.hAdapter;
		q.Type = KMTQAITYPE_WDDM_2_7_CAPS;
		q.pPrivateDriverData = &caps;
		q.PrivateDriverDataSize = sizeof(caps);
		const LONG status = query(&q);
		close_adapter c = { o.hAdapter };
		close(&c);
		if (status != 0)
			return -1;
		return (caps & 2u) != 0 ? 1 : 0;
	}

	int hags_state(std::string &gpu)
	{
		int result = -1;
		IDXGIFactory1 *factory = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
		{
			// The NVIDIA GPU if there is one, else the first hardware adapter.
			DXGI_ADAPTER_DESC1 chosen = {};
			bool found = false;
			IDXGIAdapter1 *adapter = nullptr;
			for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
			{
				DXGI_ADAPTER_DESC1 d = {};
				if (SUCCEEDED(adapter->GetDesc1(&d)) && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
				{
					if (!found || (d.VendorId == 0x10DE && chosen.VendorId != 0x10DE))
					{
						chosen = d;
						found = true;
					}
				}
				adapter->Release();
			}
			factory->Release();
			if (found)
			{
				g_gpu_vendor = chosen.VendorId;
				gpu = narrow(chosen.Description);
				result = hags_from_kernel(chosen.AdapterLuid);
			}
		}
		if (result < 0)
		{
			// Older Windows: the registry value (2 = on). Missing means "unknown", not "off".
			DWORD mode = 0, size = sizeof(mode);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &mode, &size) == ERROR_SUCCESS)
				result = mode == 2 ? 1 : 0;
		}
		return result;
	}

	void add(const char *name, setup::state result, const std::string &detail)
	{
		setup::item i;
		i.name = name;
		i.result = result;
		i.detail = detail;
		g_items.push_back(i);
	}
}

namespace setup
{
	void run(HMODULE addon)
	{
		g_items.clear();
		const std::wstring game_dir = folder_of(module_path(nullptr));
		const std::wstring addon_dir = folder_of(module_path(addon));

		// Factorio
		g_factorio_version = file_version(module_path(nullptr));
		add("Factorio", state::ok, g_factorio_version.empty() ? "version unknown" : "version " + g_factorio_version);

		// Hardware-accelerated GPU scheduling (DLSS-G refuses to run without it). Asked from the graphics kernel for the
		// GPU itself: the HwSchMode registry value is missing when Windows turns HAGS on by default.
		{
			std::string gpu;
			const int hags = hags_state(gpu);
			if (hags == 1)
				add("GPU scheduling (HAGS)", state::ok, "on" + (gpu.empty() ? std::string() : " (" + gpu + ")"));
			else if (hags == 0)
				add("GPU scheduling (HAGS)", state::problem, "off" + (gpu.empty() ? std::string() : " (" + gpu + ")") +
					": Windows Settings > System > Display > Graphics > Change default graphics settings > Hardware-accelerated GPU scheduling, then reboot");
			else
				add("GPU scheduling (HAGS)", state::warning, "could not be determined; make sure it is on (Windows Settings > System > Display > Graphics)");
		}

		// Frame generation: DLSS-G (Streamline + NVIDIA RTX, dlssg_for_sm86 on RTX 20/30) and/or FSR (AMD's DLLs).
		// One of the two is enough; only when neither is usable is it a problem.
		bool dlssg_ok = false, fsr_ok = false;
		const bool nvidia = g_gpu_vendor == 0x10DE;
		{
			const wchar_t *const files[] = { L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll" };
			std::string missing;
			for (const wchar_t *f : files)
				if (!exists(game_dir + L"fg\\" + f))
					missing += (missing.empty() ? "" : ", ") + narrow(f);
			if (!nvidia)
				add("DLSS-G (Streamline, bin\\x64\\fg\\)", state::ok, "not used: needs an NVIDIA RTX GPU (FSR is used instead)");
			else if (!missing.empty())
				add("DLSS-G (Streamline, bin\\x64\\fg\\)", state::warning, "not installed, missing: " + missing + " (Streamline 2.14.16 release, bin\\x64)");
			else
			{
				const std::string v = file_version(game_dir + L"fg\\sl.interposer.dll");
				const bool expected = v.rfind("2.14.16", 0) == 0;
				add("DLSS-G (Streamline, bin\\x64\\fg\\)", expected ? state::ok : state::warning,
					"all files present, sl.interposer " + (v.empty() ? std::string("version unknown") : v) + (expected ? "" : " (built against 2.14.16)"));
				dlssg_ok = true;
			}
		}

		// dlssg_for_sm86 (only needed for DLSS-G on RTX 20/30)
		if (nvidia && dlssg_ok)
		{
			const bool proxy = exists(game_dir + L"version.dll");
			const bool ini = exists(game_dir + L"dlssg_sm86.ini");
			const bool dir = is_directory(game_dir + L"dlssg_sm86");
			if (proxy && ini && dir)
				add("dlssg_for_sm86", state::ok, "version.dll, dlssg_sm86.ini, dlssg_sm86\\ present");
			else
				add("dlssg_for_sm86", state::warning, std::string("incomplete (") + (proxy ? "" : "version.dll ") + (ini ? "" : "dlssg_sm86.ini ") +
					(dir ? "" : "dlssg_sm86\\ ") + "missing): needed for DLSS-G on RTX 20/30, not on RTX 40/50");
		}

		// AMD FSR Frame Generation (any DirectX 12 GPU)
		{
			const bool loader = exists(game_dir + L"amd_fidelityfx_loader_dx12.dll");
			const bool framegen = exists(game_dir + L"amd_fidelityfx_framegeneration_dx12.dll");
			fsr_ok = loader && framegen;
			if (fsr_ok)
				add("AMD FSR Frame Generation (bin\\x64)", state::ok, "amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_framegeneration_dx12.dll present");
			else
				add("AMD FSR Frame Generation (bin\\x64)", state::warning, std::string("not installed, missing: ") + (loader ? "" : "amd_fidelityfx_loader_dx12.dll ") +
					(framegen ? "" : "amd_fidelityfx_framegeneration_dx12.dll") + " (both come with this add-on's release zip)");
		}
		if (!dlssg_ok && !fsr_ok)
			add("Frame generation", state::problem, nvidia ? "neither DLSS-G nor FSR is installed: install either (see the README)"
				: "AMD FSR Frame Generation is not installed: copy its two DLLs from the release zip into bin\\x64");

		// Shader compiler for the motion vectors
		{
			HMODULE c = LoadLibraryW(L"d3dcompiler_47.dll");
			add("Shader compiler", c != nullptr ? state::ok : state::problem,
				c != nullptr ? "d3dcompiler_47.dll available" : "d3dcompiler_47.dll not found: motion vectors stay zero");
			if (c != nullptr)
				FreeLibrary(c);
		}

		// Two builds of this add-on side by side would both hook the game
		{
			const bool dev = exists(addon_dir + L"hudless_probe.addon64");
			const bool release = exists(addon_dir + L"factorio_fg.addon64");
			if (dev && release)
				add("Add-on files", state::warning, "both hudless_probe.addon64 and factorio_fg.addon64 are present: keep one (only the first loaded runs)");
			else
				add("Add-on files", state::ok, dev ? "hudless_probe.addon64 (developer build)" : "factorio_fg.addon64");
		}
	}

	const std::vector<item> &items()
	{
		return g_items;
	}

	const std::string &factorio_version()
	{
		return g_factorio_version;
	}

	int problems()
	{
		int n = 0;
		for (const item &i : g_items)
			n += i.result == state::problem ? 1 : 0;
		return n;
	}
}
