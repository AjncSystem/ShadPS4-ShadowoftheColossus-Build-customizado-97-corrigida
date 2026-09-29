// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

/// Host helper the walker calls to copy dynamic-index windows (see ReadConstDynamicWindowFlag).
void SrtCopyWindow(u32* dst, u64 src, u64 bytes);
using PFN_SrtCopyWindow = void (*)(u32*, u64, u64);

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/,
                                            PFN_SrtCopyWindow /*copy_window*/);

/// ReadConst flag marking a load whose dword index is only known at shader run time (e.g. a
/// loop counter). The SRT walker copies a window of the pointed-to memory into the flattened
/// buffer; the low bits hold the window's first dword and the index is clamped to its size.
constexpr u32 ReadConstDynamicWindowFlag = 1u << 31;
constexpr u32 ReadConstDynamicWindowDwords = 512;
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
