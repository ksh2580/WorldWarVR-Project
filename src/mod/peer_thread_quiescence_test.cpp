#include "peer_thread_quiescence.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

void expect(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace wawvr::mod;

    expect(
        classify_peer_thread_failure(
            PeerThreadFailureStage::open, ERROR_INVALID_PARAMETER,
            WAIT_FAILED) == PeerThreadFailureDisposition::vanished,
        "a vanished Toolhelp snapshot thread is skippable");
    expect(
        classify_peer_thread_failure(
            PeerThreadFailureStage::open, ERROR_ACCESS_DENIED,
            WAIT_OBJECT_0) == PeerThreadFailureDisposition::fail_closed,
        "OpenThread access failures remain fatal");
    expect(
        classify_peer_thread_failure(
            PeerThreadFailureStage::suspend, ERROR_INVALID_PARAMETER,
            WAIT_TIMEOUT) == PeerThreadFailureDisposition::fail_closed,
        "a live thread with a suspend failure remains fatal");
    expect(
        classify_peer_thread_failure(
            PeerThreadFailureStage::suspend, ERROR_INVALID_HANDLE,
            WAIT_OBJECT_0) == PeerThreadFailureDisposition::vanished,
        "a signaled thread object proves post-open termination");
    expect(
        classify_peer_thread_failure(
            PeerThreadFailureStage::context, ERROR_GEN_FAILURE,
            WAIT_FAILED) == PeerThreadFailureDisposition::fail_closed,
        "an unprovable context failure remains fatal");

    constexpr std::array<PeerThreadPatchRange, 2> ranges{{
        {0x1000u, 5u},
        {0x2000u, 8u},
    }};
    expect(
        instruction_in_patch_ranges(0x1000u, ranges),
        "the first byte is inside a protected range");
    expect(
        instruction_in_patch_ranges(0x1004u, ranges),
        "the final byte is inside a protected range");
    expect(
        !instruction_in_patch_ranges(0x1005u, ranges),
        "the end address is outside a half-open protected range");
    expect(
        instruction_in_patch_ranges(0x2007u, ranges),
        "all supplied patch ranges are checked");
    expect(
        !instruction_in_patch_ranges(0x1fffu, ranges),
        "addresses between protected ranges remain outside");

    SuspendedPeerThreads suspended;
    PeerThreadQuiesceResult result{};
    expect(
        !suspended.suspend(
            std::span<const PeerThreadPatchRange>{}, &result) &&
            result.status ==
                PeerThreadQuiesceStatus::invalid_patch_ranges &&
            result.system_error == ERROR_INVALID_PARAMETER,
        "an empty patch-range set fails before thread enumeration");

    return EXIT_SUCCESS;
}
