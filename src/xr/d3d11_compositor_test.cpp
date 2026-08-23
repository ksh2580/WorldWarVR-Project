// SPDX-License-Identifier: GPL-3.0-only

#include "d3d11_compositor.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace
{

int failures = 0;

void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool Near(const float left, const float right)
{
    return std::abs(left - right) < 0.000001F;
}

void TestPackedEyesHaveDisjointBilinearBounds()
{
    using wawvr::xr::compositor_detail::BuildEyeLocalBloomSamplingRegion;
    using wawvr::xr::compositor_detail::EyeLocalBloomSamplingRegion;

    EyeLocalBloomSamplingRegion left = {};
    EyeLocalBloomSamplingRegion right = {};
    Expect(BuildEyeLocalBloomSamplingRegion(
               {0.0F, 0.0F, 0.5F, 1.0F}, 2560, 1440, &left),
           "left packed-eye bloom bounds build");
    Expect(BuildEyeLocalBloomSamplingRegion(
               {0.5F, 0.0F, 0.5F, 1.0F}, 2560, 1440, &right),
           "right packed-eye bloom bounds build");

    const float half_texel_u = 0.5F / 2560.0F;
    const float half_texel_v = 0.5F / 1440.0F;
    Expect(Near(left.min_u, half_texel_u) &&
               Near(left.max_u, 0.5F - half_texel_u),
           "left taps remain half a texel inside the left crop");
    Expect(Near(right.min_u, 0.5F + half_texel_u) &&
               Near(right.max_u, 1.0F - half_texel_u),
           "right taps remain half a texel inside the right crop");
    Expect(left.max_u < 0.5F && right.min_u > 0.5F &&
               left.max_u < right.min_u,
           "bilinear footprints cannot cross the packed-eye seam");
    Expect(Near(left.min_v, half_texel_v) &&
               Near(left.max_v, 1.0F - half_texel_v) &&
               Near(right.min_v, left.min_v) &&
               Near(right.max_v, left.max_v),
           "vertical taps remain inside both complete-height crops");
}

void TestInvalidSamplingRegionsFailClosed()
{
    using wawvr::xr::compositor_detail::BuildEyeLocalBloomSamplingRegion;
    using wawvr::xr::compositor_detail::EyeLocalBloomSamplingRegion;

    EyeLocalBloomSamplingRegion region = {};
    Expect(!BuildEyeLocalBloomSamplingRegion(
               {0.0F, 0.0F, 0.5F, 1.0F}, 0, 1440, &region),
           "zero-width source rejects bloom");
    Expect(!BuildEyeLocalBloomSamplingRegion(
               {0.0F, 0.0F, 0.5F, 1.0F}, 2560, 1440, nullptr),
           "null bloom-region output fails closed");
    Expect(!BuildEyeLocalBloomSamplingRegion(
               {0.75F, 0.0F, 0.5F, 1.0F}, 2560, 1440, &region),
           "out-of-texture crop rejects bloom");
    Expect(!BuildEyeLocalBloomSamplingRegion(
               {0.0F, 0.0F, 0.25F / 2560.0F, 1.0F}, 2560, 1440,
               &region),
           "sub-texel crop rejects bloom");
    Expect(!BuildEyeLocalBloomSamplingRegion(
               {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.5F, 1.0F},
               2560, 1440, &region),
           "non-finite crop rejects bloom");
}

void TestBloomRequiresExplicitCompleteOptIn()
{
    using wawvr::xr::CompositorEffects;
    using wawvr::xr::compositor_detail::ShouldUseEyeLocalBloom;

    const CompositorEffects default_effects = {};
    Expect(!ShouldUseEyeLocalBloom(default_effects, true, true),
           "default compositor behavior remains the exact copy path");

    CompositorEffects enabled = {};
    enabled.enable_eye_local_bloom = true;
    Expect(!ShouldUseEyeLocalBloom(enabled, false, true),
           "missing optional shader falls back to the copy path");
    Expect(!ShouldUseEyeLocalBloom(enabled, true, false),
           "invalid eye-local bounds fall back to the copy path");
    Expect(ShouldUseEyeLocalBloom(enabled, true, true),
           "explicit opt-in uses bloom only with a complete safe path");
}

void TestExactOptionalBloomShaderCompiles()
{
    const char* const source =
        wawvr::xr::compositor_detail::EyeLocalBloomShaderSourceForTesting();
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), "wawvr_eye_local_bloom.hlsl", nullptr,
        nullptr, "PSBloom", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
        bytecode.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result) && errors != nullptr)
    {
        std::cerr << static_cast<const char*>(errors->GetBufferPointer())
                  << '\n';
    }
    Expect(SUCCEEDED(result) && bytecode != nullptr,
           "the exact optional eye-local bloom shader compiles for ps_4_0");
}

} // namespace

int main()
{
    TestPackedEyesHaveDisjointBilinearBounds();
    TestInvalidSamplingRegionsFailClosed();
    TestBloomRequiresExplicitCompleteOptIn();
    TestExactOptionalBloomShaderCompiles();
    if (failures != 0)
    {
        std::cerr << failures << " D3D11 compositor test(s) failed\n";
        return 1;
    }
    std::cout << "D3D11 compositor tests passed\n";
    return 0;
}
