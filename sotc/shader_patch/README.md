# Shadow of the Colossus (CUSA08809) shader patches

`fs_0x00000000291dbcd0_0.spv`: post-process composite (per-object motion blur + DOF + bloom).
- The blur weight (alpha of the half-resolution motion-blur image, `%169`) is multiplied by 0.5.
- The colour entering the tone curve (`%338/%341/%346`, after the `data[37..39]` scale) is
  multiplied by 0.8 (about -1/3 EV): the sky and sun glare in the open world clipped to white.
The game scales motion blur by per-frame motion, so at the emulator's ~16 fps (vs 30 on PS4) the
trails were twice as long and showed as a "ghost" following Wander.

Install: copy the `.spv` to `user/shader/patch/` and enable `GPU.patch_shaders` for CUSA08809.
The SPIR-V comes from this branch's recompiler, so it must be regenerated whenever SPIR-V
generation changes (e.g. after merging upstream): enable `GPU.dump_shaders` (and disable the
pipeline cache so the shader is recompiled), reach gameplay, then run
`python make_patch.py user/shader/dump/fs_0x00000000291dbcd0_0.spv fs_0x00000000291dbcd0_0.spv [blur] [exposure]`
(defaults 0.5 and 0.8). The script finds the edits by instruction pattern.
