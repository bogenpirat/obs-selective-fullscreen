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

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct TrackedWindow {
	HWND hwnd = nullptr;
	RECT window_rect{};        /* GetWindowRect; includes invisible resize borders */
	RECT visible_rect{};       /* DWMWA_EXTENDED_FRAME_BOUNDS; what the user sees */
	bool drawable = false;     /* not minimized/cloaked and intersects the target monitor */
	bool title_matched = true; /* window title satisfies at least one matching rule */
};

/* One entry of the user's application list: an executable name or full path,
 * optionally restricted to windows whose title contains a substring. */
struct FilterRule {
	std::wstring exe;   /* lower-case exe name or full path */
	std::wstring title; /* lower-case title substring; empty matches any title */
};

struct MonitorInfo {
	std::wstring id;   /* stable device id used in settings */
	std::wstring name; /* human-readable label for the properties UI */
	RECT rect{};       /* desktop coordinates, physical pixels */
	bool primary = false;
};

class WindowTracker {
public:
	void set_filter(std::vector<FilterRule> rules, bool match_full_path);

	/* Matching top-level windows in bottom-to-top z-order. Windows that are
	 * minimized/cloaked or off-monitor are included (so captures survive)
	 * but flagged as not drawable. Windows whose exe matches but whose title
	 * does not are included with title_matched=false so the caller can keep
	 * their captures warm across transient title changes. */
	std::vector<TrackedWindow> scan(const RECT &monitor_rect);

private:
	struct PidCacheEntry {
		std::wstring path;     /* full lower-case image path */
		std::wstring basename; /* lower-case file name */
		uint64_t expires_ms = 0;
	};

	const PidCacheEntry *executable_for_pid(DWORD pid);
	bool matches(const PidCacheEntry &entry, HWND hwnd, bool &title_matched) const;

	std::vector<FilterRule> rules_;
	bool match_full_path_ = false;
	std::unordered_map<DWORD, PidCacheEntry> pid_cache_;
};

std::vector<MonitorInfo> enumerate_monitors();

/* Resolve a monitor by its stable id; empty id falls back to the primary
 * monitor. Returns false if no monitor matches. */
bool find_monitor_by_id(const std::wstring &id, MonitorInfo &out);

/* Lower-case executable base names of processes that currently own visible
 * top-level windows; feeds the "add running application" combo box. */
std::vector<std::wstring> enumerate_window_executables();
