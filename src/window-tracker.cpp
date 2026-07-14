/*
obs-selective-fullscreen
Copyright (C) 2026 Julian <juliii@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "window-tracker.h"

#include <dwmapi.h>

#include <algorithm>

namespace {

constexpr uint64_t PID_CACHE_TTL_MS = 10000;
constexpr size_t PID_CACHE_MAX = 512;

std::wstring to_lower(std::wstring s)
{
	if (!s.empty())
		CharLowerBuffW(s.data(), (DWORD)s.size());
	return s;
}

std::wstring basename_of(const std::wstring &path)
{
	const size_t pos = path.find_last_of(L"\\/");
	return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool query_process_image(DWORD pid, std::wstring &path)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return false;

	wchar_t buffer[1024];
	DWORD length = (DWORD)(sizeof(buffer) / sizeof(buffer[0]));
	const bool success = QueryFullProcessImageNameW(process, 0, buffer, &length);
	CloseHandle(process);

	if (!success)
		return false;

	path.assign(buffer, length);
	return true;
}

/* Top-level windows that could belong to an application the user selected. */
bool window_is_candidate(HWND hwnd)
{
	if (!IsWindowVisible(hwnd))
		return false;

	const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	if (style & WS_CHILD)
		return false;

	const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (ex_style & WS_EX_TOOLWINDOW)
		return false;

	RECT rect;
	if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
		return false;

	return true;
}

bool window_cloaked(HWND hwnd)
{
	DWORD cloaked = 0;
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
		return cloaked != 0;
	return false;
}

/* EnumWindows visits top-level windows in top-to-bottom z-order. */
BOOL CALLBACK collect_windows(HWND hwnd, LPARAM param)
{
	auto *list = reinterpret_cast<std::vector<HWND> *>(param);
	list->push_back(hwnd);
	return TRUE;
}

} // namespace

void WindowTracker::set_filter(std::vector<std::wstring> exe_names, bool match_full_path)
{
	exe_names_.clear();
	for (auto &name : exe_names) {
		std::wstring lowered = to_lower(std::move(name));
		if (!lowered.empty())
			exe_names_.push_back(std::move(lowered));
	}
	match_full_path_ = match_full_path;
}

const WindowTracker::PidCacheEntry *WindowTracker::executable_for_pid(DWORD pid)
{
	const uint64_t now = GetTickCount64();

	auto it = pid_cache_.find(pid);
	if (it != pid_cache_.end() && it->second.expires_ms > now)
		return &it->second;

	if (pid_cache_.size() > PID_CACHE_MAX)
		pid_cache_.clear();

	PidCacheEntry entry;
	entry.expires_ms = now + PID_CACHE_TTL_MS;

	std::wstring path;
	if (query_process_image(pid, path)) {
		entry.path = to_lower(std::move(path));
		entry.basename = basename_of(entry.path);
	}

	auto result = pid_cache_.insert_or_assign(pid, std::move(entry));
	return &result.first->second;
}

bool WindowTracker::matches(const PidCacheEntry &entry) const
{
	if (entry.path.empty())
		return false;

	for (const auto &name : exe_names_) {
		/* Names containing a path separator always compare against the
		 * full image path so pasted paths work in either mode. */
		if (match_full_path_ || name.find(L'\\') != std::wstring::npos ||
		    name.find(L'/') != std::wstring::npos) {
			if (entry.path == name)
				return true;
		} else if (entry.basename == name) {
			return true;
		}
	}
	return false;
}

std::vector<TrackedWindow> WindowTracker::scan(const RECT &monitor_rect)
{
	std::vector<TrackedWindow> result;
	if (exe_names_.empty())
		return result;

	std::vector<HWND> hwnds;
	hwnds.reserve(128);
	EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&hwnds));

	for (HWND hwnd : hwnds) {
		if (!window_is_candidate(hwnd))
			continue;

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (!pid)
			continue;

		const PidCacheEntry *entry = executable_for_pid(pid);
		if (!entry || !matches(*entry))
			continue;

		TrackedWindow window;
		window.hwnd = hwnd;
		if (!GetWindowRect(hwnd, &window.window_rect))
			continue;

		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &window.visible_rect,
						 sizeof(window.visible_rect))))
			window.visible_rect = window.window_rect;

		RECT intersection;
		window.drawable = !IsIconic(hwnd) && !window_cloaked(hwnd) &&
				  IntersectRect(&intersection, &window.visible_rect, &monitor_rect);

		result.push_back(window);
	}

	/* EnumWindows returned top-to-bottom; rendering wants bottom-to-top. */
	std::reverse(result.begin(), result.end());
	return result;
}

namespace {

BOOL CALLBACK collect_monitors(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
	auto *list = reinterpret_cast<std::vector<MonitorInfo> *>(param);

	MONITORINFOEXW mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(monitor, &mi))
		return TRUE;

	MonitorInfo info;
	info.rect = mi.rcMonitor;
	info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

	std::wstring device_name = mi.szDevice;
	DISPLAY_DEVICEW dd{};
	dd.cb = sizeof(dd);
	if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) {
		info.id = dd.DeviceID;
		device_name = dd.DeviceString;
	} else {
		info.id = mi.szDevice;
	}

	wchar_t label[512];
	swprintf_s(label, L"%s: %ldx%ld @ %ld,%ld%s", device_name.c_str(), mi.rcMonitor.right - mi.rcMonitor.left,
		   mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left, mi.rcMonitor.top,
		   info.primary ? L" (Primary)" : L"");
	info.name = label;

	list->push_back(std::move(info));
	return TRUE;
}

} // namespace

std::vector<MonitorInfo> enumerate_monitors()
{
	std::vector<MonitorInfo> monitors;
	EnumDisplayMonitors(nullptr, nullptr, collect_monitors, reinterpret_cast<LPARAM>(&monitors));
	return monitors;
}

bool find_monitor_by_id(const std::wstring &id, MonitorInfo &out)
{
	const std::vector<MonitorInfo> monitors = enumerate_monitors();
	if (monitors.empty())
		return false;

	if (!id.empty()) {
		for (const auto &monitor : monitors) {
			if (monitor.id == id) {
				out = monitor;
				return true;
			}
		}
		return false;
	}

	for (const auto &monitor : monitors) {
		if (monitor.primary) {
			out = monitor;
			return true;
		}
	}
	out = monitors.front();
	return true;
}

std::vector<std::wstring> enumerate_window_executables()
{
	std::vector<HWND> hwnds;
	hwnds.reserve(128);
	EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&hwnds));

	std::vector<std::wstring> names;
	for (HWND hwnd : hwnds) {
		if (!window_is_candidate(hwnd) || window_cloaked(hwnd))
			continue;

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (!pid)
			continue;

		std::wstring path;
		if (!query_process_image(pid, path))
			continue;

		std::wstring name = basename_of(to_lower(std::move(path)));
		if (name.empty())
			continue;
		if (std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(std::move(name));
	}

	std::sort(names.begin(), names.end());
	return names;
}
