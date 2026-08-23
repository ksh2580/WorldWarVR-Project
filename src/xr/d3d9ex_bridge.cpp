// SPDX-License-Identifier: GPL-3.0-only
// WorldAtWarVR D3D9Ex/D3D11 bridge. Comparative research history and the
// Public API usage follows the pinned OpenXR and Direct3D SDK contracts.

#include "d3d9ex_bridge.h"

#if !defined(_WIN32)
#error WorldAtWarVR's D3D9Ex bridge currently supports Windows only.
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>
#include <vector>

namespace wawvr::xr
{

using Microsoft::WRL::ComPtr;

struct D3D9ExSharedTextureBridge::Impl
{
    enum class SlotState : std::uint8_t
    {
        Free,
        Producing,
        Ready,
        Acquired,
        ConsumerPending,
    };

    struct Slot
    {
        ComPtr<IDirect3DTexture9> producer_texture;
        ComPtr<IDirect3DSurface9> producer_surface;
        ComPtr<IDirect3DQuery9> producer_fence;
        HANDLE shared_handle = nullptr;

        ComPtr<ID3D11Texture2D> consumer_texture;
        ComPtr<ID3D11ShaderResourceView> consumer_view;
        ComPtr<ID3D11Query> consumer_fence;

        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t generation = 0;
        std::uint64_t serial = 0;
        bool rendered_views_valid = false;
        EyeView rendered_eyes[kEyeCount] = {};
        SlotState state = SlotState::Free;
    };

    D3D9ExBridgeConfig config = {};
    HostCallbacks host = {};
    mutable std::mutex mutex;
    std::vector<Slot> slots;
    ComPtr<IDirect3DDevice9> producer_device;
    ComPtr<ID3D11Device> consumer_device;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t generation_counter = 0;
    std::uint64_t serial_counter = 0;
    std::uint64_t last_acquired_serial = 0;
    bool initialized = false;
    bool active = false;
    bool unavailable_until_invalidate = false;
    bool logged_backpressure = false;

    void Log(const LogLevel level, const char* format, ...) const
    {
        if (host.log == nullptr)
        {
            return;
        }
        char message[1024] = {};
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);
        message[sizeof(message) - 1] = '\0';
        host.log(host.user_data, level, message);
    }

    void ReleaseResourcesLocked()
    {
        slots.clear();
        producer_device.Reset();
        consumer_device.Reset();
        width = 0;
        height = 0;
        active = false;
        logged_backpressure = false;
        last_acquired_serial = 0;
    }

    void PollProducerLocked(const bool flush)
    {
        const DWORD flags = flush ? D3DGETDATA_FLUSH : 0;
        for (Slot& slot : slots)
        {
            if (slot.state != SlotState::Producing ||
                slot.producer_fence == nullptr)
            {
                continue;
            }
            const HRESULT hr = slot.producer_fence->GetData(nullptr, 0, flags);
            if (hr == S_OK)
            {
                slot.state = SlotState::Ready;
            }
            else if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "D3D9 producer fence failed: 0x%08lx", hr);
                slot.state = SlotState::Free;
            }
        }
    }

    void PollConsumerLocked(ID3D11DeviceContext* context)
    {
        if (context == nullptr)
        {
            return;
        }
        for (Slot& slot : slots)
        {
            if (slot.state != SlotState::ConsumerPending ||
                slot.consumer_fence == nullptr)
            {
                continue;
            }
            BOOL complete = FALSE;
            const HRESULT hr = context->GetData(
                slot.consumer_fence.Get(),
                &complete,
                sizeof(complete),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK && complete)
            {
                slot.state = SlotState::Free;
            }
            else if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "D3D11 consumer fence failed: 0x%08lx", hr);
                active = false;
            }
        }
    }

    bool CreateSlotsLocked(
        IDirect3DDevice9* device,
        const D3DSURFACE_DESC& back_buffer_description)
    {
        ComPtr<IDirect3DDevice9Ex> device_ex;
        const HRESULT query_result = device->QueryInterface(
            IID_PPV_ARGS(device_ex.GetAddressOf()));
        if (FAILED(query_result) || device_ex == nullptr)
        {
            Log(LogLevel::Warning,
                "Game renderer is not using IDirect3DDevice9Ex; GPU bridge disabled");
            unavailable_until_invalidate = true;
            return false;
        }

        ReleaseResourcesLocked();
        producer_device = device;
        width = back_buffer_description.Width;
        height = back_buffer_description.Height;
        slots.resize(config.ring_size);

        for (Slot& slot : slots)
        {
            slot.generation = ++generation_counter;
            slot.width = width;
            slot.height = height;
            HRESULT hr = device_ex->CreateTexture(
                width,
                height,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                slot.producer_texture.GetAddressOf(),
                &slot.shared_handle);
            if (FAILED(hr) || slot.shared_handle == nullptr)
            {
                Log(LogLevel::Error,
                    "CreateTexture(D3D9Ex shared) failed: 0x%08lx", hr);
                ReleaseResourcesLocked();
                return false;
            }
            hr = slot.producer_texture->GetSurfaceLevel(
                0, slot.producer_surface.GetAddressOf());
            if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "GetSurfaceLevel(D3D9Ex shared) failed: 0x%08lx", hr);
                ReleaseResourcesLocked();
                return false;
            }
            hr = device->CreateQuery(
                D3DQUERYTYPE_EVENT, slot.producer_fence.GetAddressOf());
            if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "CreateQuery(D3D9 producer) failed: 0x%08lx", hr);
                ReleaseResourcesLocked();
                return false;
            }
        }

        active = true;
        Log(LogLevel::Info,
            "D3D9Ex shared bridge created %u fenced %ux%u textures",
            static_cast<unsigned>(slots.size()), width, height);
        return true;
    }

    bool EnsureConsumerResourcesLocked(ID3D11Device* device, Slot& slot)
    {
        if (consumer_device != nullptr && consumer_device.Get() != device)
        {
            Log(LogLevel::Error,
                "D3D11 consumer device changed while the bridge was active");
            active = false;
            return false;
        }
        if (consumer_device == nullptr)
        {
            D3DDEVICE_CREATION_PARAMETERS creation = {};
            ComPtr<IDirect3D9> d3d9;
            ComPtr<IDirect3D9Ex> d3d9_ex;
            LUID d3d9_luid = {};
            ComPtr<IDXGIDevice> dxgi_device;
            ComPtr<IDXGIAdapter> dxgi_adapter;
            DXGI_ADAPTER_DESC dxgi_description = {};
            const bool identified =
                producer_device != nullptr &&
                SUCCEEDED(producer_device->GetCreationParameters(&creation)) &&
                SUCCEEDED(producer_device->GetDirect3D(d3d9.GetAddressOf())) &&
                SUCCEEDED(d3d9.As(&d3d9_ex)) &&
                SUCCEEDED(d3d9_ex->GetAdapterLUID(
                    creation.AdapterOrdinal, &d3d9_luid)) &&
                SUCCEEDED(device->QueryInterface(
                    IID_PPV_ARGS(dxgi_device.GetAddressOf()))) &&
                SUCCEEDED(dxgi_device->GetAdapter(
                    dxgi_adapter.GetAddressOf())) &&
                SUCCEEDED(dxgi_adapter->GetDesc(&dxgi_description));
            if (!identified ||
                d3d9_luid.HighPart != dxgi_description.AdapterLuid.HighPart ||
                d3d9_luid.LowPart != dxgi_description.AdapterLuid.LowPart)
            {
                Log(LogLevel::Error,
                    "D3D9Ex and OpenXR D3D11 devices are on different or unidentified adapters; use CPU capture or recreate D3D9Ex on the OpenXR adapter");
                unavailable_until_invalidate = true;
                active = false;
                return false;
            }
            consumer_device = device;
        }

        if (slot.consumer_texture == nullptr)
        {
            HRESULT hr = device->OpenSharedResource(
                slot.shared_handle,
                IID_PPV_ARGS(slot.consumer_texture.GetAddressOf()));
            if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "OpenSharedResource(D3D9Ex frame) failed: 0x%08lx", hr);
                return false;
            }

            D3D11_TEXTURE2D_DESC description = {};
            slot.consumer_texture->GetDesc(&description);
            if (description.Width != slot.width ||
                description.Height != slot.height ||
                description.MipLevels != 1 ||
                description.ArraySize != 1 ||
                description.SampleDesc.Count != 1 ||
                description.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                Log(LogLevel::Error,
                    "D3D9Ex shared texture has an unexpected D3D11 description");
                slot.consumer_texture.Reset();
                return false;
            }
            hr = device->CreateShaderResourceView(
                slot.consumer_texture.Get(),
                nullptr,
                slot.consumer_view.GetAddressOf());
            if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "CreateShaderResourceView(shared frame) failed: 0x%08lx",
                    hr);
                slot.consumer_texture.Reset();
                return false;
            }
        }

        if (slot.consumer_fence == nullptr)
        {
            D3D11_QUERY_DESC query = {};
            query.Query = D3D11_QUERY_EVENT;
            const HRESULT hr = device->CreateQuery(
                &query, slot.consumer_fence.GetAddressOf());
            if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "CreateQuery(D3D11 consumer) failed: 0x%08lx", hr);
                return false;
            }
        }
        return true;
    }
};

D3D9ExSharedTextureBridge::D3D9ExSharedTextureBridge()
    : impl_(std::make_unique<Impl>())
{
}

D3D9ExSharedTextureBridge::~D3D9ExSharedTextureBridge()
{
    Shutdown();
}

bool D3D9ExSharedTextureBridge::Initialize(
    const D3D9ExBridgeConfig& config,
    const HostCallbacks& host)
{
    Shutdown();
    impl_->config = config;
    impl_->config.ring_size = std::clamp(config.ring_size, 2u, 8u);
    impl_->host = host;
    impl_->initialized = true;
    return true;
}

void D3D9ExSharedTextureBridge::Shutdown()
{
    if (!impl_)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ReleaseResourcesLocked();
    impl_->unavailable_until_invalidate = false;
    impl_->initialized = false;
}

bool D3D9ExSharedTextureBridge::CaptureBackBuffer(
    IDirect3DDevice9* device,
    const FrameState* rendered_frame)
{
    if (device == nullptr || !impl_->initialized)
    {
        return false;
    }

    ComPtr<IDirect3DSurface9> back_buffer;
    HRESULT hr = device->GetBackBuffer(
        0, 0, D3DBACKBUFFER_TYPE_MONO, back_buffer.GetAddressOf());
    if (FAILED(hr))
    {
        impl_->Log(LogLevel::Error, "GetBackBuffer failed: 0x%08lx", hr);
        return false;
    }
    D3DSURFACE_DESC description = {};
    hr = back_buffer->GetDesc(&description);
    if (FAILED(hr) || description.Width < 2 || description.Height == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->unavailable_until_invalidate)
    {
        return false;
    }
    if (!impl_->active ||
        impl_->producer_device.Get() != device ||
        impl_->width != description.Width ||
        impl_->height != description.Height)
    {
        if (!impl_->CreateSlotsLocked(device, description))
        {
            return false;
        }
    }

    impl_->PollProducerLocked(false);
    Impl::Slot* destination = nullptr;
    for (Impl::Slot& slot : impl_->slots)
    {
        if (slot.state == Impl::SlotState::Free)
        {
            destination = &slot;
            break;
        }
    }
    if (destination == nullptr)
    {
        std::uint64_t oldest_serial = std::numeric_limits<std::uint64_t>::max();
        for (Impl::Slot& slot : impl_->slots)
        {
            if (slot.state == Impl::SlotState::Ready &&
                slot.serial < oldest_serial)
            {
                oldest_serial = slot.serial;
                destination = &slot;
            }
        }
    }
    if (destination == nullptr)
    {
        if (!impl_->logged_backpressure)
        {
            impl_->Log(LogLevel::Warning,
                       "D3D9Ex capture dropped a frame while all slots were busy");
            impl_->logged_backpressure = true;
        }
        return false;
    }

    hr = device->StretchRect(
        back_buffer.Get(),
        nullptr,
        destination->producer_surface.Get(),
        nullptr,
        D3DTEXF_NONE);
    if (SUCCEEDED(hr))
    {
        hr = destination->producer_fence->Issue(D3DISSUE_END);
    }
    if (FAILED(hr))
    {
        impl_->Log(LogLevel::Error,
                   "D3D9Ex back-buffer resolve failed: 0x%08lx", hr);
        impl_->active = false;
        return false;
    }

    destination->serial = ++impl_->serial_counter;
    destination->rendered_views_valid =
        rendered_frame != nullptr && rendered_frame->views_valid;
    if (destination->rendered_views_valid)
    {
        for (std::uint32_t eye = 0; eye < kEyeCount; ++eye)
        {
            destination->rendered_eyes[eye] = rendered_frame->eyes[eye];
        }
    }
    destination->state = Impl::SlotState::Producing;
    return true;
}

bool D3D9ExSharedTextureBridge::AcquireLatest(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    SharedFrameToken* frame)
{
    if (device == nullptr || context == nullptr || frame == nullptr ||
        !impl_->initialized)
    {
        return false;
    }
    *frame = {};

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active)
    {
        return false;
    }
    impl_->PollConsumerLocked(context);
    impl_->PollProducerLocked(true);

    Impl::Slot* newest = nullptr;
    for (Impl::Slot& slot : impl_->slots)
    {
        if (slot.state == Impl::SlotState::Ready &&
            slot.serial > impl_->last_acquired_serial &&
            (newest == nullptr || slot.serial > newest->serial))
        {
            newest = &slot;
        }
    }
    if (newest == nullptr)
    {
        return false;
    }

    for (Impl::Slot& slot : impl_->slots)
    {
        if (&slot != newest && slot.state == Impl::SlotState::Ready)
        {
            slot.state = Impl::SlotState::Free;
        }
    }
    if (!impl_->EnsureConsumerResourcesLocked(device, *newest))
    {
        newest->state = Impl::SlotState::Free;
        return false;
    }

    newest->state = Impl::SlotState::Acquired;
    impl_->last_acquired_serial = newest->serial;
    frame->source.texture = newest->consumer_texture.Get();
    frame->source.view = newest->consumer_view.Get();
    frame->source.width = newest->width;
    frame->source.height = newest->height;
    frame->source.serial = newest->serial;
    frame->source.pixels_are_srgb_encoded = true;
    frame->source.rendered_views_valid = newest->rendered_views_valid;
    if (newest->rendered_views_valid)
    {
        for (std::uint32_t eye = 0; eye < kEyeCount; ++eye)
        {
            frame->source.rendered_eyes[eye] = newest->rendered_eyes[eye];
        }
    }
    frame->slot = static_cast<std::uint32_t>(newest - impl_->slots.data());
    frame->generation = newest->generation;
    return true;
}

void D3D9ExSharedTextureBridge::Release(
    ID3D11DeviceContext* context,
    const SharedFrameToken& frame)
{
    if (context == nullptr || frame.slot >= impl_->slots.size())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Slot& slot = impl_->slots[frame.slot];
    if (slot.state != Impl::SlotState::Acquired ||
        slot.generation != frame.generation ||
        slot.serial != frame.source.serial)
    {
        return;
    }

    ID3D11ShaderResourceView* null_view = nullptr;
    context->PSSetShaderResources(0, 1, &null_view);
    context->End(slot.consumer_fence.Get());
    // Flush is necessary because D3D9 and D3D11 have no keyed-mutex contract.
    // It submits the consumer fence without synchronously waiting for it.
    context->Flush();
    slot.state = Impl::SlotState::ConsumerPending;
}

bool D3D9ExSharedTextureBridge::Invalidate(ID3D11DeviceContext* context)
{
    if (!impl_)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const Impl::Slot& slot : impl_->slots)
    {
        if (slot.state == Impl::SlotState::Acquired)
        {
            impl_->Log(LogLevel::Error,
                       "Cannot invalidate D3D9Ex bridge while a frame is acquired");
            return false;
        }
    }

    bool consumer_pending = std::any_of(
        impl_->slots.begin(),
        impl_->slots.end(),
        [](const Impl::Slot& slot)
        {
            return slot.state == Impl::SlotState::ConsumerPending;
        });
    if (consumer_pending && context == nullptr)
    {
        impl_->Log(LogLevel::Error,
                   "D3D11 context is required to retire shared frames before reset");
        return false;
    }
    if (consumer_pending)
    {
        context->Flush();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        do
        {
            impl_->PollConsumerLocked(context);
            consumer_pending = std::any_of(
                impl_->slots.begin(),
                impl_->slots.end(),
                [](const Impl::Slot& slot)
                {
                    return slot.state == Impl::SlotState::ConsumerPending;
                });
            if (!consumer_pending)
            {
                break;
            }
            Sleep(0);
        } while (std::chrono::steady_clock::now() < deadline);

        if (consumer_pending)
        {
            impl_->Log(LogLevel::Error,
                       "Timed out retiring D3D11 shared frames before reset");
            return false;
        }
    }

    impl_->ReleaseResourcesLocked();
    impl_->unavailable_until_invalidate = false;
    return true;
}

bool D3D9ExSharedTextureBridge::active() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->active;
}

std::uint64_t D3D9ExSharedTextureBridge::latest_serial() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->serial_counter;
}

} // namespace wawvr::xr
