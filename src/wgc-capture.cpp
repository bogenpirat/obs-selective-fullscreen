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

#include "wgc-capture.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <mutex>

using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace {

/* Wrap OBS's D3D11 device as a WinRT IDirect3DDevice; also hand out the
 * immediate context used for frame copies. Cached for the process. */
bool get_shared_device(IDirect3DDevice &winrt_device, winrt::com_ptr<ID3D11DeviceContext> &context)
{
	static std::mutex mutex;
	static IDirect3DDevice cached_device{nullptr};
	static winrt::com_ptr<ID3D11DeviceContext> cached_context;
	static bool failed = false;

	std::lock_guard<std::mutex> lock(mutex);
	if (failed)
		return false;
	if (cached_device) {
		winrt_device = cached_device;
		context = cached_context;
		return true;
	}

	obs_enter_graphics();
	auto *d3d11 = static_cast<ID3D11Device *>(gs_get_device_obj());
	obs_leave_graphics();

	if (!d3d11) {
		obs_log(LOG_ERROR, "WGC capture requires the Direct3D 11 renderer");
		failed = true;
		return false;
	}

	try {
		winrt::com_ptr<IDXGIDevice> dxgi;
		winrt::check_hresult(d3d11->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void()));

		winrt::com_ptr<::IInspectable> inspectable;
		winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));

		cached_device = inspectable.as<IDirect3DDevice>();
		d3d11->GetImmediateContext(cached_context.put());
	} catch (const winrt::hresult_error &error) {
		obs_log(LOG_ERROR, "Failed to wrap D3D11 device for WGC: 0x%08X", (unsigned)error.code().value);
		failed = true;
		return false;
	}

	winrt_device = cached_device;
	context = cached_context;
	return true;
}

/* Removing the yellow capture border requires Windows 11 and a one-time
 * (prompt-free for desktop apps) access request. */
bool borderless_capture_granted()
{
	static std::once_flag flag;
	static bool granted = false;

	std::call_once(flag, [] {
		try {
			using namespace winrt::Windows::Foundation::Metadata;
			using namespace winrt::Windows::Security::Authorization::AppCapabilityAccess;
			using winrt::Windows::Graphics::Capture::GraphicsCaptureAccess;
			using winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind;

			if (!ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
							       L"IsBorderRequired"))
				return;

			const auto status =
				GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
			granted = status == AppCapabilityAccessStatus::Allowed;
		} catch (...) {
			granted = false;
		}
	});

	return granted;
}

bool cursor_toggle_supported()
{
	try {
		using namespace winrt::Windows::Foundation::Metadata;
		return ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
							 L"IsCursorCaptureEnabled");
	} catch (...) {
		return false;
	}
}

} // namespace

struct WgcWindowCapture::Impl : std::enable_shared_from_this<WgcWindowCapture::Impl> {
	IDirect3DDevice device{nullptr};
	winrt::com_ptr<ID3D11DeviceContext> context;
	GraphicsCaptureItem item{nullptr};
	Direct3D11CaptureFramePool frame_pool{nullptr};
	GraphicsCaptureSession session{nullptr};
	Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived;
	GraphicsCaptureItem::Closed_revoker item_closed;

	std::atomic<bool> closed{false};
	std::atomic<bool> shutting_down{false};

	/* Written on WGC's frame threads, read on the render thread; both
	 * always inside the OBS graphics context, which serializes access. */
	gs_texture_t *texture = nullptr;
	uint32_t texture_width = 0;
	uint32_t texture_height = 0;

	SizeInt32 pool_size{};

	void on_frame(const Direct3D11CaptureFramePool &pool)
	{
		auto frame = pool.TryGetNextFrame();
		if (!frame)
			return;

		const SizeInt32 content = frame.ContentSize();
		if (content.Width < 1 || content.Height < 1)
			return;

		winrt::com_ptr<ID3D11Texture2D> source;
		auto access =
			frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
		if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), source.put_void())))
			return;

		D3D11_TEXTURE2D_DESC desc;
		source->GetDesc(&desc);
		const uint32_t copy_width = std::min((uint32_t)content.Width, desc.Width);
		const uint32_t copy_height = std::min((uint32_t)content.Height, desc.Height);

		obs_enter_graphics();
		if (!shutting_down) {
			if (!texture || texture_width != copy_width || texture_height != copy_height) {
				gs_texture_destroy(texture);
				texture = gs_texture_create(copy_width, copy_height, GS_BGRA, 1, nullptr, 0);
				texture_width = copy_width;
				texture_height = copy_height;
			}
			if (texture) {
				auto *destination = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(texture));
				D3D11_BOX box{0, 0, 0, copy_width, copy_height, 1};
				context->CopySubresourceRegion(destination, 0, 0, 0, 0, source.get(), 0, &box);
			}
		}
		obs_leave_graphics();

		/* The window was resized: match the pool to the new size so the
		 * next frame is delivered at full resolution. */
		if (content.Width != pool_size.Width || content.Height != pool_size.Height) {
			try {
				pool.Recreate(device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
				pool_size = content;
			} catch (...) {
			}
		}
	}

	~Impl()
	{
		obs_enter_graphics();
		gs_texture_destroy(texture);
		texture = nullptr;
		obs_leave_graphics();
	}
};

WgcWindowCapture::WgcWindowCapture(HWND hwnd, bool capture_cursor) : hwnd_(hwnd)
{
	/* Threads reaching this point need a COM apartment; MTA matches both
	 * OBS's graphics thread and WGC's worker threads. Never uninitialized
	 * on purpose: the apartment must outlive the capture objects. */
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	IDirect3DDevice device{nullptr};
	winrt::com_ptr<ID3D11DeviceContext> context;
	if (!get_shared_device(device, context))
		return;

	try {
		auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

		GraphicsCaptureItem item{nullptr};
		const HRESULT hr =
			interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
		if (FAILED(hr) || !item) {
			obs_log(LOG_DEBUG, "WGC CreateForWindow failed for %p: 0x%08X", hwnd, (unsigned)hr);
			return;
		}

		auto impl = std::make_shared<Impl>();
		impl->device = device;
		impl->context = context;
		impl->item = item;

		impl->pool_size = item.Size();
		impl->pool_size.Width = std::max(impl->pool_size.Width, 1);
		impl->pool_size.Height = std::max(impl->pool_size.Height, 1);

		impl->frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
			device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl->pool_size);

		std::weak_ptr<Impl> weak = impl;
		impl->frame_arrived = impl->frame_pool.FrameArrived(
			winrt::auto_revoke, [weak](const Direct3D11CaptureFramePool &pool, const auto &) {
				if (auto strong = weak.lock())
					strong->on_frame(pool);
			});
		impl->item_closed = item.Closed(winrt::auto_revoke, [weak](const auto &, const auto &) {
			if (auto strong = weak.lock())
				strong->closed = true;
		});

		impl->session = impl->frame_pool.CreateCaptureSession(item);

		if (cursor_toggle_supported()) {
			try {
				impl->session.IsCursorCaptureEnabled(capture_cursor);
			} catch (...) {
			}
		}
		if (borderless_capture_granted()) {
			try {
				impl->session.IsBorderRequired(false);
			} catch (...) {
			}
		}

		impl->session.StartCapture();
		impl_ = std::move(impl);
	} catch (const winrt::hresult_error &error) {
		obs_log(LOG_DEBUG, "WGC capture setup failed for %p: 0x%08X", hwnd, (unsigned)error.code().value);
		impl_.reset();
	} catch (...) {
		obs_log(LOG_DEBUG, "WGC capture setup failed for %p", hwnd);
		impl_.reset();
	}
}

WgcWindowCapture::~WgcWindowCapture()
{
	if (!impl_)
		return;

	impl_->shutting_down = true;
	impl_->frame_arrived.revoke();
	impl_->item_closed.revoke();
	try {
		if (impl_->session)
			impl_->session.Close();
	} catch (...) {
	}
	try {
		if (impl_->frame_pool)
			impl_->frame_pool.Close();
	} catch (...) {
	}
	impl_.reset();
}

bool WgcWindowCapture::valid() const
{
	return impl_ != nullptr;
}

bool WgcWindowCapture::closed() const
{
	return !impl_ || impl_->closed;
}

void WgcWindowCapture::set_cursor_enabled(bool enabled)
{
	if (!impl_ || !impl_->session)
		return;
	try {
		if (cursor_toggle_supported())
			impl_->session.IsCursorCaptureEnabled(enabled);
	} catch (...) {
	}
}

gs_texture_t *WgcWindowCapture::texture() const
{
	return impl_ ? impl_->texture : nullptr;
}
