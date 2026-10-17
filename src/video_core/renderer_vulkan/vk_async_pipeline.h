// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: background creation of graphics pipelines. The idea and the bounded-queue design come
// from FelipePrado0/gow3_pc (vk_async_compiler.h, GPL-2.0-or-later), reduced to what this build
// needs: only the driver-side pipeline creation (vkCreateGraphicsPipelines) runs on the workers;
// shader translation, which reads guest memory, stays on the GPU thread.

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/thread.h"

namespace Vulkan {

/// SOTC_ASYNC_PIPELINES=0 turns all background compilation off (every new shader and pipeline is
/// built on the GPU thread, as upstream does: nothing skipped, but a hitch per new pipeline).
inline bool AsyncPipelinesEnabled() {
    static const bool enabled = [] {
        if (const char* v = std::getenv("SOTC_ASYNC_PIPELINES")) {
            return std::atoi(v) != 0;
        }
        return true;
    }();
    return enabled;
}

/// SOTC_ASYNC_COMPUTE=0: compute pipelines are built on the GPU thread again.
inline bool AsyncComputeEnabled() {
    static const bool enabled = [] {
        if (const char* v = std::getenv("SOTC_ASYNC_COMPUTE")) {
            return std::atoi(v) != 0;
        }
        return true;
    }();
    return enabled;
}

/// SOTC_ASYNC_SHADERS=0: SPIR-V of fragment and compute shaders is emitted on the GPU thread again.
inline bool AsyncShadersEnabled() {
    static const bool enabled = [] {
        if (const char* v = std::getenv("SOTC_ASYNC_SHADERS")) {
            return std::atoi(v) != 0;
        }
        return true;
    }();
    return enabled;
}

/// Worker count: SOTC_ASYNC_PIPELINE_THREADS, otherwise a quarter of the hardware threads,
/// between 1 and 4 (the GPU and guest threads need the rest).
inline unsigned AsyncPipelineThreadCount() {
    if (const char* v = std::getenv("SOTC_ASYNC_PIPELINE_THREADS")) {
        return std::clamp(static_cast<unsigned>(std::atoi(v)), 1u, 16u);
    }
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(cores / 4, 1u, 4u);
}

/// Low-priority worker threads with a bounded queue. The destructor drops tasks that have not
/// started and waits for the running ones.
class AsyncPipelineCompiler {
public:
    explicit AsyncPipelineCompiler(unsigned count, std::size_t capacity_ = 128)
        : capacity{capacity_} {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([this, i] {
                Common::SetCurrentThreadName(("sotc:PipelineCompile" + std::to_string(i)).c_str());
                Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock{mutex};
                        cv.wait(lock, [&] { return stopping || !tasks.empty(); });
                        if (stopping) {
                            return;
                        }
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    task();
                }
            });
        }
    }

    ~AsyncPipelineCompiler() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
            tasks.clear();
        }
        cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    AsyncPipelineCompiler(const AsyncPipelineCompiler&) = delete;
    AsyncPipelineCompiler& operator=(const AsyncPipelineCompiler&) = delete;

    /// False when the queue is full; the caller tries again on a later draw.
    bool Submit(std::function<void()> task) {
        {
            std::scoped_lock lock{mutex};
            if (stopping || tasks.size() >= capacity) {
                return false;
            }
            tasks.push_back(std::move(task));
        }
        cv.notify_one();
        return true;
    }

private:
    std::size_t capacity;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::vector<std::thread> workers;
};

} // namespace Vulkan
