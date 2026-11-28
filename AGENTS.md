# Notes for coding agents

**hdslave Necronomicon** is a Subharmonicon-style synth: two oscillators with subharmonics, two 4-step sequencers and four rhythm generators: a CLAP plug-in for the Akai MPC Sample. It is made to run in hdslaveOS, the hdslave fork of Kerem Kiliç's Melodee. Its CLAP id is `com.hdslave.necronomicon`, and it builds to `build/Necronomicon.clap`.

## Read the workspace guide first

On the author's machine, this repository sits in one folder beside the other hdslave projects. That folder's `../AGENTS.md` and `../Workflow/` hold the shared workflow:
- building in the Linux VM;
- what hdslaveOS does to plug-ins;
- screen pages;
- reviews;
- publishing, the hub repository and staging for the SD card.

Read `../AGENTS.md`, then `../Workflow/plugins.md` and `../Workflow/host-facts.md`, before you change anything. The rules below hold either way.

## Build and test

- `./build.sh` builds the plug-in in a Linux arm64 container (image `melodee-builder`, Colima VM `melodee`). It runs `make all test`: the host tests in `tests/host.cpp` and the sound's fingerprint. Pass make targets to run others, for example `./build.sh sanitize` (ASan and UBSan over the host tests).
- `tools/bench.sh` counts the instructions of a 128-frame block with callgrind. A block is about 4.8 M cycles on one of the MPC's Cortex-A55 cores.
- Check the build's own exit code: `./build.sh | grep …` hides a failure.

## Rules

- **Fixed for saved projects.** Parameter ids, preset keys and choice orders never change once released, because saved projects and presets refer to them.
  - Add new ones at the end.
  - Ranges stay as they are too: the host stores bindings normalized to the range.
- **The sound is held to the bit** by `tests/fingerprint.txt`.
  - A refactor or speed-up must leave every hash unchanged. With `-ffp-contract=fast`, even reshaping a loop can change the last bit, so check after every edit.
  - Only a change meant to change the sound runs `make fingerprint-update`, and its commit says which hashes moved and why.
- **Real-time and determinism.** The output is the same at every block size. No allocation, locks or file I/O on the audio thread.
- **Notes** must survive how hdslaveOS sends them:
  - every MIDI event of a block arrives at frame 0;
  - a re-pressed key arrives as on, on, off;
  - CC 120 and 123 arrive at 0 on every channel when the host silences.
- **Commits** are made as `hdslave <111106843+hdslave@users.noreply.github.com>`, with Conventional Commits. Never push, publish or stage for the card without the owner's word.
- **Other agents' work.** Others may be working here. Check `git status` and `git worktree list`, and never edit, commit or reset files you did not change.
- **Names and art.** No trademarked names in the plug-in's names, presets, UI text or art. Art is original; fonts are OFL, with their licences in `licenses/`.
- **Read the README** before you change the sound. It lists the sources, the inferred values and the deliberate deviations. Keep it current: version, costs, and "not yet run on the MPC" until it has been.

## This plug-in

- `tests/unit.cpp` checks the engine directly:
  - the rhythm counting, against brute force;
  - the scales;
  - the phase lock of the subs;
  - an FFT aliasing check below −45 dB for every output.
- Subharmonics count oscillator cycles. A new divisor takes effect at the next wrap of the oscillator.
- When Seq Oct changes, the step text is rescanned on the main thread through `request_callback`.
- **Pads.** While Run is Run (playing or stopped), notes 36-51 are steps: 36-39 Seq 1, 40-43 Seq 2, 44-51 the same again. `Engine::shown[]` shows the step until that sequencer's next clock step, Reset, Next or a Play or jump (a pad arriving in Play's own block holds until the next clock), without moving `step[]`, and starts both EGs as Trigger does. Pads never join the held-key list. Other notes, and every note with Run off, transpose and gate as before. `testStepPads` in `tests/host.cpp` holds it. CC 120 and a wildcard choke are `Engine::panic()` (silence at once, Hold drone included; while playing in EG: On the next sequencer step sounds again — the clocks are deliberately not gated on `silenced`); CC 123 only ends held notes.
- **Parameter count 58 (58 % 3 = 1), accepted.** Fine Tune sits alone on the last Edit row. The verified "append two parameters" remedy (V6) was overruled by the master plan (M-23: document, never pad): the count lands on a multiple of three only when real parameters are appended, with per-sequencer lengths the candidate pair. Never renumber or add filler to fix the row.
- **Pitch settle on load while active is deferred (V9).** A state or preset loaded over a sounding voice eases the pitch over the 8-frame ticks (about 1 ms) instead of snapping, and lands at the next clock, note or pad. Deferred with the fleet's settle-on-load intent decision (M-18: the ease may be anti-zipper); do not snap it as a drive-by fix.

## Release packaging

Every release ZIP includes the full repository `LICENSE.txt` beside the third-party notices, outside signed plug-in bundles. Preserve both when packaging the plug-in for another project.
