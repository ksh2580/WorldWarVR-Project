// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xr_types.h"

#include <cstdint>
#include <memory>

struct IDirect3DDevice9;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace wawvr::xr
{

struct D3D9ExBridgeConfig
{
    // Three slots allow D3D9 production, D3D11 sampling, and retirement to
    // overlap without forcing a CPU/GPU synchronization point.
    std::uint32_t ring_size = 3;
};

struct SharedFrameToken
{
    D3D11SourceFrame source = {};
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
};

// Single-render-thread bridge. Capture must be called before the game's
// Present. Acquire/Release bracket every D3D11 use of the returned texture.
// The game device must really be IDirect3DDevice9Ex; this class deliberately
// has no synchronous system-memory fallback.
class D3D9ExSharedTextureBridge final
{
public:
    D3D9ExSharedTextureBridge();
    ~D3D9ExSharedTextureBridge();

    D3D9ExSharedTextureBridge(const D3D9ExSharedTextureBridge&) = delete;
    D3D9ExSharedTextureBridge& operator=(
        const D3D9ExSharedTextureBridge&) = delete;

    bool Initialize(
        const D3D9ExBridgeConfig& config,
        const HostCallbacks& host);
    void Shutdown();

    bool CaptureBackBuffer(
        IDirect3DDevice9* device,
        const FrameState* rendered_frame = nullptr);
    bool AcquireLatest(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        SharedFrameToken* frame);
    void Release(
        ID3D11DeviceContext* context,
        const SharedFrameToken& frame);

    // Call before/after a D3D9 Reset/ResetEx sequence.
    // Returns false while a caller still owns a frame, or when D3D11 work did
    // not retire in time. A Reset/ResetEx hook must defer reset on failure.
    bool Invalidate(ID3D11DeviceContext* context = nullptr);

    bool active() const;
    std::uint64_t latest_serial() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wawvr::xr
