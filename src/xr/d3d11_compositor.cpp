// SPDX-License-Identifier: GPL-3.0-only
// WorldAtWarVR D3D11 compositor. Comparative research history and the public
// API usage follows the pinned OpenXR and Direct3D SDK contracts.

#include "d3d11_compositor.h"
#include "compositor_math.hpp"

#if !defined(_WIN32)
#error WorldAtWarVR's D3D11 compositor currently supports Windows only.
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wawvr::xr
{

using Microsoft::WRL::ComPtr;

namespace
{

constexpr const char* kShaderSource = R"hlsl(
cbuffer CompositorConstants : register(b0)
{
    float2 uv_scale;
    float2 uv_offset;
    float2 reticle_center;
    float2 reticle_target_size;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertex_id : SV_VertexID)
{
    VertexOutput output;
    float2 position;
    if (vertex_id == 0)
        position = float2(-1.0, -1.0);
    else if (vertex_id == 1)
        position = float2(-1.0, 3.0);
    else
        position = float2(3.0, -1.0);

    output.position = float4(position, 0.0, 1.0);
    output.uv = float2(
        0.5 * (position.x + 1.0),
        0.5 * (1.0 - position.y));
    output.uv = output.uv * uv_scale + uv_offset;
    return output;
}

VertexOutput VSReticle(uint vertex_id : SV_VertexID)
{
    VertexOutput output;
    float2 position;
    if (vertex_id == 0)
        position = float2(-1.0, -1.0);
    else if (vertex_id == 1)
        position = float2(-1.0, 3.0);
    else
        position = float2(3.0, -1.0);

    output.position = float4(position, 0.0, 1.0);
    output.uv = float2(
        0.5 * (position.x + 1.0),
        0.5 * (1.0 - position.y));
    return output;
}

Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

float4 PSMain(VertexOutput input) : SV_Target
{
    return source_texture.Sample(source_sampler, input.uv);
}

float4 PSReticle(VertexOutput input) : SV_Target
{
    float2 pixel_delta =
        (input.uv - reticle_center) * reticle_target_size;
    float radius = length(pixel_delta);
    float ring =
        smoothstep(4.5, 5.5, radius) *
        (1.0 - smoothstep(8.0, 9.0, radius));
    float dot = 1.0 - smoothstep(1.0, 2.25, radius);
    float alpha = max(ring, dot) * 0.95;
    return float4(1.0, 0.42, 0.08, alpha);
}
)hlsl";

// Kept separate from the mandatory copy/reticle program so an optional bloom
// compiler or device failure cannot prevent the compositor from initializing.
constexpr const char* kBloomShaderSource = R"hlsl(
cbuffer CompositorConstants : register(b0)
{
    float2 uv_scale;
    float2 uv_offset;
    float2 reticle_center;
    float2 reticle_target_size;
    float2 bloom_uv_min;
    float2 bloom_uv_max;
    float2 bloom_texel_size;
    float2 bloom_padding;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

float3 ExtractBloom(float3 color)
{
    const float peak = max(color.r, max(color.g, color.b));
    const float contribution = saturate((peak - 0.70) / 0.30);
    return color * contribution;
}

float3 BloomTap(float2 uv)
{
    return ExtractBloom(source_texture.Sample(
        source_sampler, clamp(uv, bloom_uv_min, bloom_uv_max)).rgb);
}

float4 PSBloom(VertexOutput input) : SV_Target
{
    const float2 center = clamp(input.uv, bloom_uv_min, bloom_uv_max);
    const float4 base = source_texture.Sample(source_sampler, center);
    const float2 axial = bloom_texel_size * 3.0;
    const float2 diagonal = bloom_texel_size * 2.0;

    float3 glow = 0.0;
    glow += BloomTap(center + float2( axial.x, 0.0)) * 0.15;
    glow += BloomTap(center + float2(-axial.x, 0.0)) * 0.15;
    glow += BloomTap(center + float2(0.0,  axial.y)) * 0.15;
    glow += BloomTap(center + float2(0.0, -axial.y)) * 0.15;
    glow += BloomTap(center + float2( diagonal.x,  diagonal.y)) * 0.10;
    glow += BloomTap(center + float2(-diagonal.x,  diagonal.y)) * 0.10;
    glow += BloomTap(center + float2( diagonal.x, -diagonal.y)) * 0.10;
    glow += BloomTap(center + float2(-diagonal.x, -diagonal.y)) * 0.10;

    return float4(saturate(base.rgb + glow * 0.40), base.a);
}
)hlsl";

struct alignas(16) CompositorConstants
{
    float uv_scale[2] = {};
    float uv_offset[2] = {};
    float reticle_center[2] = {};
    float reticle_target_size[2] = {};
    float bloom_uv_min[2] = {};
    float bloom_uv_max[2] = {};
    float bloom_texel_size[2] = {};
    float bloom_padding[2] = {};
};

static_assert(sizeof(CompositorConstants) == 64);
static_assert(offsetof(CompositorConstants, uv_scale) == 0);
static_assert(offsetof(CompositorConstants, uv_offset) == 8);
static_assert(offsetof(CompositorConstants, reticle_center) == 16);
static_assert(offsetof(CompositorConstants, reticle_target_size) == 24);
static_assert(offsetof(CompositorConstants, bloom_uv_min) == 32);
static_assert(offsetof(CompositorConstants, bloom_uv_max) == 40);
static_assert(offsetof(CompositorConstants, bloom_texel_size) == 48);
static_assert(offsetof(CompositorConstants, bloom_padding) == 56);

bool GetSrgbViewFormats(
    const DXGI_FORMAT source,
    DXGI_FORMAT* typeless,
    DXGI_FORMAT* srgb)
{
    switch (source)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        *typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        *srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        return true;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        *typeless = DXGI_FORMAT_B8G8R8X8_TYPELESS;
        *srgb = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        *typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        *srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        return true;
    default:
        return false;
    }
}

} // namespace

namespace compositor_detail
{

bool BuildEyeLocalBloomSamplingRegion(
    const NormalizedRect& rectangle,
    const std::uint32_t source_width,
    const std::uint32_t source_height,
    EyeLocalBloomSamplingRegion* const region) noexcept
{
    if (region == nullptr || source_width == 0 || source_height == 0 ||
        !std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) ||
        !std::isfinite(rectangle.width) ||
        !std::isfinite(rectangle.height) || rectangle.x < 0.0F ||
        rectangle.y < 0.0F || rectangle.width <= 0.0F ||
        rectangle.height <= 0.0F ||
        rectangle.x + rectangle.width > 1.0F ||
        rectangle.y + rectangle.height > 1.0F)
    {
        return false;
    }

    EyeLocalBloomSamplingRegion candidate = {};
    candidate.texel_u = 1.0F / static_cast<float>(source_width);
    candidate.texel_v = 1.0F / static_cast<float>(source_height);
    candidate.min_u = rectangle.x + 0.5F * candidate.texel_u;
    candidate.min_v = rectangle.y + 0.5F * candidate.texel_v;
    candidate.max_u =
        rectangle.x + rectangle.width - 0.5F * candidate.texel_u;
    candidate.max_v =
        rectangle.y + rectangle.height - 0.5F * candidate.texel_v;
    if (!std::isfinite(candidate.min_u) ||
        !std::isfinite(candidate.min_v) ||
        !std::isfinite(candidate.max_u) ||
        !std::isfinite(candidate.max_v) ||
        !std::isfinite(candidate.texel_u) ||
        !std::isfinite(candidate.texel_v) ||
        candidate.min_u > candidate.max_u ||
        candidate.min_v > candidate.max_v)
    {
        return false;
    }

    *region = candidate;
    return true;
}

bool ShouldUseEyeLocalBloom(
    const CompositorEffects& effects,
    const bool shader_available,
    const bool sampling_region_valid) noexcept
{
    return effects.enable_eye_local_bloom && shader_available &&
           sampling_region_valid;
}

const char* EyeLocalBloomShaderSourceForTesting() noexcept
{
    return kBloomShaderSource;
}

} // namespace compositor_detail

struct D3D11Compositor::Impl
{
    HostCallbacks host = {};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11PixelShader> bloom_pixel_shader;
    ComPtr<ID3D11VertexShader> reticle_vertex_shader;
    ComPtr<ID3D11PixelShader> reticle_pixel_shader;
    ComPtr<ID3D11BlendState> reticle_blend;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;

    ComPtr<ID3D11Texture2D> decoded_texture;
    ComPtr<ID3D11ShaderResourceView> decoded_view;
    std::uint32_t decoded_width = 0;
    std::uint32_t decoded_height = 0;
    DXGI_FORMAT decoded_source_format = DXGI_FORMAT_UNKNOWN;
    bool invalid_bloom_region_logged = false;
    bool initialized = false;

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

    bool CompileShader(
        const char* source,
        const char* source_name,
        const char* entry,
        const char* profile,
        const LogLevel failure_level,
        ComPtr<ID3DBlob>* bytecode)
    {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(
            source,
            std::strlen(source),
            source_name,
            nullptr,
            nullptr,
            entry,
            profile,
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            bytecode->GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(hr))
        {
            const char* details = errors != nullptr
                ? static_cast<const char*>(errors->GetBufferPointer())
                : "no compiler diagnostics";
            Log(failure_level, "D3DCompile(%s) failed: 0x%08lx: %s",
                entry, hr, details);
            return false;
        }
        return true;
    }

    bool CreatePipeline()
    {
        ComPtr<ID3DBlob> vertex_bytecode;
        ComPtr<ID3DBlob> pixel_bytecode;
        ComPtr<ID3DBlob> reticle_vertex_bytecode;
        ComPtr<ID3DBlob> reticle_pixel_bytecode;
        if (!CompileShader(
                kShaderSource, "wawvr_compositor.hlsl", "VSMain",
                "vs_4_0", LogLevel::Error, &vertex_bytecode) ||
            !CompileShader(
                kShaderSource, "wawvr_compositor.hlsl", "PSMain",
                "ps_4_0", LogLevel::Error, &pixel_bytecode) ||
            !CompileShader(
                kShaderSource, "wawvr_compositor.hlsl", "VSReticle",
                "vs_4_0", LogLevel::Error, &reticle_vertex_bytecode) ||
            !CompileShader(
                kShaderSource, "wawvr_compositor.hlsl", "PSReticle",
                "ps_4_0", LogLevel::Error, &reticle_pixel_bytecode))
        {
            return false;
        }

        HRESULT hr = device->CreateVertexShader(
            vertex_bytecode->GetBufferPointer(),
            vertex_bytecode->GetBufferSize(),
            nullptr,
            vertex_shader.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = device->CreatePixelShader(
                pixel_bytecode->GetBufferPointer(),
                pixel_bytecode->GetBufferSize(),
                nullptr,
                pixel_shader.GetAddressOf());
        }
        if (SUCCEEDED(hr))
        {
            hr = device->CreateVertexShader(
                reticle_vertex_bytecode->GetBufferPointer(),
                reticle_vertex_bytecode->GetBufferSize(),
                nullptr,
                reticle_vertex_shader.GetAddressOf());
        }
        if (SUCCEEDED(hr))
        {
            hr = device->CreatePixelShader(
                reticle_pixel_bytecode->GetBufferPointer(),
                reticle_pixel_bytecode->GetBufferSize(),
                nullptr,
                reticle_pixel_shader.GetAddressOf());
        }
        if (FAILED(hr))
        {
            Log(LogLevel::Error, "Create compositor shader failed: 0x%08lx", hr);
            return false;
        }

        ComPtr<ID3DBlob> bloom_pixel_bytecode;
        if (CompileShader(
                kBloomShaderSource, "wawvr_eye_local_bloom.hlsl",
                "PSBloom", "ps_4_0", LogLevel::Warning,
                &bloom_pixel_bytecode))
        {
            const HRESULT bloom_hr = device->CreatePixelShader(
                bloom_pixel_bytecode->GetBufferPointer(),
                bloom_pixel_bytecode->GetBufferSize(), nullptr,
                bloom_pixel_shader.GetAddressOf());
            if (FAILED(bloom_hr))
            {
                bloom_pixel_shader.Reset();
                Log(LogLevel::Warning,
                    "CreatePixelShader(eye-local bloom) failed: 0x%08lx; "
                    "using the compositor copy path",
                    bloom_hr);
            }
        }

        D3D11_BLEND_DESC blend_description = {};
        D3D11_RENDER_TARGET_BLEND_DESC& target_blend =
            blend_description.RenderTarget[0];
        target_blend.BlendEnable = TRUE;
        target_blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOp = D3D11_BLEND_OP_ADD;
        target_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        target_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target_blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(
            &blend_description, reticle_blend.GetAddressOf());
        if (FAILED(hr))
        {
            Log(LogLevel::Error,
                "CreateBlendState(reticle) failed: 0x%08lx", hr);
            return false;
        }

        D3D11_SAMPLER_DESC sampler_description = {};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(
            &sampler_description, sampler.GetAddressOf());
        if (FAILED(hr))
        {
            Log(LogLevel::Error, "CreateSamplerState failed: 0x%08lx", hr);
            return false;
        }

        D3D11_BUFFER_DESC buffer_description = {};
        buffer_description.ByteWidth = sizeof(CompositorConstants);
        buffer_description.Usage = D3D11_USAGE_DYNAMIC;
        buffer_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(
            &buffer_description, nullptr, constants.GetAddressOf());
        if (FAILED(hr))
        {
            Log(LogLevel::Error, "CreateBuffer(compositor) failed: 0x%08lx", hr);
            return false;
        }
        return true;
    }

    ID3D11ShaderResourceView* PrepareSource(const D3D11SourceFrame& source)
    {
        if (!source.pixels_are_srgb_encoded)
        {
            return source.view;
        }

        D3D11_TEXTURE2D_DESC source_description = {};
        source.texture->GetDesc(&source_description);
        DXGI_FORMAT typeless_format = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT srgb_format = DXGI_FORMAT_UNKNOWN;
        if (!GetSrgbViewFormats(
                source_description.Format, &typeless_format, &srgb_format))
        {
            Log(LogLevel::Error,
                "Cannot create an sRGB view for shared DXGI format %u",
                static_cast<unsigned>(source_description.Format));
            return nullptr;
        }

        const bool recreate =
            decoded_texture == nullptr ||
            decoded_width != source_description.Width ||
            decoded_height != source_description.Height ||
            decoded_source_format != source_description.Format;
        if (recreate)
        {
            decoded_view.Reset();
            decoded_texture.Reset();

            D3D11_TEXTURE2D_DESC decoded_description = source_description;
            decoded_description.Format = typeless_format;
            decoded_description.Usage = D3D11_USAGE_DEFAULT;
            decoded_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            decoded_description.CPUAccessFlags = 0;
            decoded_description.MiscFlags = 0;
            decoded_description.SampleDesc.Count = 1;
            decoded_description.SampleDesc.Quality = 0;
            HRESULT hr = device->CreateTexture2D(
                &decoded_description,
                nullptr,
                decoded_texture.GetAddressOf());
            if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "CreateTexture2D(sRGB decode) failed: 0x%08lx", hr);
                return nullptr;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC view_description = {};
            view_description.Format = srgb_format;
            view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view_description.Texture2D.MostDetailedMip = 0;
            view_description.Texture2D.MipLevels = 1;
            hr = device->CreateShaderResourceView(
                decoded_texture.Get(),
                &view_description,
                decoded_view.GetAddressOf());
            if (FAILED(hr))
            {
                Log(LogLevel::Error,
                    "CreateShaderResourceView(sRGB decode) failed: 0x%08lx", hr);
                decoded_texture.Reset();
                return nullptr;
            }
            decoded_width = source_description.Width;
            decoded_height = source_description.Height;
            decoded_source_format = source_description.Format;
        }

        // Preserve the source bits in typeless storage, then interpret them
        // through an sRGB view when the compositor samples the texture.
        context->CopyResource(decoded_texture.Get(), source.texture);
        return decoded_view.Get();
    }

    bool UpdateConstants(
        const NormalizedRect& rectangle,
        const compositor_detail::EyeLocalBloomSamplingRegion*
            bloom_region)
    {
        CompositorConstants values = {};
        values.uv_scale[0] = rectangle.width;
        values.uv_scale[1] = rectangle.height;
        values.uv_offset[0] = rectangle.x;
        values.uv_offset[1] = rectangle.y;
        if (bloom_region != nullptr)
        {
            values.bloom_uv_min[0] = bloom_region->min_u;
            values.bloom_uv_min[1] = bloom_region->min_v;
            values.bloom_uv_max[0] = bloom_region->max_u;
            values.bloom_uv_max[1] = bloom_region->max_v;
            values.bloom_texel_size[0] = bloom_region->texel_u;
            values.bloom_texel_size[1] = bloom_region->texel_v;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map(
            constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || mapped.pData == nullptr)
        {
            Log(LogLevel::Error,
                "Map(compositor constants) failed: 0x%08lx", hr);
            return false;
        }
        std::memcpy(mapped.pData, &values, sizeof(values));
        context->Unmap(constants.Get(), 0);
        return true;
    }

    bool UpdateReticleConstants(
        const CompositorReticle& reticle,
        const std::uint32_t target_width,
        const std::uint32_t target_height)
    {
        CompositorConstants values = {};
        values.reticle_center[0] = reticle.u;
        values.reticle_center[1] = reticle.v;
        values.reticle_target_size[0] = static_cast<float>(target_width);
        values.reticle_target_size[1] = static_cast<float>(target_height);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map(
            constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || mapped.pData == nullptr)
        {
            Log(LogLevel::Error,
                "Map(reticle constants) failed: 0x%08lx", hr);
            return false;
        }
        std::memcpy(mapped.pData, &values, sizeof(values));
        context->Unmap(constants.Get(), 0);
        return true;
    }
};

D3D11Compositor::D3D11Compositor() : impl_(std::make_unique<Impl>()) {}

D3D11Compositor::~D3D11Compositor()
{
    Shutdown();
}

bool D3D11Compositor::Initialize(
    OpenXrRuntime& runtime,
    const HostCallbacks& host)
{
    Shutdown();
    if (!runtime.initialized() || runtime.d3d11_device() == nullptr ||
        runtime.d3d11_context() == nullptr)
    {
        return false;
    }
    impl_->host = host;
    impl_->device = runtime.d3d11_device();
    impl_->context = runtime.d3d11_context();
    if (!impl_->CreatePipeline())
    {
        Shutdown();
        return false;
    }
    impl_->initialized = true;
    return true;
}

void D3D11Compositor::Shutdown()
{
    if (!impl_)
    {
        return;
    }
    impl_->decoded_view.Reset();
    impl_->decoded_texture.Reset();
    impl_->constants.Reset();
    impl_->reticle_blend.Reset();
    impl_->reticle_pixel_shader.Reset();
    impl_->reticle_vertex_shader.Reset();
    impl_->sampler.Reset();
    impl_->bloom_pixel_shader.Reset();
    impl_->pixel_shader.Reset();
    impl_->vertex_shader.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
    impl_->decoded_width = 0;
    impl_->decoded_height = 0;
    impl_->decoded_source_format = DXGI_FORMAT_UNKNOWN;
    impl_->invalid_bloom_region_logged = false;
    impl_->initialized = false;
}

bool D3D11Compositor::RenderStereo(
    OpenXrRuntime& runtime,
    const FrameState& frame,
    const D3D11SourceFrame& source,
    const StereoSourceLayout& layout,
    const CompositorReticle* const reticle,
    std::uint32_t* const released_eye_mask,
    const CompositorEffects effects)
{
    if (released_eye_mask != nullptr)
    {
        *released_eye_mask = 0;
    }
    if (!impl_->initialized || !frame.should_render || !frame.views_valid ||
        source.texture == nullptr || source.view == nullptr ||
        source.width < 2 || source.height == 0)
    {
        return false;
    }
    if (runtime.d3d11_device() != impl_->device.Get() ||
        runtime.d3d11_context() != impl_->context.Get())
    {
        impl_->Log(LogLevel::Error,
                   "Compositor was used with a different OpenXR D3D11 device");
        return false;
    }

    ID3D11ShaderResourceView* source_view = impl_->PrepareSource(source);
    if (source_view == nullptr)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC source_description = {};
    source.texture->GetDesc(&source_description);

    for (std::uint32_t eye = 0; eye < kEyeCount; ++eye)
    {
        const NormalizedRect& rectangle = layout.eyes[eye];
        const NormalizedViewport& destination = layout.destinations[eye];
        if (rectangle.width <= 0.0f || rectangle.height <= 0.0f ||
            rectangle.x < 0.0f || rectangle.y < 0.0f ||
            rectangle.x + rectangle.width > 1.0f ||
            rectangle.y + rectangle.height > 1.0f)
        {
            impl_->Log(LogLevel::Error, "Invalid normalized eye source rectangle");
            return false;
        }
        if (!std::isfinite(destination.x) ||
            !std::isfinite(destination.y) ||
            !std::isfinite(destination.width) ||
            !std::isfinite(destination.height) ||
            destination.width <= 0.0f || destination.height <= 0.0f ||
            destination.width > 8.0f || destination.height > 8.0f ||
            destination.x < -8.0f || destination.x > 8.0f ||
            destination.y < -8.0f || destination.y > 8.0f)
        {
            impl_->Log(LogLevel::Error,
                       "Invalid normalized OpenXR destination viewport");
            return false;
        }

        compositor_detail::EyeLocalBloomSamplingRegion bloom_region = {};
        const bool bloom_region_valid =
            effects.enable_eye_local_bloom &&
            compositor_detail::BuildEyeLocalBloomSamplingRegion(
                rectangle, source_description.Width,
                source_description.Height, &bloom_region);
        const bool use_eye_local_bloom =
            compositor_detail::ShouldUseEyeLocalBloom(
                effects, impl_->bloom_pixel_shader != nullptr,
                bloom_region_valid);
        if (effects.enable_eye_local_bloom && !bloom_region_valid &&
            !impl_->invalid_bloom_region_logged)
        {
            impl_->invalid_bloom_region_logged = true;
            impl_->Log(LogLevel::Warning,
                "Eye-local bloom source bounds were invalid; using the "
                "compositor copy path");
        }

        EyeRenderTarget target = {};
        if (!runtime.AcquireEyeImage(eye, &target))
        {
            return false;
        }

        NormalizedViewport effective_destination = destination;
        bool destination_ready = true;
        if (layout.preserve_source_aspect)
        {
            const EyeView& submitted_eye = source.rendered_views_valid
                ? source.rendered_eyes[eye]
                : frame.eyes[eye];
            destination_ready = fit_source_aspect_viewport(
                rectangle, source.width, source.height,
                layout.source_aspect_multipliers[eye], destination,
                submitted_eye.fov,
                &effective_destination);
            if (!destination_ready)
            {
                impl_->Log(LogLevel::Error,
                           "Could not aspect-fit mono source rectangle");
            }
        }

        bool eye_succeeded = destination_ready && impl_->UpdateConstants(
            rectangle, use_eye_local_bloom ? &bloom_region : nullptr);
        if (eye_succeeded)
        {
            D3D11_VIEWPORT viewport = {};
            viewport.TopLeftX =
                effective_destination.x * static_cast<float>(target.width);
            viewport.TopLeftY =
                effective_destination.y * static_cast<float>(target.height);
            viewport.Width =
                effective_destination.width * static_cast<float>(target.width);
            viewport.Height =
                effective_destination.height * static_cast<float>(target.height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            impl_->context->RSSetViewports(1, &viewport);

            const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            impl_->context->ClearRenderTargetView(
                target.render_target, clear_color);
            impl_->context->OMSetRenderTargets(
                1, &target.render_target, nullptr);
            impl_->context->IASetInputLayout(nullptr);
            impl_->context->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->context->VSSetShader(
                impl_->vertex_shader.Get(), nullptr, 0);
            impl_->context->PSSetShader(
                use_eye_local_bloom
                    ? impl_->bloom_pixel_shader.Get()
                    : impl_->pixel_shader.Get(),
                nullptr, 0);
            ID3D11Buffer* constant_buffer = impl_->constants.Get();
            impl_->context->VSSetConstantBuffers(0, 1, &constant_buffer);
            impl_->context->PSSetConstantBuffers(0, 1, &constant_buffer);
            ID3D11SamplerState* sampler = impl_->sampler.Get();
            impl_->context->PSSetSamplers(0, 1, &sampler);
            impl_->context->PSSetShaderResources(0, 1, &source_view);
            impl_->context->Draw(3, 0);

            const bool draw_reticle =
                reticle != nullptr && reticle->visible &&
                reticle->target_eye == eye &&
                std::isfinite(reticle->u) && std::isfinite(reticle->v) &&
                reticle->u >= 0.0F && reticle->u <= 1.0F &&
                reticle->v >= 0.0F && reticle->v <= 1.0F;
            if (draw_reticle)
            {
                eye_succeeded = impl_->UpdateReticleConstants(
                    *reticle, target.width, target.height);
                if (eye_succeeded)
                {
                    D3D11_VIEWPORT reticle_viewport = {};
                    reticle_viewport.Width = static_cast<float>(target.width);
                    reticle_viewport.Height = static_cast<float>(target.height);
                    reticle_viewport.MinDepth = 0.0F;
                    reticle_viewport.MaxDepth = 1.0F;
                    impl_->context->RSSetViewports(1, &reticle_viewport);
                    impl_->context->VSSetShader(
                        impl_->reticle_vertex_shader.Get(), nullptr, 0);
                    impl_->context->PSSetShader(
                        impl_->reticle_pixel_shader.Get(), nullptr, 0);
                    ID3D11BlendState* blend = impl_->reticle_blend.Get();
                    constexpr float blend_factor[4] = {};
                    impl_->context->OMSetBlendState(
                        blend, blend_factor, 0xffffffffU);
                    impl_->context->Draw(3, 0);
                    impl_->context->OMSetBlendState(
                        nullptr, blend_factor, 0xffffffffU);
                }
            }

            ID3D11ShaderResourceView* null_view = nullptr;
            impl_->context->PSSetShaderResources(0, 1, &null_view);
            ID3D11RenderTargetView* null_target = nullptr;
            impl_->context->OMSetRenderTargets(1, &null_target, nullptr);

            // xrReleaseSwapchainImage transfers the image back to the
            // runtime. Submit the D3D11 draw first; otherwise the runtime can
            // consume an image whose commands are still only queued in the
            // immediate context. Flush at this transfer boundary as well.
            impl_->context->Flush();
        }

        const bool released = runtime.ReleaseEyeImage(eye);
        if (released && released_eye_mask != nullptr)
        {
            *released_eye_mask |= (1u << eye);
        }
        if (!eye_succeeded || !released)
        {
            return false;
        }
    }
    return true;
}

} // namespace wawvr::xr
