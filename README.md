# Necronomicon

A polyrhythmic subharmonic synthesizer as a CLAP plug-in, built for the Akai MPC Sample running Melodee (64-bit ARM
Linux, Cortex-A55). Hosts list it as **hdslave Necronomicon** (id `com.hdslave.necronomicon`). It follows the front panel
and normalled signal flow of the Moog Subharmonicon, without the patch bay: two VCOs, each with two subharmonic
oscillators that divide its frequency by 1 to 16, a ladder filter, two AD envelopes, two 4-step sequencers, and four
rhythm generators that clock the sequencers in polyrhythms.

The rhythm generators divide the host's clock: they play while the host plays, stop when it stops, and line up with
the song's place when it starts or jumps.

## Signal flow

```
VCO 1 ──► SUB 1, SUB 2 (VCO 1's frequency / 1 … 16, locked to its phase)     VCO 2 ──► its SUB 1, SUB 2
VCO 1, its subs, VCO 2, its subs ──► mixer (six levels; overdrives when hot)
  ──► 4-pole ladder low pass (VCF EG moves the cutoff) ──► VCA (VCA EG) ──► Volume ──► out (stereo)

host clock (Clock Div) ──► RHYTHM 1 … 4 (every 1st … 16th clock) ──OR / XOR──► SEQ 1 and / or SEQ 2 (4 steps each)
SEQ 1 ──► VCO 1's pitch and / or its subs' divisors        SEQ 2 ──► the same for VCO 2
every step of either sequencer ──► both EGs (EG: On)
MIDI note ──► both VCOs' pitch (from middle C) and a gate on both EGs
```

A subharmonic counts its VCO's cycles, so it stays locked to it in phase whatever the VCO does: VCO 1 at C and its
subs at 1/3 and 1/5 are a C with the F and the A-flat of the subharmonic series below it, exactly in tune.

## Controls

The panel's names, in the order Melodee's Edit page lists them (three to a page).

| Necronomicon | Panel | Range |
|---|---|---|
| VCO 1 Freq, VCO 2 Freq | VCO 1 FREQ, VCO 2 FREQ | Middle C (261.6 Hz) to the top C (4,186 Hz), 4 octaves |
| VCO 1 Sub 1, VCO 1 Sub 2, VCO 2 Sub 1, VCO 2 Sub 2 | SUB 1 FREQ, SUB 2 FREQ (each VCO) | 1/1 … 1/16 of the VCO's frequency |
| VCO 1 Level … VCO 2 Sub 2 Level | the six mixer levels | 0 – 100 %; together they overdrive when hot |
| VCO 1 Wave, VCO 2 Wave | VCO 1 WAVE, VCO 2 WAVE | **Square**: the VCO and its subs square. **Pulse/Saw**: the VCO a pulse whose width its SUB 1's sawtooth sweeps (the panel's normalled PWM), the subs sawtooth. **Saw**: all three sawtooth |
| Quantize | QUANTIZE | Off / 12-ET / 8-ET / 12-JI / 8-JI: the VCOs' pitch (knob, step and note together) to the nearest note of a scale on C |
| Cutoff | CUTOFF | 20 Hz – 20 kHz |
| Resonance | RESONANCE | 0 – 100 %; self-oscillates from about 90 % |
| VCF EG Amt | VCF EG AMT | ±100 %: the VCF EG moves the cutoff up to 8 octaves up or down |
| VCF Attack, VCA Attack | VCF ATTACK, VCA ATTACK | 1 ms – 10 s |
| VCF Decay, VCA Decay | VCF DECAY, VCA DECAY | 5 ms – 10 s (time to fall 60 dB) |
| EG | EG button | **Off**: the steps start nothing (Trigger and notes still do). **On**: every step of either sequencer starts both EGs. **Hold**: both EGs rise to the top and stay there (a drone); leaving Hold, they decay |
| Volume | VOLUME | 0 – 100 % |
| Seq 1 Step 1–4, Seq 2 Step 1–4 | STEP 1–4 (each sequencer) | ± Seq Oct: centre is no change; the text reads in semitones at the Seq Oct setting |
| Seq Oct | SEQ OCT | ±1 / ±2 / ±5 octaves, for both sequencers |
| Seq 1 > Osc 1, Seq 1 > Sub 1, Seq 1 > Sub 2 | SEQ 1 ASSIGN: OSC 1, SUB 1, SUB 2 | Off / On, any together. On **Osc**, the step moves the VCO's pitch (its subs follow it). On a **Sub**, the step moves that sub's divisor instead: 3 divisors for each octave of step, up a smaller divisor (a higher note) |
| Seq 2 > Osc 2, Seq 2 > Sub 1, Seq 2 > Sub 2 | SEQ 2 ASSIGN | the same for VCO 2 and its subs |
| Rhythm 1–4 | RHYTHM 1–4 | 1 – 16: the generator fires on every n-th clock |
| Rhythm 1 > Seq 1 … Rhythm 4 > Seq 2 | each RHYTHM's SEQ 1 and SEQ 2 buttons | Off / On: the sequencers it moves |
| Rhythm Logic | Rhythm Generator Logic (a MIDI setting on the panel) | **OR**: a sequencer moves when any of its generators fires. **XOR**: only when exactly one does (two together cancel) |
| Clock Div | TEMPO (replaced) | 1/2 … 1/64 of a beat, with triplets: the clock the generators divide (1/16 is four a beat, the panel's own clock under MIDI clock) |
| Run | PLAY | Stop / Run: with Run off the host plays but the sequencers hold and nothing fires |
| Reset | RESET | both sequencers to step 1 and the generators to their start: the next clock fires every routed generator and plays step 1 |
| Next | NEXT | both sequencers on one step, without the EGs |
| Trigger | TRIGGER | both EGs at once, without moving the sequencers (not during an attack; nothing in EG: Hold) |
| Fine Tune | OSC 1 + OSC 2 held, TEMPO turned | ±50 cents on both VCOs |

A MIDI note (a pad or key on a Melodee plug-in track) moves both VCOs by its distance from middle C (note 60), as the
panel's VCO 1 input does (normalled to VCO 2), and holds both EGs at the top while it is down. The pitch stays where
the last note put it, so playing pads transposes the running pattern; with EG: Off and the transport stopped, the
pads play it like a monophonic keyboard.

## Presets

Six factory presets, inside the plug-in: Melodee lists them on the plug-in's PRESETS page (any host with CLAP preset
discovery does). A preset sets every control, the sequencers and the rhythm generators too; Default puts everything
back.

| Preset | What it is |
|---|---|
| Default | VCO 1 on C with an octave sub, VCO 2 on G with a sub two octaves down; Seq 1 on every 4th sixteenth, Seq 2 on every 3rd |
| Subharmonic Chords | 12-JI, each VCO's subs a few subharmonics below it, slow envelopes, the sequencers walking chord tones on 8 and 6 |
| Polyrhythm Bass | Low saws in 3 against 4, a third generator on 7 into both sequencers, Seq 2 moving VCO 2's sub divisor |
| PWM Pad | Both VCOs in Pulse/Saw (their widths swept by their SUB 1s), long attacks, changes on 16 and 12 |
| Sub Arp | The sequencers on the subs' divisors only: a subharmonic arpeggio, three generators under XOR |
| Hold Drone | EG: Hold, just intonation, a resonant filter, the sequencers retuning the drone on slow rhythms |

They are the patches the demo renders play: the tests load each preset and check it gives the same values and the
same samples as the demo's list.

## Clocking from the host

- A clock falls on every multiple of Clock Div counted from the song's top, while the host plays. Rhythm n fires on
  every n-th clock from the top, so all four fire together on the song's first clock and line up again on their
  common multiples. Each sequencer moves a step when its generators fire (by the logic), and the first time after a
  Play, a jump or Reset it plays the step it is on.
- Play from the top starts on its first frame; each later clock lands on its frame. A Play or a jump places both
  sequencers by the song's place: the steps the generators would have reached from the top.
- Stop stops the clock (sounds ring out; EG: Hold keeps droning). A jump, or a loop going round, lines everything up
  with the song's place again. A wrap that falls inside a block still plays that clock, at the start of the next block
  (at most a block late: 2.7 ms at 128 frames).
- Run off: the host plays but the sequencers hold their steps and nothing fires; Next, Reset and Trigger work as on
  the panel. Reset while playing restarts the generators from the next clock, until the next Play or jump lines them
  up with the song again.
- In Melodee, keep the plug-in's **TRANSPORT** on (the default): with it off the plug-in never hears Play.

## Where values were inferred

The Subharmonicon's manual documents the routing and several ranges (VCO 4 octaves from middle C, divisors 1–16,
cutoff 20 Hz – 20 kHz, attacks 1 ms – 10 s, decays 5 ms – 10 s, SEQ OCT ±1/±2/±5, rhythms 1–16, the OR / XOR logic,
the three WAVE positions, a trigger ignored during an attack, fine tune ±50 cents). It does not give these, so they
are chosen to match what is documented:

- VCF EG depth: ±8 octaves at full turn.
- Resonance: the ladder self-oscillates from about 90 % of the turn (the manual: at its maximum).
- Pulse/Saw: SUB 1's sawtooth sweeps the pulse between 5 % and 95 % (the PWM input's range is 1 – 99 %).
- A step on a sub moves its divisor by 3 for each octave the step is worth (the manual says only that it changes the
  divisor). A new divisor takes over at the VCO's next cycle.
- The scales: 8-ET and 8-JI are the eight notes of a major scale (8-JI's ratios are the manual's just C major:
  1/1 9/8 5/4 4/3 3/2 5/3 15/8 2/1); 12-JI is the 5-limit chromatic scale (1/1 16/15 9/8 6/5 5/4 4/3 45/32 3/2 8/5
  5/3 16/9 15/8). All on C. The knob, the step and a note are quantized together, then Fine Tune.
- XOR with three or four generators on a sequencer: it moves when exactly one fires. The logic is the same for both
  sequencers, each with its own generators.
- A trigger during a decay starts the attack from where the envelope is.
- The mixer's overdrive: one soft-clipping stage after the six levels (squared tapers).

Deviations: Trigger is a trigger in every EG mode (a plug-in button cannot be held, so EG: Off's gate is left to MIDI
notes, which hold the EGs as a gate does); notes carry no velocity, as on the panel.

Left out on purpose: the patch bay, the external clock, MIDI CCs, the PWM input and the SEQ OCT / QUANTIZE output
settings (there are no outputs), and the panel's TEMPO (the clock is the host's; 1/64 is the fastest).

## Cost

One voice, run at twice the host's rate (so the oscillators' edges and the ladder stay clean) and filtered back
down with a half-band FIR. All six oscillators' edges are band-limited with polynomial BLEP: away from the
harmonics, each output stays 45 dB or more below them at 1 and 3 kHz (`tests/unit.cpp` checks it). Instructions a
128-frame block takes, from `tools/bench.sh` (callgrind):

| Case | Instructions | Share of one A55 core at 1 instruction a cycle |
|---|---|---|
| Everything busy (all six oscillators sounding, the pulse width swept, a high VCO, the sequencers on the subs, resonance, EG: Hold) | about 90,000 | 1.9 % |
| The default pattern with a shorter decay | about 41,000 | 0.9 % |
| Stopped | under 600 | 0.01 % |

While the VCA is closed between hits the oscillators and filter are skipped. The real share on the MPC depends on
how many instructions the A55 retires a cycle on this code; measure it with Melodee's plug-in load figure.

## Build and test

Needs Docker with a Linux arm64 context (Melodee's `colima-melodee` and its `melodee-builder` image by default).

```sh
./build.sh          # build/Necronomicon.clap, then the tests, the sound's fingerprint and demo renders build/demo-*.wav
./build.sh bench    # build/bench
tools/bench.sh      # instruction counts (Melodee's melodee-profiler image)
```

`tests/unit.cpp` checks the engine below the CLAP API: the rhythm generators' counting by common multiples against
counting clock by clock (3,000 random routings, both logics), the scales, the subs locked to their VCO through
random pitch changes and divisor changes, and every output's aliasing.

`tests/host.cpp` loads the plug-in through the CLAP API and checks: description and ports; every parameter's range,
text and default (the steps' text following Seq Oct, the host asked to read it again); state save and load; the
presets as preset discovery lists them and preset-load loads them; clock timing to the frame at 44.1, 48 and 96 kHz
and two tempos; 3 against 4 across the sequencers, OR and XOR on one; a generator routed nowhere, Run off; stop,
restart, jumps, a loop wrapping inside a block, a host without a song position, the steps placed by the song's place;
the same output whatever the block size (1, 37, 128, 4096 frames); the VCO range, each quantizer, Fine Tune; Next,
Reset, the steps on the VCO and on a sub's divisor, a sub following its VCO, Seq 2 on VCO 2, each wave's pitch;
EG Off with Trigger, EG Hold and back, notes (CLAP, MIDI, a choke) gating the EGs and moving the pitch, Reset while
playing; and finite, bounded output at 60 random and extreme settings.

`tests/fingerprint.cpp` then renders 45 fixed scenarios (each preset, the bench's patches, 24 random patches with the
buttons, notes and knobs moving, other block sizes and rates, stop and jumps, each quantizer and wave, notes while it
plays, XOR switched while it plays) and hashes every output sample's bits against `tests/fingerprint.txt`: work
meant to keep the sound, such as a speed-up, must leave every hash as it is. A change meant to change the sound
updates them with `make fingerprint-update` (run inside the build image). The hashes hold for Melodee's build image
and these compiler flags.

## Install

Copy `build/Necronomicon.clap` to `Melodee/Plugins/hdslave Necronomicon/hdslave Necronomicon.clap` on the card (or the staged
card's folder), then pick it on an empty track: Stage, Plugin, hdslave Necronomicon. It needs only libc and libm, glibc 2.35 or
newer (the MPC has 2.39); the build is 199 KB.
