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

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "wgc-capture.h"
#include "window-tracker.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t MAX_CAPTURES = 32;
constexpr uint64_t CREATE_RETRY_MS = 5000;
constexpr float MONITOR_RECHECK_SECONDS = 1.0f;

std::wstring utf8_to_wide(const char *utf8)
{
	std::wstring result;
	if (!utf8 || !*utf8)
		return result;
	wchar_t *wide = nullptr;
	os_utf8_to_wcs_ptr(utf8, 0, &wide);
	if (wide) {
		result = wide;
		bfree(wide);
	}
	return result;
}

std::string wide_to_utf8(const std::wstring &wide)
{
	std::string result;
	if (wide.empty())
		return result;
	char *utf8 = nullptr;
	os_wcs_to_utf8_ptr(wide.c_str(), 0, &utf8);
	if (utf8) {
		result = utf8;
		bfree(utf8);
	}
	return result;
}

struct SourceSettings {
	std::wstring monitor_id;
	std::vector<std::wstring> exe_names;
	bool match_full_path = false;
	bool cursor = true;
};

struct CompositeSource {
	obs_source_t *source = nullptr;

	std::mutex settings_mutex;
	SourceSettings settings;
	bool settings_changed = true;

	WindowTracker tracker;
	MonitorInfo monitor;
	bool monitor_valid = false;
	float monitor_check_elapsed = MONITOR_RECHECK_SECONDS;
	std::atomic<uint32_t> width{0};
	std::atomic<uint32_t> height{0};

	std::map<HWND, std::unique_ptr<WgcWindowCapture>> captures;
	std::map<HWND, uint64_t> failed_windows; /* hwnd -> earliest retry (ms) */
	std::vector<TrackedWindow> draw_list;    /* bottom-to-top */
};

void pcc_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<CompositeSource *>(data);

	SourceSettings parsed;
	parsed.monitor_id = utf8_to_wide(obs_data_get_string(settings, "monitor"));
	parsed.match_full_path = obs_data_get_bool(settings, "match_full_path");
	parsed.cursor = obs_data_get_bool(settings, "cursor");

	obs_data_array_t *list = obs_data_get_array(settings, "process_list");
	if (list) {
		const size_t count = obs_data_array_count(list);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(list, i);
			std::wstring name = utf8_to_wide(obs_data_get_string(item, "value"));
			obs_data_release(item);
			if (!name.empty())
				parsed.exe_names.push_back(std::move(name));
		}
		obs_data_array_release(list);
	}

	std::lock_guard<std::mutex> lock(s->settings_mutex);
	s->settings = std::move(parsed);
	s->settings_changed = true;
}

void pcc_tick(void *data, float seconds)
{
	auto *s = static_cast<CompositeSource *>(data);

	SourceSettings settings;
	bool changed;
	{
		std::lock_guard<std::mutex> lock(s->settings_mutex);
		settings = s->settings;
		changed = s->settings_changed;
		s->settings_changed = false;
	}

	if (changed) {
		s->tracker.set_filter(settings.exe_names, settings.match_full_path);
		for (auto &entry : s->captures)
			entry.second->set_cursor_enabled(settings.cursor);
	}

	s->monitor_check_elapsed += seconds;
	if (changed || s->monitor_check_elapsed >= MONITOR_RECHECK_SECONDS) {
		s->monitor_check_elapsed = 0.0f;
		s->monitor_valid = find_monitor_by_id(settings.monitor_id, s->monitor);
		if (s->monitor_valid) {
			s->width = (uint32_t)(s->monitor.rect.right - s->monitor.rect.left);
			s->height = (uint32_t)(s->monitor.rect.bottom - s->monitor.rect.top);
		} else {
			s->width = 0;
			s->height = 0;
		}
	}

	if (!s->monitor_valid || settings.exe_names.empty()) {
		s->draw_list.clear();
		s->captures.clear();
		s->failed_windows.clear();
		return;
	}

	std::vector<TrackedWindow> windows = s->tracker.scan(s->monitor.rect);
	const uint64_t now = GetTickCount64();

	/* Drop captures for windows that vanished or whose item closed. */
	for (auto it = s->captures.begin(); it != s->captures.end();) {
		const HWND hwnd = it->first;
		const bool still_matched = std::any_of(windows.begin(), windows.end(),
						       [hwnd](const TrackedWindow &w) { return w.hwnd == hwnd; });
		if (!still_matched || it->second->closed())
			it = s->captures.erase(it);
		else
			++it;
	}
	for (auto it = s->failed_windows.begin(); it != s->failed_windows.end();) {
		const HWND hwnd = it->first;
		const bool still_matched = std::any_of(windows.begin(), windows.end(),
						       [hwnd](const TrackedWindow &w) { return w.hwnd == hwnd; });
		if (!still_matched)
			it = s->failed_windows.erase(it);
		else
			++it;
	}

	/* Start captures for newly matched windows. */
	for (const TrackedWindow &window : windows) {
		if (s->captures.size() >= MAX_CAPTURES)
			break;
		if (s->captures.count(window.hwnd))
			continue;

		auto failed = s->failed_windows.find(window.hwnd);
		if (failed != s->failed_windows.end()) {
			if (failed->second > now)
				continue;
			s->failed_windows.erase(failed);
		}

		auto capture = std::make_unique<WgcWindowCapture>(window.hwnd, settings.cursor);
		if (capture->valid()) {
			s->captures.emplace(window.hwnd, std::move(capture));
		} else {
			obs_log(LOG_INFO, "Could not start WGC capture for window %p (elevated process?)", window.hwnd);
			s->failed_windows[window.hwnd] = now + CREATE_RETRY_MS;
		}
	}

	s->draw_list = std::move(windows);
}

void pcc_render(void *data, gs_effect_t *)
{
	auto *s = static_cast<CompositeSource *>(data);
	if (!s->monitor_valid)
		return;

	const RECT &mon = s->monitor.rect;
	const long mon_width = mon.right - mon.left;
	const long mon_height = mon.bottom - mon.top;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	/* WGC frames carry premultiplied alpha (rounded corners etc.). */
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	for (const TrackedWindow &window : s->draw_list) {
		if (!window.drawable)
			continue;

		auto it = s->captures.find(window.hwnd);
		if (it == s->captures.end())
			continue;
		gs_texture_t *texture = it->second->texture();
		if (!texture)
			continue;

		const RECT &wr = window.window_rect;
		const RECT &vr = window.visible_rect;

		/* Crop the invisible resize borders that GetWindowRect (and the
		 * WGC frame) include but DWM's visible frame does not. */
		const long crop_x = std::max(0L, vr.left - wr.left);
		const long crop_y = std::max(0L, vr.top - wr.top);
		const long visible_width = vr.right - vr.left;
		const long visible_height = vr.bottom - vr.top;

		long dst_x = vr.left - mon.left;
		long dst_y = vr.top - mon.top;

		/* Clip to the monitor canvas. */
		const long clip_left = std::max(0L, -dst_x);
		const long clip_top = std::max(0L, -dst_y);
		const long clip_right = std::max(0L, dst_x + visible_width - mon_width);
		const long clip_bottom = std::max(0L, dst_y + visible_height - mon_height);

		long sub_x = crop_x + clip_left;
		long sub_y = crop_y + clip_top;
		long sub_width = visible_width - clip_left - clip_right;
		long sub_height = visible_height - clip_top - clip_bottom;

		/* The texture may lag one frame behind the window rect during
		 * live resizes; clamp so we never sample outside it. */
		const long texture_width = (long)gs_texture_get_width(texture);
		const long texture_height = (long)gs_texture_get_height(texture);
		if (sub_x >= texture_width || sub_y >= texture_height)
			continue;
		sub_width = std::min(sub_width, texture_width - sub_x);
		sub_height = std::min(sub_height, texture_height - sub_y);
		if (sub_width <= 0 || sub_height <= 0)
			continue;

		gs_effect_set_texture_srgb(image, texture);
		while (gs_effect_loop(effect, "Draw")) {
			gs_matrix_push();
			gs_matrix_translate3f((float)(dst_x + clip_left), (float)(dst_y + clip_top), 0.0f);
			gs_draw_sprite_subregion(texture, 0, (uint32_t)sub_x, (uint32_t)sub_y, (uint32_t)sub_width,
						 (uint32_t)sub_height);
			gs_matrix_pop();
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

const char *pcc_get_name(void *)
{
	return obs_module_text("SourceName");
}

void *pcc_create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new CompositeSource;
	s->source = source;
	pcc_update(s, settings);
	return s;
}

void pcc_destroy(void *data)
{
	delete static_cast<CompositeSource *>(data);
}

uint32_t pcc_get_width(void *data)
{
	return static_cast<CompositeSource *>(data)->width;
}

uint32_t pcc_get_height(void *data)
{
	return static_cast<CompositeSource *>(data)->height;
}

void pcc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "cursor", true);
	obs_data_set_default_bool(settings, "match_full_path", false);
	obs_data_set_default_string(settings, "monitor", "");
}

bool add_process_modified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const char *value = obs_data_get_string(settings, "add_process");
	if (!value || !*value)
		return false;

	obs_data_array_t *list = obs_data_get_array(settings, "process_list");
	if (!list)
		list = obs_data_array_create();

	bool duplicate = false;
	const size_t count = obs_data_array_count(list);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(list, i);
		if (_stricmp(obs_data_get_string(item, "value"), value) == 0)
			duplicate = true;
		obs_data_release(item);
	}

	if (!duplicate) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "value", value);
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}

	obs_data_set_array(settings, "process_list", list);
	obs_data_array_release(list);
	obs_data_set_string(settings, "add_process", "");
	return true;
}

obs_properties_t *pcc_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *monitor_list = obs_properties_add_list(props, "monitor", obs_module_text("Monitor"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const MonitorInfo &monitor : enumerate_monitors()) {
		const std::string name = wide_to_utf8(monitor.name);
		const std::string id = wide_to_utf8(monitor.id);
		obs_property_list_add_string(monitor_list, name.c_str(), id.c_str());
	}

	obs_properties_add_editable_list(props, "process_list", obs_module_text("ProcessList"),
					 OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);

	obs_property_t *add_process = obs_properties_add_list(props, "add_process", obs_module_text("AddProcess"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(add_process, obs_module_text("AddProcess.None"), "");
	for (const std::wstring &exe : enumerate_window_executables()) {
		const std::string name = wide_to_utf8(exe);
		obs_property_list_add_string(add_process, name.c_str(), name.c_str());
	}
	obs_property_set_modified_callback(add_process, add_process_modified);

	obs_properties_add_bool(props, "match_full_path", obs_module_text("MatchFullPath"));
	obs_properties_add_bool(props, "cursor", obs_module_text("CaptureCursor"));

	return props;
}

} // namespace

void register_composite_source()
{
	obs_source_info info = {};
	info.id = "selective_fullscreen";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB;
	info.get_name = pcc_get_name;
	info.create = pcc_create;
	info.destroy = pcc_destroy;
	info.update = pcc_update;
	info.get_defaults = pcc_get_defaults;
	info.get_properties = pcc_get_properties;
	info.video_tick = pcc_tick;
	info.video_render = pcc_render;
	info.get_width = pcc_get_width;
	info.get_height = pcc_get_height;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;
	obs_register_source(&info);
}
