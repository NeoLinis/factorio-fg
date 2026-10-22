#include "stutter.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <vector>

namespace stutter
{
	namespace
	{
		constexpr int WINDOW = 120;     // frames for the usual frame time (median)
		constexpr double RECENT_S = 60.0;

		log_fn g_log = nullptr;
		std::wstring g_file;
		std::string g_pending;
		double g_intervals[WINDOW] = {};
		int g_pos = 0, g_len = 0;
		uint64_t g_frames = 0;
		uint32_t g_total = 0;
		double g_median = 0;
		struct spike { double at_s, ms, addon_ms; };
		std::deque<spike> g_recent;
		spike g_last = { -1, 0, 0 };
		float g_addon_now = 0, g_addon_worst = 0; // add-on time per frame: last frame, worst in the last ~60 s
		double g_worst_at = 0;
		double g_last_flush = 0;
		uint32_t g_logged = 0;

		double now_s()
		{
			static LARGE_INTEGER f = {};
			if (f.QuadPart == 0)
				QueryPerformanceFrequency(&f);
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
		}

		double median()
		{
			std::vector<double> v(g_intervals, g_intervals + g_len);
			std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
			return v[v.size() / 2];
		}

		void write_pending()
		{
			if (g_pending.empty() || g_file.empty())
				return;
			if (FILE *f = _wfopen(g_file.c_str(), L"ab"))
			{
				std::fwrite(g_pending.data(), 1, g_pending.size(), f);
				std::fclose(f);
			}
			g_pending.clear();
		}
	}

	void set_output(const std::wstring &dir, log_fn log)
	{
		g_log = log;
		if (!g_file.empty() || dir.empty())
			return;
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::filesystem::path p = std::filesystem::path(dir) / L"stutters.txt";
		const std::filesystem::path prev = std::filesystem::path(dir) / L"stutters_previous.txt";
		if (std::filesystem::exists(p, ec))
		{
			std::filesystem::remove(prev, ec);
			std::filesystem::rename(p, prev, ec);
		}
		g_file = p.wstring();
		SYSTEMTIME st;
		GetLocalTime(&st);
		char head[160];
		std::snprintf(head, sizeof(head), "Factorio FG stutters, session started %04u-%02u-%02u %02u:%02u:%02u\r\n", st.wYear, st.wMonth, st.wDay,
			st.wHour, st.wMinute, st.wSecond);
		g_pending += head;
	}

	void frame(double interval_ms, const frame_info &f)
	{
		const double t = now_s();
		const float addon = f.present_ms + f.mask_draw_ms;
		g_addon_now = addon;
		if (addon > g_addon_worst || t - g_worst_at > RECENT_S)
		{
			g_addon_worst = addon;
			g_worst_at = t;
		}
		++g_frames;
		// loading / no rendering (under 5 fps) is not a stutter, and does not count as usual either
		const bool usable = interval_ms > 0.0 && interval_ms < 5000.0 && f.base_fps >= 5.0f;
		if (usable && g_len >= WINDOW / 2)
		{
			if ((g_frames & 15) == 0 || g_median <= 0)
				g_median = median();
			const double usual = g_median;
			if (interval_ms > 2.0 * usual && interval_ms > usual + 10.0)
			{
				++g_total;
				g_last = { t, interval_ms, addon };
				g_recent.push_back(g_last);
				const double extra = interval_ms - usual;
				const char *who = addon >= 0.5 * extra ? "MOSTLY THE ADD-ON" : (addon >= 0.15 * extra ? "partly the add-on" : "outside the add-on (Factorio, driver, system)");
				char line[800];
				std::snprintf(line, sizeof(line),
					"stutter: %.1f ms frame (usual %.1f) - %s: add-on %.2f ms [mask while drawing %.2f (%u draws, %u own maps, %.0f KB%s), at the last present %.2f: mask %.2f, dump %.2f, bridge %.2f (cut wait %.2f, GPU queue wait %.2f, Present %.2f)] | Factorio %.0f fps, %dx%s%s%s%s",
					interval_ms, usual, who, addon, f.mask_draw_ms, f.mask_draws, f.mask_own_maps, f.mask_vertex_kb, f.mask_copy_all ? ", copying at every draw" : "",
					f.present_ms, f.mask_end_ms, f.mask_dump_ms, f.bridge_ms, f.cut_wait_ms, f.alloc_wait_ms, f.present_call_ms, f.base_fps, f.multiplier,
					f.scene_cut ? " | scene cut" : "", f.multiplier_changed ? " | multiplier changed" : "", f.mask_dumped ? " | mask dump written" : "",
					f.mask_valid ? "" : " | no character mask (map view / menu)");
				if (g_log != nullptr && g_logged < 500)
				{
					++g_logged;
					g_log(line);
				}
				SYSTEMTIME st;
				GetLocalTime(&st);
				char stamp[32];
				std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u ", st.wHour, st.wMinute, st.wSecond);
				if (g_pending.size() < (1u << 20))
				{
					g_pending += stamp;
					g_pending += line;
					g_pending += "\r\n";
				}
			}
		}
		while (!g_recent.empty() && t - g_recent.front().at_s > RECENT_S)
			g_recent.pop_front();
		if (usable)
		{
			g_intervals[g_pos] = interval_ms;
			g_pos = (g_pos + 1) % WINDOW;
			g_len = std::min(g_len + 1, WINDOW);
		}
		// written between stutters, about once a second (not in the frame that stuttered)
		if (!g_pending.empty() && t - g_last_flush > 1.0 && t - g_last.at_s > 0.5)
		{
			g_last_flush = t;
			write_pending();
		}
	}

	void readout(char *buf, size_t size)
	{
		const double t = now_s();
		char last[96] = "";
		if (g_last.at_s >= 0)
			std::snprintf(last, sizeof(last), " | last %.0f s ago: %.0f ms, add-on %.1f ms", t - g_last.at_s, g_last.ms, g_last.addon_ms);
		std::snprintf(buf, size, "Stutters: %zu in the last 60 s, %u total%s | add-on on Factorio's thread %.2f ms/frame (worst 60 s %.2f) | usual frame %.1f ms",
			g_recent.size(), g_total, last, g_addon_now, g_addon_worst, g_median);
	}

	void flush()
	{
		write_pending();
	}
}
