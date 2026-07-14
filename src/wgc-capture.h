/*
obs-fullscreen-windowselection
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

#include <graphics/graphics.h>

#include <cstdint>
#include <memory>

/* One Windows.Graphics.Capture session for a single top-level window,
 * mirrored into a gs_texture_t on OBS's D3D11 device. */
class WgcWindowCapture {
public:
	/* Fails (valid() == false) when WGC refuses the window, e.g. windows
	 * of elevated processes while OBS is not elevated. */
	WgcWindowCapture(HWND hwnd, bool capture_cursor);
	~WgcWindowCapture();

	WgcWindowCapture(const WgcWindowCapture &) = delete;
	WgcWindowCapture &operator=(const WgcWindowCapture &) = delete;

	bool valid() const;
	bool closed() const; /* the capture item reported the window as gone */
	void set_cursor_enabled(bool enabled);

	HWND hwnd() const { return hwnd_; }

	/* Latest frame; only valid inside the OBS graphics context. Null until
	 * the first frame arrived. */
	gs_texture_t *texture() const;

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;
	HWND hwnd_;
};
