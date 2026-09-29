// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "core/signals.h"
#include "emulator.h"

#if defined(ARCH_X86_64)
#define Crash() __asm__ __volatile__("int $3")
#elif defined(ARCH_ARM64)
#define Crash() __asm__ __volatile__("brk 0")
#else
#error "Missing Crash() implementation for target CPU architecture."
#endif

#ifdef _WIN32
#include <cstdio>
#include <windows.h>

// DEBUG: the asynchronous log is often lost when an assert kills the process, so write the
// host call stack straight to stderr first (symbolize with the exe base printed here).
static void WriteAssertStack() {
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    char line[128];
    DWORD written = 0;
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    int len = snprintf(line, sizeof(line), "ASSERT-STACK tid=%lu exe_base=%p\n",
                       GetCurrentThreadId(), static_cast<void*>(GetModuleHandleW(nullptr)));
    WriteFile(err, line, static_cast<DWORD>(len), &written, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        len = snprintf(line, sizeof(line), "  #%02u %p\n", i, frames[i]);
        WriteFile(err, line, static_cast<DWORD>(len), &written, nullptr);
    }
}
#endif

void assert_fail_impl() {
#ifdef _WIN32
    WriteAssertStack();
#endif
    Core::Signals::Instance()->RemoveHandlers();
    Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    Crash();
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
