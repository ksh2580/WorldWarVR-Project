#pragma once

#include <cstdint>

namespace wawvr::mod {

inline constexpr std::uint32_t kFloatOneBits = 0x3F800000U;

// Implements the exact ordered comparison used by the verified stereo LOD
// path without converting the source bits. Positive finite values above
// one and positive infinity clamp; NaNs, signed values, and <= 1.0 survive
// bit-for-bit. The inactive path is an unconditional identity transform.
[[nodiscard]] constexpr std::uint32_t select_lod_tan_half_fov_y_bits(
    const std::uint32_t source_bits, const bool stereo_scene_active) noexcept {
    constexpr std::uint32_t kSignBit = 0x80000000U;
    constexpr std::uint32_t kPositiveInfinityBits = 0x7F800000U;
    const std::uint32_t magnitude = source_bits & ~kSignBit;
    if (stereo_scene_active && (source_bits & kSignBit) == 0 &&
        magnitude > kFloatOneBits && magnitude <= kPositiveInfinityBits) {
        return kFloatOneBits;
    }
    return source_bits;
}

} // namespace wawvr::mod
