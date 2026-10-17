# Background shader and pipeline compilation (SotC build)

Shader compilation that would stop the GPU thread is moved to low-priority worker threads. While
something is being built, the draws or compute dispatches that need it are skipped, so the frame
does not wait: an object or effect can appear a few frames late the first time it is seen,
instead of the game freezing for tens of milliseconds.

Everything finished goes into the same disk cache as before, so with the pipeline cache enabled
the next session loads it at start-up and nothing is skipped for it.

Idea and queue design adapted from FelipePrado0/gow3_pc (GPL-2.0-or-later).

## What runs in the background

| Part | When | Switch (environment variable, `0` = off) |
|---|---|---|
| Graphics pipeline creation (driver compile) | driver supports dynamic vertex input (current NVIDIA and AMD); no tessellation or geometry stage | `SOTC_ASYNC_PIPELINES` (also turns off everything below) |
| Compute pipeline creation | always | `SOTC_ASYNC_COMPUTE` |
| SPIR-V emission and shader module creation | fragment and compute shaders | `SOTC_ASYNC_SHADERS` |

The GCN-to-IR translation of a new shader still runs on the GPU thread, because it reads guest
memory. Vertex, tessellation and geometry shaders are emitted on the GPU thread too, for the same
reason (fetch shader and copy shader live in guest memory).

`SOTC_ASYNC_PIPELINE_THREADS` sets the number of worker threads (default: a quarter of the CPU
threads, 1 to 4).

## Checking that it is on

The log says at start-up, for example:
`Background compilation on (2 threads): graphics pipelines on, compute pipelines on, SPIR-V on`.
Each pipeline built this way is logged as `Compiling ... pipeline ... in the background`.

The pipeline cache must be enabled (`Vulkan PipelineCacheEnabled: true` in the log), otherwise
everything is compiled again in every session.

## Known trade-off

A draw or dispatch that happens only once (for example a texture rendered once while loading) is
lost if what it needs is not ready yet. Skipping a compute dispatch can leave wrong data for a
while longer than a skipped draw. If something looks wrong:

1. `SOTC_ASYNC_COMPUTE=0` first, then `SOTC_ASYNC_SHADERS=0`, to find which part causes it;
2. or play once with `SOTC_ASYNC_PIPELINES=0` to fill the cache, then turn it back on.
