# OpenBFME Apple Silicon runtime source variants

This fork preserves the Godot source variants used while investigating OpenBFME
on Apple Silicon. The base is Godot 4.7.2-stable, commit
`ed1daf0bf001b61586d9930840f2f1394092c079`.

The OpenBFME integration branch is
[codex/macos-base-debug](https://github.com/vl-bbnn/openbfme-godot/tree/codex/macos-base-debug).
Each branch below preserves its original source base and exact changed file
bytes. Existing commits were retained; previously uncommitted variants were
recorded separately. The original working copies and their build provenance
were preserved during publication.

Publication is source preservation, not release qualification. Branches are
alternatives, not an ordered series to merge together. Earlier failed or
superseded experiments are retained for diagnosis. No Godot upstream pull
request or production runtime update is implied.

## Scope and limits

- Sampler/binding and shared texture accounting changes have scoped M1 checks;
  they do not establish compatibility or performance on every Metal device.
- GPU timing records completed command-buffer intervals; stage-timing V5 is an
  optional bounded collector. Neither is automatically GPU frame time,
  utilization, an FPS improvement or a total unified-memory measurement.
- Stage-timing V1 through V4 are archived development attempts, not qualified
  replacements for V5. Their original failures were not erased.
- Pipeline-owner, terrain-mode and Bib graphics variants are diagnostic controls.
  They do not change the acceptance standard for rendered pixels or retail
  parity. Mobile and clustered renderer controls serve different paths.
- Fence observers record waits and command-buffer metadata. They do not fix or
  close the historical Metal fence timeout.
- Main-loop/macOS span instrumentation and editor documentation/crash diagnostics
  are separate tooling variants, not simulation behavior changes.

No game assets, saves, application bundles, debug symbols, local reports or
captures are included in these branches. Source publication did not rebuild
these runtimes, relabel existing binaries, change active/rollback applications
or rerun all device qualification matrices.

## Branch index

The machine-readable manifest is
[`misc/openbfme-runtime-variants.json`](misc/openbfme-runtime-variants.json).
Its hashes cover the changed source files, not generated build outputs.

| Variant | Branch | Source commit |
|---|---|---|
| bib-graphics-terrain-source-20261027 | [codex/openbfme-bib-graphics-trace](https://github.com/vl-bbnn/godot/tree/codex/openbfme-bib-graphics-trace) | `e4d0ddef5b35` |
| buffer-cache-source | [codex/openbfme-metal-buffer-cache](https://github.com/vl-bbnn/godot/tree/codex/openbfme-metal-buffer-cache) | `1a0909ddb692` |
| cache-identity-source | [codex/openbfme-metal-sampler](https://github.com/vl-bbnn/godot/tree/codex/openbfme-metal-sampler) | `77f33c702024` |
| clean-metal-source | [codex/openbfme-metal-binding](https://github.com/vl-bbnn/godot/tree/codex/openbfme-metal-binding) | `33d10a33f187` |
| construction-trace-source | [codex/openbfme-construction-trace](https://github.com/vl-bbnn/godot/tree/codex/openbfme-construction-trace) | `a76cbd096d1e` |
| current-fence-observer-source-20261027 | [codex/openbfme-fence-observer](https://github.com/vl-bbnn/godot/tree/codex/openbfme-fence-observer) | `3b46d2ba536e` |
| editor-doc-source | [codex/openbfme-editor-doc](https://github.com/vl-bbnn/godot/tree/codex/openbfme-editor-doc) | `55b2079073b6` |
| fence-trace-source | [codex/openbfme-fence-trace](https://github.com/vl-bbnn/godot/tree/codex/openbfme-fence-trace) | `53a64455838e` |
| firstuse-pipeline-owner-source | [codex/openbfme-pipeline-owner](https://github.com/vl-bbnn/godot/tree/codex/openbfme-pipeline-owner) | `58e5535d4966` |
| firstuse-pipeline-owner-v3-source | [codex/openbfme-pipeline-owner-v3](https://github.com/vl-bbnn/godot/tree/codex/openbfme-pipeline-owner-v3) | `d9c542672780` |
| gpu-timing-source | [codex/openbfme-gpu-timing](https://github.com/vl-bbnn/godot/tree/codex/openbfme-gpu-timing) | `9ab7f3079fa1` |
| residual-frame-source | [codex/openbfme-frame-spans](https://github.com/vl-bbnn/godot/tree/codex/openbfme-frame-spans) | `c5b274250437` |
| source | [codex/openbfme-pipeline-trace](https://github.com/vl-bbnn/godot/tree/codex/openbfme-pipeline-trace) | `b58a5094f45a` |
| stage-timing-source-20261025 | [codex/openbfme-stage-timing-v1](https://github.com/vl-bbnn/godot/tree/codex/openbfme-stage-timing-v1) | `73d4ff65227b` |
| stage-timing-source-v2-20261025 | [codex/openbfme-stage-timing-v2](https://github.com/vl-bbnn/godot/tree/codex/openbfme-stage-timing-v2) | `f77e054ac808` |
| stage-timing-source-v3-20261025 | [codex/openbfme-stage-timing-v3](https://github.com/vl-bbnn/godot/tree/codex/openbfme-stage-timing-v3) | `57e7cc632b04` |
| stage-timing-source-v4-20261025 | [codex/openbfme-stage-timing-v4](https://github.com/vl-bbnn/godot/tree/codex/openbfme-stage-timing-v4) | `f4887f81a0d0` |
| stage-timing-source-v5-20261025 | [codex/openbfme-stage-timing-v5](https://github.com/vl-bbnn/godot/tree/codex/openbfme-stage-timing-v5) | `5bb50a00a434` |
| terrain-pipeline-mode-source-20261026 | [codex/openbfme-terrain-mobile-control](https://github.com/vl-bbnn/godot/tree/codex/openbfme-terrain-mobile-control) | `71789f893ac6` |
| terrain-pipeline-mode-v2-source-20261026 | [codex/openbfme-terrain-clustered-control](https://github.com/vl-bbnn/godot/tree/codex/openbfme-terrain-clustered-control) | `993f4d428510` |
| texture-accounting-source | [codex/openbfme-texture-accounting](https://github.com/vl-bbnn/godot/tree/codex/openbfme-texture-accounting) | `7eb933eb2700` |
