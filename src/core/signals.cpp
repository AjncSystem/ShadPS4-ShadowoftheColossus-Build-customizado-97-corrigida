// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>
#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <atomic>
#include <bit>
#include <map>
#include <mutex>
#include <windows.h>
#include <tlhelp32.h>
#include "common/memory_patcher.h"
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

namespace Core {

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// SOTCWATCH: in-emulator data watchpoints for the Shadow of the Colossus NaN hunt.
// A game patch puts int3 at eboot+0xF4059A ("mov [r14+0xf0], rax", 7 bytes), where rax holds
// the "cur" particle position array and [r14+0xe8] the "prev" array. We emulate the store and
// arm hardware write watchpoints on a few particles in every thread of the process.
// ---------------------------------------------------------------------------
static constexpr u64 SOTC_BP_OFFSET = 0xF4059A;
static std::atomic<int> g_sotc_arm_count{0};
static std::atomic<int> g_sotc_hit_count{0};
static u64 g_sotc_watch[4]{};

static void SotcSetDebugRegs(CONTEXT* ctx) {
    ctx->Dr0 = g_sotc_watch[0];
    ctx->Dr1 = g_sotc_watch[1];
    ctx->Dr2 = g_sotc_watch[2];
    ctx->Dr3 = g_sotc_watch[3];
    u64 dr7 = 0;
    for (int i = 0; i < 4; ++i) {
        if (g_sotc_watch[i] != 0) {
            dr7 |= 1ULL << (i * 2);              // local enable
            dr7 |= 0xDULL << (16 + i * 4);       // RW=01 (write), LEN=11 (4 bytes)
        }
    }
    ctx->Dr7 = dr7;
    ctx->Dr6 = 0;
}

static void SotcArmAllThreads(CONTEXT* current) {
    SotcSetDebugRegs(current);
    current->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
            continue;
        }
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                               FALSE, te.th32ThreadID);
        if (th == nullptr) {
            continue;
        }
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT tc{};
            tc.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &tc)) {
                SotcSetDebugRegs(&tc);
                tc.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(th, &tc);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

// --- SOTCFPE: unmask SSE "invalid operation" to trap the first instruction producing a NaN ---
static std::atomic<bool> g_fpe_enabled{false};
static thread_local bool t_fpe_stepping = false;
static std::mutex g_fpe_mutex;
static std::map<u64, u32> g_fpe_sites; // rip -> count

static void SotcUnmaskAllThreads(CONTEXT* current) {
    current->MxCsr &= ~0xBFULL; // clear flags (bits 0-5) and IM (bit 7)
    current->FltSave.MxCsr = static_cast<DWORD>(current->MxCsr);
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
            continue;
        }
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                               FALSE, te.th32ThreadID);
        if (th == nullptr) {
            continue;
        }
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT tc{};
            tc.ContextFlags = CONTEXT_FLOATING_POINT;
            if (GetThreadContext(th, &tc)) {
                tc.MxCsr &= ~0xBFULL;
                tc.FltSave.MxCsr = static_cast<DWORD>(tc.MxCsr);
                tc.ContextFlags = CONTEXT_FLOATING_POINT;
                SetThreadContext(th, &tc);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

static bool SotcFpeHandle(EXCEPTION_POINTERS* pExp) {
    const DWORD code = pExp->ExceptionRecord->ExceptionCode;
    CONTEXT* c = pExp->ContextRecord;
    if (code == EXCEPTION_SINGLE_STEP && t_fpe_stepping && (c->Dr6 & 0xF) == 0) {
        // stepped over the trapping instruction: re-arm the trap for this thread
        t_fpe_stepping = false;
        c->EFlags &= ~0x100ULL;
        c->MxCsr &= ~0xBFULL;
        c->FltSave.MxCsr = static_cast<DWORD>(c->MxCsr);
        c->Dr6 = 0;
        return true;
    }
    if (code != STATUS_FLOAT_MULTIPLE_TRAPS && code != EXCEPTION_FLT_INVALID_OPERATION) {
        return false;
    }
    if (!g_fpe_enabled.load()) {
        return false;
    }
    const u64 base = MemoryPatcher::g_eboot_address;
    const u64 rip = c->Rip;
    const bool in_eboot = base != 0 && rip >= base && rip < base + 0x3958000;
    u32 count = 0;
    {
        std::lock_guard lk{g_fpe_mutex};
        count = ++g_fpe_sites[rip];
    }
    if (count <= 2) {
        const u32* x0 = reinterpret_cast<const u32*>(&c->Xmm0);
        LOG_CRITICAL(Debug,
                     "SOTCFPE trap thr={} rip={:#x} {} off={:#x} count={} mxcsr={:#x} "
                     "xmm0={:08x} {:08x} {:08x} {:08x}",
                     GetCurrentThreadId(), rip, in_eboot ? "eboot" : "other",
                     in_eboot ? rip - base : 0, count, c->MxCsr, x0[0], x0[1], x0[2], x0[3]);
        if (in_eboot && count == 1) {
            for (int i = 0; i < 16; ++i) {
                const u32* x = reinterpret_cast<const u32*>(&c->Xmm0) + i * 4;
                LOG_CRITICAL(Debug, "SOTCFPE   xmm{} {:08x} {:08x} {:08x} {:08x}", i, x[0], x[1],
                             x[2], x[3]);
            }
            LOG_CRITICAL(Debug,
                         "SOTCFPE   rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} rsi={:#x} rdi={:#x} "
                         "rbp={:#x} rsp={:#x} r8={:#x} r9={:#x} r12={:#x} r13={:#x} r14={:#x} "
                         "r15={:#x}",
                         c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8,
                         c->R9, c->R12, c->R13, c->R14, c->R15);
        }
    }
    if (in_eboot && rip - base == 0x160945 && count <= 4) {
        // singular 3x4 matrix inverse: dump the input (rsi) and the call stack
        const u32* m = reinterpret_cast<const u32*>(c->Rsi);
        LOG_CRITICAL(Debug,
                     "SOTCFPE INV#{} thr={} src={:#x} m={:08x} {:08x} {:08x} {:08x} | {:08x} "
                     "{:08x} {:08x} {:08x} | {:08x} {:08x} {:08x} {:08x}",
                     count, GetCurrentThreadId(), c->Rsi, m[0], m[1], m[2], m[3], m[4], m[5], m[6],
                     m[7], m[8], m[9], m[10], m[11]);
        if (count == 1 && c->Rsi - c->R14 == 0x10bc) {
            for (u64 off : {0x0ULL, 0x40ULL, 0x80ULL, 0xc0ULL, 0x1040ULL, 0x1080ULL, 0x10c0ULL,
                            0x1100ULL}) {
                for (u64 k = 0; k < 0x40; k += 0x10) {
                    const u32* q = reinterpret_cast<const u32*>(c->R14 + off + k);
                    LOG_CRITICAL(Debug, "SOTCFPE INVOBJ +{:#06x}: {:08x} {:08x} {:08x} {:08x}",
                                 off + k, q[0], q[1], q[2], q[3]);
                }
            }
        }
        const u64* sp = reinterpret_cast<const u64*>(c->Rsp);
        for (int i = 0; i < 160; ++i) {
            const u64 v = sp[i];
            if (v >= base && v < base + 0x1594000) {
                LOG_CRITICAL(Debug, "SOTCFPE INV#{}   ret? rsp+{:#x}: off={:#x}", count, i * 8,
                             v - base);
            }
        }
    }
    if (!in_eboot && count > 50) {
        // noisy non-game code: leave it masked on this thread
        c->MxCsr = (c->MxCsr & ~0x3FULL) | 0x80;
        c->FltSave.MxCsr = static_cast<DWORD>(c->MxCsr);
        return true;
    }
    // mask, clear flags and single-step over the faulting instruction
    c->MxCsr = (c->MxCsr & ~0x3FULL) | 0x80;
    c->FltSave.MxCsr = static_cast<DWORD>(c->MxCsr);
    c->EFlags |= 0x100;
    t_fpe_stepping = true;
    return true;
}

static bool SotcHandle(EXCEPTION_POINTERS* pExp) {
    const DWORD code = pExp->ExceptionRecord->ExceptionCode;
    CONTEXT* c = pExp->ContextRecord;
    const u64 base = MemoryPatcher::g_eboot_address;
    if (SotcFpeHandle(pExp)) {
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 && c->Rip == base + SOTC_BP_OFFSET) {
        // emulate: mov qword ptr [r14 + 0xf0], rax
        *reinterpret_cast<u64*>(c->R14 + 0xf0) = c->Rax;
        c->Rip += 7;
        const int n = g_sotc_arm_count.fetch_add(1);
        const u64 cur = c->Rax;
        const u64 prev = *reinterpret_cast<const u64*>(c->R14 + 0xe8);
        if (n < 200) {
            const u32* pc = reinterpret_cast<const u32*>(cur + 0x30);
            const u32* pp = reinterpret_cast<const u32*>(prev + 0x30);
            LOG_CRITICAL(Debug,
                         "SOTCWATCH arm #{} thr={} obj={:#x} rbx={:#x} prev={:#x} cur={:#x} "
                         "p3.prev={:08x} {:08x} {:08x} p3.cur={:08x} {:08x} {:08x}",
                         n, GetCurrentThreadId(), c->R14, c->Rbx, prev, cur, pp[0], pp[1], pp[2],
                         pc[0], pc[1], pc[2]);
            static const bool env_watch = [] {
                // SOTC_WATCH=addr0,addr1,... (hex) overrides the automatic prev/cur watches
                char buf[256]{};
                if (GetEnvironmentVariableA("SOTC_WATCH", buf, sizeof(buf)) == 0) {
                    return false;
                }
                char* p = buf;
                for (int i = 0; i < 4 && *p; ++i) {
                    g_sotc_watch[i] = std::strtoull(p, &p, 16);
                    while (*p == ',' || *p == ' ') {
                        ++p;
                    }
                }
                return true;
            }();
            if (env_watch) {
                SotcArmAllThreads(c);
            } else if (prev != cur) {
                const u64 a = std::min(prev, cur), b = std::max(prev, cur);
                if (g_sotc_watch[0] != a + 0x30 || g_sotc_watch[1] != b + 0x30) {
                    g_sotc_watch[0] = a + 0x30; // particle 3 x, both buffers
                    g_sotc_watch[1] = b + 0x30;
                    g_sotc_watch[2] = a + 0xF0; // particle 15 x, both buffers
                    g_sotc_watch[3] = b + 0xF0;
                }
                SotcArmAllThreads(c); // re-arm every time to cover newly created threads
            }
        }
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 && c->Rip == base + 0xED8D45) {
        // emulate: add r12, 0xc (4 bytes). r12 = source position array of the copy loop.
        const u64 src = c->R12;
        c->R12 += 0xc;
        c->Rip += 4;
        static std::atomic<int> n2{0};
        const int n = n2.fetch_add(1);
        static u64 armed_src = 0;
        const u32* p3 = reinterpret_cast<const u32*>(src + 0x30);
        if (n < 5 || armed_src != src || ((p3[0] & 0x7f800000) == 0x7f800000)) {
            LOG_CRITICAL(Debug, "SOTCWATCH copy #{} thr={} src={:#x} dst={:#x} cnt={} p3={:08x} {:08x} {:08x}",
                         n, GetCurrentThreadId(), src, *reinterpret_cast<const u64*>(c->R13),
                         c->R15, p3[0], p3[1], p3[2]);
        }
        static const bool env_watch2 = [] {
            char buf[256]{};
            if (GetEnvironmentVariableA("SOTC_WATCH", buf, sizeof(buf)) == 0) {
                return false;
            }
            char* p = buf;
            for (int i = 0; i < 4 && *p; ++i) {
                g_sotc_watch[i] = std::strtoull(p, &p, 16);
                while (*p == ',' || *p == ' ') {
                    ++p;
                }
            }
            return true;
        }();
        if (!env_watch2 && armed_src != src) {
            armed_src = src;
            g_sotc_watch[0] = src + 0x30;
            g_sotc_watch[1] = src + 0xF0;
            g_sotc_watch[2] = 0;
            g_sotc_watch[3] = 0;
        }
        SotcArmAllThreads(c);
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 && c->Rip == base + 0xEEFADA) {
        // emulate: xor eax, eax (2 bytes). r14 = world matrix array (64 bytes per bone).
        c->Rax = 0;
        c->EFlags = (c->EFlags & ~0x8D5ULL) | 0x44; // ZF=1 PF=1, CF=OF=SF=AF=0
        c->Rip += 2;
        static std::atomic<int> n3{0};
        const int n = n3.fetch_add(1);
        const u64 w125 = c->R14 + 125 * 0x40;
        const u32* m = reinterpret_cast<const u32*>(w125);
        const bool isnan = (m[0] & 0x7f800000) == 0x7f800000 && (m[0] & 0x7fffff) != 0;
        static u64 armed = 0;
        if (n < 3 || armed != w125 || isnan) {
            LOG_CRITICAL(Debug, "SOTCWATCH xform #{} thr={} world={:#x} local={:#x} w125={:08x} {:08x} {:08x} {:08x}",
                         n, GetCurrentThreadId(), c->R14, c->Rbx, m[0], m[1], m[2], m[3]);
        }
        if (armed != w125) {
            armed = w125;
            // world buffers alternate each frame: watch m[0][0] and translation.x in both
            if (g_sotc_watch[0] == 0) {
                g_sotc_watch[0] = w125;
                g_sotc_watch[2] = w125 + 0x30;
            } else if (g_sotc_watch[0] != w125 && g_sotc_watch[1] == 0) {
                g_sotc_watch[1] = w125;
                g_sotc_watch[3] = w125 + 0x30;
            }
        }
        SotcArmAllThreads(c);
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 &&
        (c->Rip == base + 0xC9836 || c->Rip == base + 0xC9AFF)) {
        // camera constructor: emulate vmovlps [obj+0x1084], xmm0 and initialize the two 3x4
        // matrices at +0x1004 and +0x10bc to identity (they are otherwise left uninitialized).
        const bool second = c->Rip == base + 0xC9AFF;
        const u64 obj = second ? c->R12 : c->Rbx;
        const u64 lo64 = *reinterpret_cast<const u64*>(&c->Xmm0);
        static const float ident[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; // 3 axes + translation
        std::memcpy(reinterpret_cast<void*>(obj + 0x10bc), ident, sizeof(ident));
        *reinterpret_cast<u64*>(obj + 0x1084) = lo64;
        c->Rip += second ? 10 : 8;
        static std::atomic<int> n{0};
        if (n.fetch_add(1) < 4) {
            LOG_CRITICAL(Debug, "SOTCFIX camera init obj={:#x}", obj);
        }
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 && c->Rip == base + 0x1292D) {
        // allocator return: emulate mov rax, r13 (3 bytes). r13 = block, r12 = size,
        // rbx = allocator object, [rbp+8] = caller.
        c->Rax = c->R13;
        c->Rip += 3;
        static u64 lo = 0, hi = 0;
        static std::once_flag once;
        std::call_once(once, [] {
            char buf[64]{};
            if (GetEnvironmentVariableA("SOTC_RANGE", buf, sizeof(buf)) != 0) {
                char* p = buf;
                lo = std::strtoull(p, &p, 16);
                while (*p == ',') {
                    ++p;
                }
                hi = std::strtoull(p, &p, 16);
            }
        });
        const u64 blk = c->R13;
        const u64 size = c->R12;
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        if (blk != 0 && blk < hi && blk + size > lo) {
            const u64 caller = *reinterpret_cast<const u64*>(c->Rbp + 8);
            const u64 caller2 = *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(c->Rbp) + 8);
            LOG_CRITICAL(Debug,
                         "SOTCPOOL alloc#{} thr={} blk={:#x} size={:#x} alloc_obj={:#x} vt={:#x} "
                         "caller={:#x} caller2={:#x}",
                         n, GetCurrentThreadId(), blk, size, c->Rbx,
                         *reinterpret_cast<const u64*>(c->Rbx) - base, caller - base,
                         caller2 - base);
        }
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 && c->Rip == base + 0xA3EE9F) {
        // emulate: mov qword ptr [rbp - 0x30], rax (4 bytes). rax = temp 4x4 array just allocated
        *reinterpret_cast<u64*>(c->Rbp - 0x30) = c->Rax;
        c->Rip += 4;
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1);
        static const bool env_watch5 = [] {
            char buf[256]{};
            if (GetEnvironmentVariableA("SOTC_WATCH", buf, sizeof(buf)) == 0) {
                return false;
            }
            char* p = buf;
            for (int i = 0; i < 4 && *p; ++i) {
                g_sotc_watch[i] = std::strtoull(p, &p, 16);
                while (*p == ',' || *p == ' ') {
                    ++p;
                }
            }
            return true;
        }();
        if (env_watch5 && (n < 4 || n % 32 == 0)) {
            SotcArmAllThreads(c);
        }
        const u64 cam = g_sotc_watch[0];
        const u64 cnt = c->Rbx & 0xffffffff; // rbx = element count (computed before the call)
        const bool overlap = cam != 0 && c->Rax < cam + 0x1200 && c->Rax + cnt * 0x40 > cam;
        if (n < 20 || overlap) {
            LOG_CRITICAL(Debug, "SOTCALLOC #{} thr={} temp4x4={:#x} count={} rsp={:#x}{}", n,
                         GetCurrentThreadId(), c->Rax, cnt, c->Rsp,
                         overlap ? " <== OVERLAPS CAMERA" : "");
        }
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 && c->Rip == base + 0x12888) {
        // allocator type 1: rax = &tls_scratch_stack (fs:[0] + tls offset). emulate mov rax,[rax]
        const u64 tlsvar = c->Rax;
        const u64 val = *reinterpret_cast<const u64*>(tlsvar);
        c->Rax = val;
        c->Rip += 3;
        static std::mutex mu;
        static std::map<u64, std::pair<DWORD, u64>> owner; // tlsvar -> (first thread, first val)
        static std::map<DWORD, u64> by_thread;             // thread -> tlsvar
        static std::atomic<int> logs{0};
        const DWORD tid = GetCurrentThreadId();
        std::lock_guard lk{mu};
        auto it = owner.find(tlsvar);
        if (it == owner.end()) {
            owner[tlsvar] = {tid, val};
            if (logs.fetch_add(1) < 200) {
                LOG_CRITICAL(Debug, "SOTCTLS new tlsvar={:#x} thr={} stack={:#x} (distinct={})",
                             tlsvar, tid, val, owner.size());
            }
        } else if (it->second.first != tid) {
            if (logs.fetch_add(1) < 200) {
                LOG_CRITICAL(Debug,
                             "SOTCTLS SHARED tlsvar={:#x} first_thr={} now_thr={} stack={:#x} "
                             "rsp={:#x}",
                             tlsvar, it->second.first, tid, val, c->Rsp);
            }
        }
        static const bool env_watch4 = [] {
            char buf[256]{};
            if (GetEnvironmentVariableA("SOTC_WATCH", buf, sizeof(buf)) == 0) {
                return false;
            }
            char* p = buf;
            for (int i = 0; i < 4 && *p; ++i) {
                g_sotc_watch[i] = std::strtoull(p, &p, 16);
                while (*p == ',' || *p == ' ') {
                    ++p;
                }
            }
            return true;
        }();
        static int arm_tick = 0;
        if (env_watch4 && (arm_tick++ % 256) == 0) {
            SotcArmAllThreads(c);
        }
        auto bt = by_thread.find(tid);
        if (bt == by_thread.end()) {
            by_thread[tid] = tlsvar;
        } else if (bt->second != tlsvar && logs.fetch_add(1) < 200) {
            LOG_CRITICAL(Debug, "SOTCTLS thread {} switched tlsvar {:#x} -> {:#x} rsp={:#x}", tid,
                         bt->second, tlsvar, c->Rsp);
            bt->second = tlsvar;
        }
        return true;
    }
    if (code == EXCEPTION_BREAKPOINT && base != 0 && c->Rip == base + 0xF227A0) {
        // emulate: mov dword ptr [rbx], eax (2 bytes). [rbp-0x70..-0x30] = new world matrix,
        // [rbp-0xb0..] = local pose matrix, [rbp-0x148] -> parent matrix, r12d = bone index.
        *reinterpret_cast<u32*>(c->Rbx) = static_cast<u32>(c->Rax);
        c->Rip += 2;
        const auto has_nan = [](u64 a, int n) {
            const u32* x = reinterpret_cast<const u32*>(a);
            for (int i = 0; i < n; ++i) {
                if ((x[i] & 0x7f800000) == 0x7f800000) {
                    return true;
                }
            }
            return false;
        };
        static std::atomic<int> nan_logs{0};
        static std::atomic<int> calls{0};
        calls.fetch_add(1);
        static const bool env_watch3 = [] {
            char buf[256]{};
            if (GetEnvironmentVariableA("SOTC_WATCH", buf, sizeof(buf)) == 0) {
                return false;
            }
            char* p = buf;
            for (int i = 0; i < 4 && *p; ++i) {
                g_sotc_watch[i] = std::strtoull(p, &p, 16);
                while (*p == ',' || *p == ' ') {
                    ++p;
                }
            }
            return true;
        }();
        if (env_watch3 && (calls.load() < 8 || calls.load() % 64 == 1)) {
            SotcArmAllThreads(c);
        }
        static const bool fpe_mode = GetEnvironmentVariableA("SOTC_FPE", nullptr, 0) != 0;
        if (fpe_mode) {
            // periodically (re-)unmask invalid-op traps on every thread (new threads included)
            static std::atomic<int> fpe_arms{0};
            if (calls.load() % 64 == 1 && fpe_arms.fetch_add(1) < 200) {
                g_fpe_enabled = true;
                SotcUnmaskAllThreads(c);
                if (fpe_arms.load() == 1) {
                    LOG_CRITICAL(Debug, "SOTCFPE enabled at calls={}", calls.load());
                }
            }
        }
        if (!fpe_mode) {
            // arm write watchpoints on bone0 of every distinct pose buffer we see (max 2)
            const u64 pb = *reinterpret_cast<const u64*>(c->Rbp - 0x158);
            if (pb != 0 && static_cast<u32>(c->R12) == 0) {
                bool changed = false;
                if (g_sotc_watch[0] == 0) {
                    g_sotc_watch[0] = pb;        // bone0 m00
                    g_sotc_watch[2] = pb + 0x30; // bone0 translation x
                    changed = true;
                } else if (g_sotc_watch[0] != pb && g_sotc_watch[1] == 0) {
                    g_sotc_watch[1] = pb;
                    g_sotc_watch[3] = pb + 0x30;
                    changed = true;
                }
                if (changed) {
                    LOG_CRITICAL(Debug, "SOTCWATCH posebuf armed {:#x} thr={} calls={}", pb,
                                 GetCurrentThreadId(), calls.load());
                }
                SotcArmAllThreads(c);
            }
        }
        if (has_nan(c->Rbp - 0x70, 16) && nan_logs.fetch_add(1) < 6) {
            const auto dump16 = [](const char* name, u64 a, int n) {
                for (int i = 0; i < n; i += 4) {
                    const u32* x = reinterpret_cast<const u32*>(a) + i;
                    LOG_CRITICAL(Debug, "SOTCWATCH {} {:#x}: {:08x} {:08x} {:08x} {:08x}", name,
                                 a + i * 4, x[0], x[1], x[2], x[3]);
                }
            };
            const u64 parent = *reinterpret_cast<const u64*>(c->Rbp - 0x148);
            const u64 posebuf = *reinterpret_cast<const u64*>(c->Rbp - 0x158);
            LOG_CRITICAL(Debug,
                         "SOTCWATCH WORLD-NaN bone={} thr={} calls={} dst={:#x} parent={:#x} "
                         "posebuf={:#x} stride={:#x} r15={:#x}",
                         static_cast<u32>(c->R12), GetCurrentThreadId(), calls.load(), c->Rbx,
                         parent, posebuf, *reinterpret_cast<const u32*>(c->Rbp - 0x13c), c->R15);
            dump16("new", c->Rbp - 0x70, 16);
            dump16("pose", c->Rbp - 0xb0, 16);
            dump16("parent", parent, 16);
        }
        return true;
    }
    if (code == EXCEPTION_SINGLE_STEP && (c->Dr6 & 0xF) != 0) {
        const int hit = g_sotc_hit_count.fetch_add(1);
        const int which = static_cast<int>(std::countr_zero(static_cast<u32>(c->Dr6 & 0xF)));
        const u64 addr = g_sotc_watch[which];
        const u32* v = reinterpret_cast<const u32*>(addr);
        const bool nan = (v[0] & 0x7f800000) == 0x7f800000 && (v[0] & 0x7fffff) != 0;
        static thread_local u64 last_key[4]{};
        const u64 key = (static_cast<u64>(v[0]) << 32) ^ v[1] ^ (c->Rip << 7);
        const bool changed = last_key[which] != key;
        last_key[which] = key;
        if ((changed && hit < 20000) || nan) {
            LOG_CRITICAL(Debug,
                         "SOTCWATCH hit#{} dr{} addr={:#x} rip={:#x} off={:#x} val={:08x} {:08x} "
                         "{:08x} {:08x}{}",
                         hit, which, addr, c->Rip, c->Rip - base, v[0], v[1], v[2], v[3],
                         nan ? " <== NaN" : "");
        }
        if (nan) {
            LOG_CRITICAL(Debug,
                         "SOTCWATCH NaN regs rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x} rsi={:#x} "
                         "rdi={:#x} rbp={:#x} rsp={:#x} r8={:#x} r9={:#x} r10={:#x} r11={:#x} "
                         "r12={:#x} r13={:#x} r14={:#x} r15={:#x}",
                         c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8,
                         c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
            for (int i = 0; i < 16; ++i) {
                const u32* x = reinterpret_cast<const u32*>(&c->Xmm0) + i * 4;
                LOG_CRITICAL(Debug, "SOTCWATCH xmm{} {:08x} {:08x} {:08x} {:08x}", i, x[0], x[1],
                             x[2], x[3]);
            }
            if (c->Rip - base == 0xEEFBF6) {
                // bone transform: entry = r13 + rdx*4 (5 dwords), local = rbx + bone*0x68,
                // world = r14 + bone*0x40 (bone*0x40 is in rsi)
                const auto dump16 = [](const char* name, u64 a, int n) {
                    for (int i = 0; i < n; i += 4) {
                        const u32* x = reinterpret_cast<const u32*>(a) + i;
                        LOG_CRITICAL(Debug, "SOTCWATCH {} {:#x}: {:08x} {:08x} {:08x} {:08x}",
                                     name, a + i * 4, x[0], x[1], x[2], x[3]);
                    }
                };
                const u64 bone = c->Rsi >> 6;
                LOG_CRITICAL(Debug, "SOTCWATCH bone={} local_base={:#x} world_base={:#x}", bone,
                             c->Rbx, c->R14);
                dump16("entry", c->R13 + c->Rdx * 4, 8);
                dump16("local", c->Rbx + bone * 0x68, 28);
                dump16("world", c->R14 + c->Rsi, 16);
                g_sotc_watch[2] = c->Rbx + bone * 0x68 + 0x14; // local matrix first element
                g_sotc_watch[3] = c->R14 + c->Rsi;              // world matrix first element
            }
            const u64* sp = reinterpret_cast<const u64*>(c->Rsp);
            for (int i = 0; i < 64; i += 4) {
                LOG_CRITICAL(Debug, "SOTCWATCH stack+{:#x}: {:016x} {:016x} {:016x} {:016x}",
                             i * 8, sp[i], sp[i + 1], sp[i + 2], sp[i + 3]);
            }
        }
        c->Dr6 = 0;
        return true;
    }
    return false;
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    using namespace Libraries::Kernel;
    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        switch (pExp->ExceptionRecord->ExceptionCode) {
        case 0xE06D7363: // C++ exception, handled by its own catch blocks
        case 0x406D1388: // Thread naming (MS_VC_EXCEPTION)
        case 0x40010006: // DBG_PRINTEXCEPTION_C
        case 0x4001000A: // DBG_PRINTEXCEPTION_WIDE_C
            return EXCEPTION_CONTINUE_SEARCH;
        default:
            break;
        }
    }
    // DEBUG: report exceptions raised while this thread is already handling one. Written with
    // WriteFile only, so it works even when the logger or the heap is the problem.
    thread_local int handler_depth = 0;
    struct DepthGuard {
        int& depth;
        ~DepthGuard() {
            --depth;
        }
    } depth_guard{++handler_depth};
    if (handler_depth >= 2 && pExp && pExp->ExceptionRecord && pExp->ContextRecord) {
        char line[256];
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        const int len = snprintf(
            line, sizeof(line),
            "NESTED-EXCEPTION depth=%d code=%08lx at=%p rip=%llx rsp=%llx fault=%llx "
            "stack=[%llx,%llx) tid=%lu\n",
            handler_depth, pExp->ExceptionRecord->ExceptionCode,
            pExp->ExceptionRecord->ExceptionAddress, pExp->ContextRecord->Rip,
            pExp->ContextRecord->Rsp,
            pExp->ExceptionRecord->NumberParameters > 1
                ? static_cast<unsigned long long>(pExp->ExceptionRecord->ExceptionInformation[1])
                : 0ULL,
            static_cast<unsigned long long>(low), static_cast<unsigned long long>(high),
            GetCurrentThreadId());
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(len), &written,
                  nullptr);
        if (handler_depth >= 8) {
            // Give up instead of recursing until the stack is gone.
            TerminateProcess(GetCurrentProcess(), 0xDEAD0001);
        }
    }
    if (pExp != nullptr && pExp->ExceptionRecord != nullptr && SotcHandle(pExp)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const auto* signals = Signals::Instance();

    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    Ucontext guest_context{pExp->ContextRecord};
    Siginfo guest_info{
        ._si_signo = 0,
        ._si_errno = 0,
        ._si_code = POSIX_SI_NOINFO,
        ._si_addr = (void*)guest_context.uc_mcontext.mc_rip,
    };

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        guest_info._si_signo = POSIX_SIGSEGV;
        guest_info._si_code = POSIX_SEGV_MAPERR;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_ILLOPC;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        guest_info._si_signo = POSIX_SIGBUS;
        guest_info._si_code = POSIX_BUS_ADRALN;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTDIV;
        break;
    case EXCEPTION_INT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTOVF;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTDIV;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTINV;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTOVF;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTUND;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTSUB; // i am not sure about this one
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTRES;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_BADSTK; // i am not sure about this one either
        break;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        guest_info._si_signo = POSIX_SIGTRAP;
        guest_info._si_code = POSIX_TRAP_BRKPT;
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (guest_info._si_signo != 0) {
        if (g_curthread &&
            g_curthread->DispatchSignal(guest_info._si_signo, &guest_info, &guest_context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const bool report_unhandled =
        use_static_windows_guest_red_zone_protection ? static_protection_exception : true;
    if (report_unhandled) {
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        // CRASHDUMP: registers, fault address and stack scan for post-mortem analysis
        if (pExp != nullptr && pExp->ContextRecord != nullptr) {
            const CONTEXT* c = pExp->ContextRecord;
            if (code == EXCEPTION_ACCESS_VIOLATION) {
                LOG_CRITICAL(Debug, "CRASHDUMP fault: {} addr={:#x}",
                             pExp->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                             pExp->ExceptionRecord->ExceptionInformation[1]);
            }
            LOG_CRITICAL(Debug, "CRASHDUMP host_exe_base={} eboot_base={:#x} eboot_off={:#x}",
                         fmt::ptr(GetModuleHandleW(nullptr)), MemoryPatcher::g_eboot_address,
                         c->Rip - MemoryPatcher::g_eboot_address);
            LOG_CRITICAL(Debug,
                         "CRASHDUMP rip={:#x} rsp={:#x} rbp={:#x} rax={:#x} rbx={:#x} "
                         "rcx={:#x} rdx={:#x} rsi={:#x} rdi={:#x}",
                         c->Rip, c->Rsp, c->Rbp, c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi,
                         c->Rdi);
            LOG_CRITICAL(Debug,
                         "CRASHDUMP r8={:#x} r9={:#x} r10={:#x} r11={:#x} r12={:#x} "
                         "r13={:#x} r14={:#x} r15={:#x}",
                         c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
            const auto readable = [](u64 addr, size_t len) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
                    return false;
                }
                const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                 PAGE_EXECUTE_WRITECOPY;
                return mbi.State == MEM_COMMIT && (mbi.Protect & ok) != 0 &&
                       (mbi.Protect & PAGE_GUARD) == 0 &&
                       addr + len <= reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
            };
            const auto dump_mem = [&](const char* name, u64 base, int qwords) {
                for (int i = 0; i < qwords; i += 2) {
                    const u64 a = base + i * 8;
                    if (!readable(a, 16)) {
                        LOG_CRITICAL(Debug, "CRASHDUMP {} {:#x}: <unreadable>", name, a);
                        return;
                    }
                    const u32* d = reinterpret_cast<const u32*>(a);
                    LOG_CRITICAL(Debug, "CRASHDUMP {} {:#x}: {:08x} {:08x} {:08x} {:08x}", name, a,
                                 d[0], d[1], d[2], d[3]);
                }
            };
            dump_mem("r9", c->R9 - 0x40, 24);
            dump_mem("rdi", c->Rdi + 0x180, 4);
            // SotC: find the owner object (obj+0x1b0 == sphere array in r9) and dump the
            // prev/cur position arrays the spheres are interpolated from.
            {
                std::vector<u64> cands{c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp,
                                       c->R8,  c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14,
                                       c->R15};
                for (int i = 0; i < 1024 && readable(c->Rsp + i * 8, 8); ++i) {
                    cands.push_back(*reinterpret_cast<const u64*>(c->Rsp + i * 8));
                }
                for (u64 v : cands) {
                    if (v < 0x10000 || !readable(v + 0x1b0, 0x18)) {
                        continue;
                    }
                    const u64 spheres = *reinterpret_cast<const u64*>(v + 0x1b0);
                    if (spheres > c->R9 || c->R9 - spheres > 0x10000) {
                        continue;
                    }
                    const u64 s = *reinterpret_cast<const u64*>(v + 0x1c0);
                    LOG_CRITICAL(Debug, "CRASHDUMP owner={:#x} spheres={:#x} idx={} s={:#x}", v,
                                 spheres, (c->R9 - spheres) / 16, s);
                    if (!readable(s + 0xe0, 0x30)) {
                        break;
                    }
                    dump_mem("s+e0", s + 0xe0, 6);
                    const u64 prev = *reinterpret_cast<const u64*>(s + 0xe8);
                    const u64 cur = *reinterpret_cast<const u64*>(s + 0xf0);
                    const u32 n = *reinterpret_cast<const u32*>(s + 0xf8);
                    LOG_CRITICAL(Debug, "CRASHDUMP prev={:#x} cur={:#x} n={}", prev, cur, n);
                    const int cnt = static_cast<int>(std::min<u32>(n, 64)) * 2;
                    dump_mem("prev", prev, cnt);
                    dump_mem("cur", cur, cnt);
                    dump_mem("sph", spheres, cnt);
                    break;
                }
            }
            for (int i = 0; i < 512; i += 4) {
                const u64 a = c->Rsp + i * 8;
                if (!readable(a, 32)) {
                    LOG_CRITICAL(Debug, "CRASHDUMP stack {:#x}: <unreadable>", a);
                    break;
                }
                const u64* q = reinterpret_cast<const u64*>(a);
                LOG_CRITICAL(Debug, "CRASHDUMP stack {:#x}: {:016x} {:016x} {:016x} {:016x}", a,
                             q[0], q[1], q[2], q[3]);
            }
        }
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

static s32 NativeSiCodeToGuest(s32 sig, s32 code) {
    using namespace Libraries::Kernel;
    switch (sig) {
    case SIGUSR1:
        return POSIX_SI_LWP;
    case SIGSEGV:
        switch (code) {
        case SEGV_MAPERR:
            return POSIX_SEGV_MAPERR;
        case SEGV_ACCERR:
            return POSIX_SEGV_ACCERR;
        }
    case SIGBUS:
        switch (code) {
        case BUS_ADRALN:
            return POSIX_BUS_ADRALN;
        case BUS_ADRERR:
            return POSIX_BUS_ADRERR;
        case BUS_OBJERR:
            return POSIX_BUS_OBJERR;
        }
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC:
            return POSIX_ILL_ILLOPC;
        case ILL_ILLOPN:
            return POSIX_ILL_ILLOPN;
        case ILL_ILLADR:
            return POSIX_ILL_ILLADR;
        case ILL_ILLTRP:
            return POSIX_ILL_ILLTRP;
        case ILL_PRVOPC:
            return POSIX_ILL_PRVOPC;
        case ILL_PRVREG:
            return POSIX_ILL_PRVREG;
        case ILL_COPROC:
            return POSIX_ILL_COPROC;
        case ILL_BADSTK:
            return POSIX_ILL_BADSTK;
        }
    case SIGFPE:
        switch (code) {
        case FPE_INTOVF:
            return POSIX_FPE_INTOVF;
        case FPE_INTDIV:
            return POSIX_FPE_INTDIV;
        case FPE_FLTDIV:
            return POSIX_FPE_FLTDIV;
        case FPE_FLTOVF:
            return POSIX_FPE_FLTOVF;
        case FPE_FLTUND:
            return POSIX_FPE_FLTUND;
        case FPE_FLTRES:
            return POSIX_FPE_FLTRES;
        case FPE_FLTINV:
            return POSIX_FPE_FLTINV;
        case FPE_FLTSUB:
            return POSIX_FPE_FLTSUB;
        }
    case SIGTRAP:
        switch (code) {
        case TRAP_BRKPT:
            return POSIX_TRAP_BRKPT;
        case TRAP_TRACE:
            return POSIX_TRAP_TRACE;
#ifdef __FreeBSD__
        case TRAP_DTRACE:
            return POSIX_TRAP_DTRACE;
#endif
        }

    default:
        return POSIX_SI_NOINFO;
    }
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    auto* thread = g_curthread;
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    Ucontext context{info, reinterpret_cast<ucontext_t*>(raw_context)};
    Siginfo guest_info{};
    if (info) {
        guest_info = *reinterpret_cast<Siginfo*>(info);
        guest_info._si_signo = sig == SIGUSR1 ? 0 : NativeToOrbisSignal(info->si_signo);
        guest_info._si_errno = NativeToPosixErrno(info->si_errno);
        guest_info._si_code = NativeSiCodeToGuest(sig, info->si_code);
        guest_info._si_addr = (void*)context.uc_mcontext.mc_rip;
    }
    Siginfo* info_p = info ? &guest_info : nullptr;
    Ucontext* context_p = raw_context ? &context : nullptr;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        const bool is_exec = Common::IsExecuteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address),
                            is_write  ? "Write to"
                            : is_exec ? "Executed from"
                                      : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (signals->DispatchIllegalInstruction(raw_context)) {
            return;
        }
    case SIGFPE:
    case SIGTRAP:
    case SIGSYS: {
        if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
            return;
        }

        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
    case SIGSLEEP: {
        // Sleep thread until signal is received again
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGSLEEP);
        sigwait(&sigset, &sig);
        break;
    }
    case SIGUSR1:
        if (thread) {
            thread->DispatchPendingSignals(info_p, context_p);
        }
        break;
    default:
        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
}

#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(
        sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
            sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
            sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
            sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGSLEEP, &action, nullptr) == 0,
        "Failed to register signal handlers.");
#endif
}

void SignalDispatch::RemoveHandlers() {
    // asserting here would get into an infinite loop until too
    // many nested exceptions makes the OS kill the process
#if defined(_WIN32)
    if (!(RemoveVectoredExceptionHandler(handle))) {
        LOG_CRITICAL(Core, "Failed to remove exception handler.");
        std::quick_exit(1);
    }
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    if (!(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
          sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
          sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
          sigaction(SIGUSR1, &action, nullptr) == 0 &&
          sigaction(SIGSLEEP, &action, nullptr) == 0)) {
        LOG_CRITICAL(Core, "Failed to remove signal handlers.");
        std::quick_exit(1);
    }
#endif
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
