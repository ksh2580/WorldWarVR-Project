#include "peer_thread_quiescence.hpp"

#include <tlhelp32.h>

namespace wawvr::mod {
namespace {

class ScopedHandle final {
public:
    explicit ScopedHandle(const HANDLE handle) noexcept : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_{};
};

void set_failure(
    PeerThreadQuiesceResult* const result,
    const PeerThreadQuiesceStatus status,
    const DWORD system_error,
    const DWORD thread_id = 0) noexcept {
    if (result != nullptr) {
        result->status = status;
        result->system_error = system_error;
        result->thread_id = thread_id;
    }
}

void note_vanished_thread(PeerThreadQuiesceResult* const result) noexcept {
    if (result != nullptr) {
        ++result->vanished_thread_count;
    }
}

[[nodiscard]] bool has_nonempty_patch_range(
    const std::span<const PeerThreadPatchRange> ranges) noexcept {
    for (const auto& range : ranges) {
        if (range.size != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

SuspendedPeerThreads::~SuspendedPeerThreads() { resume(); }

bool SuspendedPeerThreads::suspend(
    const std::span<const PeerThreadPatchRange> patch_ranges,
    PeerThreadQuiesceResult* const result) noexcept {
    if (result != nullptr) {
        *result = {};
    }
    if (thread_count_ != 0 || !has_nonempty_patch_range(patch_ranges)) {
        set_failure(
            result, PeerThreadQuiesceStatus::invalid_patch_ranges,
            ERROR_INVALID_PARAMETER);
        return false;
    }

    const DWORD process_id = GetCurrentProcessId();
    const DWORD current_thread_id = GetCurrentThreadId();
    const HANDLE snapshot_handle =
        CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot_handle == INVALID_HANDLE_VALUE) {
        set_failure(
            result, PeerThreadQuiesceStatus::snapshot_failed,
            GetLastError());
        return false;
    }
    const ScopedHandle snapshot(snapshot_handle);

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.get(), &entry)) {
        set_failure(
            result, PeerThreadQuiesceStatus::enumeration_failed,
            GetLastError());
        return false;
    }

    for (;;) {
        if (entry.th32OwnerProcessID == process_id &&
            entry.th32ThreadID != current_thread_id) {
            if (thread_count_ == threads_.size()) {
                set_failure(
                    result, PeerThreadQuiesceStatus::capacity_exceeded,
                    ERROR_INSUFFICIENT_BUFFER, entry.th32ThreadID);
                return false;
            }

            HANDLE thread = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                    THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                const DWORD error = GetLastError();
                if (classify_peer_thread_failure(
                        PeerThreadFailureStage::open, error, WAIT_FAILED) ==
                    PeerThreadFailureDisposition::vanished) {
                    note_vanished_thread(result);
                } else {
                    set_failure(
                        result, PeerThreadQuiesceStatus::thread_open_failed,
                        error, entry.th32ThreadID);
                    return false;
                }
            } else {
                const DWORD owner_process_id = GetProcessIdOfThread(thread);
                if (owner_process_id == 0) {
                    const DWORD query_error = GetLastError();
                    const DWORD wait_result =
                        WaitForSingleObject(thread, 0);
                    if (classify_peer_thread_failure(
                            PeerThreadFailureStage::context, query_error,
                            wait_result) ==
                        PeerThreadFailureDisposition::vanished) {
                        note_vanished_thread(result);
                        CloseHandle(thread);
                        thread = nullptr;
                    } else {
                        CloseHandle(thread);
                        set_failure(
                            result,
                            PeerThreadQuiesceStatus::thread_query_failed,
                            query_error, entry.th32ThreadID);
                        return false;
                    }
                } else if (owner_process_id != process_id) {
                    // The snapshot thread exited and its ID was recycled for
                    // another process before OpenThread. Never suspend it.
                    note_vanished_thread(result);
                    CloseHandle(thread);
                    thread = nullptr;
                }

                if (thread != nullptr) {
                    const DWORD initial_wait = WaitForSingleObject(thread, 0);
                    if (initial_wait == WAIT_OBJECT_0) {
                        note_vanished_thread(result);
                        CloseHandle(thread);
                        thread = nullptr;
                    } else if (initial_wait != WAIT_TIMEOUT) {
                        const DWORD wait_error = initial_wait == WAIT_FAILED
                            ? GetLastError()
                            : ERROR_GEN_FAILURE;
                        CloseHandle(thread);
                        set_failure(
                            result,
                            PeerThreadQuiesceStatus::thread_query_failed,
                            wait_error, entry.th32ThreadID);
                        return false;
                    }
                }

                if (thread != nullptr &&
                    SuspendThread(thread) == static_cast<DWORD>(-1)) {
                    const DWORD suspend_error = GetLastError();
                    const DWORD wait_result =
                        WaitForSingleObject(thread, 0);
                    if (classify_peer_thread_failure(
                            PeerThreadFailureStage::suspend, suspend_error,
                            wait_result) ==
                        PeerThreadFailureDisposition::vanished) {
                        note_vanished_thread(result);
                        CloseHandle(thread);
                        thread = nullptr;
                    } else {
                        CloseHandle(thread);
                        set_failure(
                            result,
                            PeerThreadQuiesceStatus::thread_suspend_failed,
                            suspend_error, entry.th32ThreadID);
                        return false;
                    }
                }

                if (thread != nullptr) {
                    threads_[thread_count_++] = {
                        thread, entry.th32ThreadID, true};
                }
            }
        }

        entry.dwSize = sizeof(entry);
        SetLastError(ERROR_SUCCESS);
        if (!Thread32Next(snapshot.get(), &entry)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                set_failure(
                    result, PeerThreadQuiesceStatus::enumeration_failed,
                    error);
                return false;
            }
            break;
        }
    }

    for (std::size_t index = 0; index < thread_count_; ++index) {
        auto& thread = threads_[index];
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread.handle, &context)) {
            const DWORD context_error = GetLastError();
            const DWORD wait_result =
                WaitForSingleObject(thread.handle, 0);
            if (classify_peer_thread_failure(
                    PeerThreadFailureStage::context, context_error,
                    wait_result) ==
                PeerThreadFailureDisposition::vanished) {
                note_vanished_thread(result);
                thread.suspended = false;
                CloseHandle(thread.handle);
                thread.handle = nullptr;
                continue;
            }
            set_failure(
                result, PeerThreadQuiesceStatus::thread_context_failed,
                context_error, thread.id);
            return false;
        }
#if defined(_M_IX86)
        const std::uintptr_t instruction = context.Eip;
#elif defined(_M_X64)
        const std::uintptr_t instruction = context.Rip;
#else
#error Unsupported Windows architecture for peer-thread quiescence.
#endif
        if (instruction_in_patch_ranges(instruction, patch_ranges)) {
            set_failure(
                result,
                PeerThreadQuiesceStatus::thread_inside_patch_range,
                ERROR_BUSY, thread.id);
            return false;
        }
    }

    if (result != nullptr) {
        result->status = PeerThreadQuiesceStatus::acquired;
        result->system_error = ERROR_SUCCESS;
        result->thread_id = 0;
    }
    return true;
}

void SuspendedPeerThreads::resume() noexcept {
    while (thread_count_ != 0) {
        auto& thread = threads_[--thread_count_];
        if (thread.handle != nullptr) {
            if (thread.suspended) {
                ResumeThread(thread.handle);
            }
            CloseHandle(thread.handle);
        }
        thread = {};
    }
}

} // namespace wawvr::mod
