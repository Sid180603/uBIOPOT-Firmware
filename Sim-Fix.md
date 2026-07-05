Sure. The idea is to make the simulator **behave like a real multi-metal sample** instead of replaying one canned peak — and, importantly, to route that data through the **real detection code** so the sim actually tests the multi-peak pipeline, not just the screen layout.

Right now the sim does two artificial things (both in sim/main_sim.c):
1. generates **one** Gaussian (Pb at −400 mV), and
2. **skips** `peaks_find()` and hands the results screen a hardcoded `peaks[1]`.

The change swaps both for realistic behavior. Here's what each part means.

## Part 1 — Generate a curve with several peaks
A real voltammogram from a mixed sample is the **sum of one Gaussian per metal**, each centered at that metal's stripping potential, with a height proportional to its concentration:

$$I(E) = \sum_k A_k \cdot \exp\!\left(-\frac{(E-\mu_k)^2}{2\sigma^2}\right)$$

To make the peaks land in the right metal windows (from metal_id.c), you pick centers inside each window:

| Metal | Center μ (mV) | Window (mV) | Example height A (µA) |
|---|---|---|---|
| Cd | −700 | −800…−600 | 30 |
| Pb | −450 | −550…−400 | 50 ← tallest → "primary" |
| Cu | 0 | −100…+50 | 25 |
| Hg | +300 | +200…+400 | 18 |

With σ ≈ 60–80 mV and centers spaced 300–450 mV apart, the four bumps are **well separated**, so the curve dips clearly between them. The sim already sweeps −1000 → +990 mV, so all four fit on the axis.

Instead of the current single-Gaussian line, the generator becomes a small loop summing those four terms at each `E`.

## Part 2 — Run the *real* finder instead of hardcoding
This is the key part. At scan end, replace the hardcoded `peaks[1]` with a call to the actual firmware function:
```c
peak_t peaks[4];
uint16_t n = peaks_find(s_I_buf, s_E_buf, SIM_SCAN_POINTS,
                        peaks, 4, 1.0f /* µA prominence */);
scr_results_set(peaks, n, 1);
```
Now the sim exercises the **same code the device runs**.

## What `peaks_find` does to that curve (why you'll get 4)
Walking through peaks.c on the 4-Gaussian buffer:
1. **Scan for local maxima** — every point higher than both neighbors. Each Gaussian crest qualifies → 4 candidates.
2. **Prominence filter (≥ 1.0 µA)** — for each crest, measure how far it rises above the valley floor to its taller neighbor. Because the curve dips well below each crest (peaks are separated), all four have prominence far above 1 µA → all kept.
3. **Sort tallest-first** — Pb (50) → Cd (30) → Cu (25) → Hg (18).
4. Returns `n = 4`.

Then scr_results.c runs `metal_identify()` on each peak's potential and renders:
- **Primary (big):** `Pb / Lead / +50.0 µA / −0.450 V / WHO: 10 µg/L [Not calibrated]`
- **Secondary list:** `P2: +30.0 µA @ −0.700 V Cd`, `P3: +25.0 µA @ 0.000 V Cu`, `P4: +18.0 µA @ +0.300 V Hg`
- **Mini voltammogram:** the full 4-bump curve.

And the live scan screen draws the whole multi-peak trace as it sweeps.

## Why this is worth doing
- It turns the sim from a **layout preview** into a **real test of the detection + metal-ID + multi-peak results layout** — the exact logic you were worried about ("why only one peak?").
- It's **sim-only** — it changes main_sim.c (a PC stub) and touches **no firmware logic**. peaks.c, metal_id.c, scr_results.c are used *as-is*, which is the point: you're testing them, not modifying them.
- Optional realism knobs: add a small sloping **baseline** and light **noise** to mimic a real curve, or tie the four heights to a "concentration" variable so you can watch peaks grow/shrink.

## What it still won't test (honest boundary)
It still uses the **stub engine** (an `lv_timer` replaying computed points), not the real acquisition task/HAL/DAC/ADC. So it validates **UI + peak detection + metal-ID**, but not timing, drivers, or real analog — those remain for Wokwi (real firmware) and the bench.

---

Want me to make this change to main_sim.c now — 4 Gaussians (Cd/Pb/Cu/Hg) summed into the buffer, routed through the real `peaks_find()`, with an optional baseline+noise toggle? It's contained to the sim and gives you a proper multi-peak UI to iterate on.

---
Verification of the Proposal

The proposal is technically sound with one critical prerequisite it doesn't mention. Here's the breakdown:

What's correct

Part 1 — Multi-Gaussian generation: The math is right. Summing 4 Gaussians with centers inside the metal windows from metal_id.c will produce a realistic multi-peak voltammogram. The chosen centers (Cd @ -700, Pb @ -450, Cu @ 0, Hg @ +300) all fall within their respective windows. The σ=60-80 mV with 300+ mV spacing means negligible overlap — each peak drops below 1% of its height before the next begins.

Part 2 — peaks_find behavior: I verified peaks.c line by line. The claim about what it does is accurate:
1. Local maxima — strictly greater than both neighbors (line 69-70). Each Gaussian crest qualifies.
2. Prominence filter — the prominence algorithm (lines 17-38) measures rise above the highest valley floor. With well-separated Gaussians that drop to ~0 between peaks, all four will have prominence equal to roughly their full height (30-50 µA) — far above the 1.0 µA threshold.
3. Sort tallest-first — sort_peaks_descending (lines 44-55) sorts by I_uA descending. So output order will be: Pb(50) → Cd(30) → Cu(25) → Hg(18). Correct.
4. Returns n=4 since max_peaks=4.

Part 2 — metal_identify behavior: I verified metal_id.c. It does a linear scan of the profile table and returns the first window match. The peak potentials from peaks_find (which are the E values at each Gaussian crest) will map to the correct metals since each center was chosen inside its window.

The results screen rendering claim is also correct — scr_results.c:252-292 shows the primary peak (index 0) with large metal symbol + name + WHO info, and secondary peaks P2-P4 in a smaller list below.

The critical prerequisite it glosses over

The proposal says "peaks.c, metal_id.c, scr_results.c are used as-is" — but the sim currently doesn't compile peaks.c or metal_id.c.

Here's the situation:
- The WSL setup guide (Step 6) copies only the headers from echem_core/include/echem_core/*.h into the stubs directory.
- The source files (peaks.c, metal_id.c, scan_state.c) are never copied to the sim.
- The current sim works because main_sim.c only includes peaks.h for the peak_t typedef but never calls peaks_find() — it hardcodes the results. And metal_identify() is called from scr_results.c which IS compiled, but this means the sim already has a linker dependency on metal_id.c that must be satisfied somehow.

Wait — let me check if scr_results.c is actually compiled in the sim and whether it links.

Looking at the WSL setup: Step 5 copies scr_results.c into $SIM/src/screens/, and the CMake glob picks it up. scr_results.c calls metal_identify() at line 252 and 283. Those are defined in metal_id.c. If the sim compiles and links successfully today, then either:
1. metal_id.c is somehow getting compiled (maybe via the echem_core header copy that also copies .c files?), or
2. The sim hasn't been rebuilt since the results screen was updated to use metal_identify().

The Step 6 command is: cp "$REPO/components/echem_core/include/echem_core/"*.h "$SIM/src/stubs/echem_core/" — this copies only headers, not the .c files from components/echem_core/src/.

This means the sim will fail to link right now with undefined reference to 'metal_identify' if rebuilt. The results screen was updated to use metal_id after the sim was last built.

What needs to happen before the multi-Gaussian change

1. Copy the pure-C echem_core source files into the sim build:
cp "$REPO/components/echem_core/src/peaks.c"    "$SIM/src/screens/"
cp "$REPO/components/echem_core/src/metal_id.c"  "$SIM/src/screens/"
cp "$REPO/components/echem_core/src/scan_state.c" "$SIM/src/screens/"
1. (Putting them in src/screens/ is a shortcut since that directory is already globbed by CMake. A cleaner approach would be a separate src/echem/ directory with its own glob.)
2. Or add them to the WSL setup guide as a new step (Step 6b) and update the CMake patch to glob them.

The peaks.c and metal_id.c files are pure C with no IDF dependencies (they only include their own headers + <math.h> / <float.h>), so they compile cleanly on the host. scan_state.c is similarly pure. protocol.c references AQUAHMET_PROTOCOL_VERSION from protocol.h which is also pure. dpv.c, technique.c, and calibration.c may have IDF deps — you'd need to check, but you only need peaks.c, metal_id.c, and scan_state.c for this change.

The "honest boundary" section is accurate

The sim tests UI + peak detection + metal-ID but not the real HAL/DAC/ADC/FreeRTOS acquisition pipeline. That's a truthful statement.

One minor inaccuracy

The proposal says peaks_find will return exactly 4. That's true for the described inputs unless two Gaussian tails create a tiny secondary bump at a saddle point between two peaks. With σ=60-80 mV and 250+ mV spacing, this won't happen — the inter-peak values drop to effectively zero. But with added noise (the "optional realism knob"), a noise spike could create a spurious local maximum. The 1.0 µA prominence threshold would filter most of these, but very unlucky noise could produce a 5th candidate that gets dropped because max_peaks=4. Not wrong, just worth noting.


This is a sharp catch and worth verifying against the actual files rather than taking it at face value. Let me read the WSL setup guide to confirm exactly what gets copied into the sim build, and re-confirm the `metal_identify` call sites.   

Read [](file:///c%3A/Users/z00541ce/Documents/Coding/ST%20Thesis/uBIOPOT-Firmware/sim/WSL_SIM_SETUP.md#1-1), lines 1 to 200

Let me confirm exactly which `echem_core` functions the sim's compiled files actually call — that tells us precisely which `.c` files must be added to the build.   

Searched for regex `metal_identify|metal_concentration|metal_get_profiles|peaks_find|scan_state_next|scan_state_name` (`**/uBIOPOT-Firmware/components/ui_tft/src/*.c`), 3 results

I independently verified this against the files — **the critique is correct**, and it caught a real gap in my proposal. Here's my confirmation, plus one refinement where I can make it even more precise.

## Confirmed: there's a pre-existing latent link bug (independent of my change)
- **Step 5** of WSL_SIM_SETUP.md copies `scr_*.c` (so scr_results.c **is** compiled in the sim) and CMake globs `src/screens/*.c`.
- **Step 6** copies only `echem_core/include/echem_core/*.h` — **headers only**, never the `.c` sources.
- I confirmed the call sites: scr_results.c and scr_results.c both call `metal_identify()`, which is defined in metal_id.c.

So scr_results.c **compiles** (the header is present) but will **fail to link** with `undefined reference to metal_identify` the moment the sim is rebuilt against the current results screen. The "Build: 0 errors" in your recent commit was the **firmware** (`idf.py build`), not the WSL sim — so this hasn't surfaced yet. The critique's hypothesis #2 (results screen gained `metal_id` *after* the sim was last built) is the correct explanation.

**My earlier wording was imprecise.** "Used as-is" was true in the sense of *not modifying* those files — but I wrongly implied they were already in the sim's compile set. metal_id.c isn't, and peaks.c isn't either (harmless today only because nothing in the sim calls `peaks_find` yet — my multi-Gaussian change would be the first caller).

## One refinement to the critique (verified)
The critique lists `peaks.c, metal_id.c, and scan_state.c` as needed. I checked what the compiled sim files actually reference:
- scr_results.c → `metal_identify` → **needs metal_id.c** ✅
- my proposed main_sim.c → `peaks_find` → **needs peaks.c** ✅
- `scan_state.c` → **not needed.** main_sim.c only uses the `scan_state_t` **enum constants** (`SCAN_STATE_RUNNING/IDLE`), which come from the header — no function from `scan_state.c` is called by any compiled sim file (grep confirms no `scan_state_next`/`scan_state_name` calls).

Both required files are **pure C** (peaks.c: `<float.h>/<string.h>`; metal_id.c: `<math.h>`) — they compile cleanly on the host.

## The correct prerequisite before the multi-Gaussian change
Add the two pure-C sources to the sim build:
1. **peaks.c** and **metal_id.c** → into the sim's compiled set.
2. **Cleaner than the `src/screens/` shortcut:** create a `src/echem/` dir, copy them there, and add `file(GLOB ECHEM_SOURCES "${PROJECT_SOURCE_DIR}/src/echem/*.c")` to the CMake patch. Keeps electrochemistry separate from screens.
3. Update WSL_SIM_SETUP.md with the new copy step + CMake glob.

This **fixes the existing latent link bug** *and* enables the multi-peak change.

## Agreed on the "exactly 4" nuance
Correct: for the clean 4-Gaussian input, `peaks_find` returns exactly 4. **With the optional noise knob**, a spike could create a 5th local maximum — but the 1.0 µA prominence gate filters most, and `max_peaks=4` caps the rest, so it silently drops the smallest. Worth noting; not wrong. If we add noise, I'd bump `max_peaks` to, say, 6 in the sim call so a real small peak isn't crowded out by a noise artifact.

---

So the corrected plan is **two changes, in order**:
1. **Link fix (required regardless):** add peaks.c + metal_id.c to the sim build (`src/echem/` + CMake glob) and update the setup guide.
2. **Multi-Gaussian:** rewrite main_sim.c's generator to sum Cd/Pb/Cu/Hg Gaussians and route the buffer through the real `peaks_find()` → `scr_results_set()`.

Want me to implement both now (the `src/echem/` build fix + guide/CMake update, then the multi-Gaussian main_sim.c)? I'll keep it entirely within `sim/` and the setup guide — no firmware logic touched.




Two simple things that work together:

1. Raise the prominence threshold to 3–5 µA. The smallest real peak (Hg) is 18 µA — its prominence will be ~18 µA since the curve drops to near-zero on both sides. A 5 µA threshold gives a 3.6× margin over noise while never threatening a real peak. The 1.0 µA threshold in the proposal is copy-pasted from the firmware (where it's tuned for real electrochemistry with sub-µA features) — in the sim with ±1-2 µA synthetic noise, it's too permissive.

2. Don't add noise by default. Pure Gaussians are deterministic — you get exactly 4 peaks every time, which makes the sim predictable for visual iteration. Add noise as an opt-in #define SIM_ADD_NOISE 0 toggle. When it's off, even the 1.0 µA threshold works perfectly. When someone flips it on to stress-test the peak finder, the higher threshold catches the edge case.

You don't need to increase max_peaks — the issue isn't dropping a real peak, it's a noise spike sneaking in as a 5th candidate. With max_peaks=4, the 5th candidate would cause a real peak to get evicted from the sorted output. Raising the threshold prevents the spurious candidate from ever entering the list, which is the correct fix. Increasing max_peaks would just let the garbage through.


The analysis is mostly right, with one thing I'd push back on.

The scan_state.c refinement is correct. I over-included it in my earlier list. No sim-compiled file calls scan_state_next() — the UI layer only uses the scan_state_t enum constants from the header. Only peaks.c and metal_id.c need to be added.

The src/echem/ directory approach is clean. Better than dumping them into src/screens/ — keeps the conceptual boundary between UI and electrochemistry, and the CMake glob pattern is self-documenting.

The max_peaks=6 suggestion is the wrong fix for the noise problem. It conflicts with what I recommended earlier, and for good reason. Increasing max_peaks doesn't prevent the problem — it just changes who gets evicted. With max_peaks=6, you'd still collect noise-generated spurious peaks, and those would show up in the secondary peaks list on the results screen as P5: +2.1 µA @ -0.082 V ??. The user sees ghost metals.

The correct fix (which I already suggested) is raising the prominence threshold to 3-5 µA in the sim call. That prevents spurious noise peaks from ever entering the candidate list, while the smallest real peak (Hg at 18 µA) has ~18 µA prominence — a 3.6-6× safety margin. Keep max_peaks=4 in the sim. The firmware can use its own threshold tuned for real electrochemistry.

So the corrected plan is sound — go ahead with both changes. Fix the link issue (src/echem/ + CMake + guide update), then do the multi-Gaussian main_sim.c rewrite with peaks_find(..., 4, 5.0f) and noise off by default.


Checked against the actual peaks.c logic — and I **agree with the critique on all three points. I withdraw my `max_peaks=6` suggestion; it was wrong.** More importantly, when I re-read the finder carefully, I found the reasoning is even *stronger* than the critique stated. Here's the verification.

## The decisive detail I under-weighted: `peaks_find` collects **left-to-right, then stops at `max_peaks`**
Look at Pass 1 in peaks.c:
```c
for (uint16_t i = 1; i + 1 < n && found < max_peaks; i++) {
```
The loop halts **as soon as `found == max_peaks`**, scanning in **index (potential) order**, *then* Pass 2 sorts by height. So it keeps the **first N qualifying maxima by position**, not the N tallest.

This is exactly why raising `max_peaks` is the wrong lever:
- With a **low threshold + noise**, early noise spikes (at the negative-E end, scanned first) **consume slots**. With `max_peaks=4`, four early noise/real maxima can fill up **before the scan ever reaches Hg at +300 mV** → the **rightmost real peak is silently missed**. That's worse than a cosmetic ghost.
- Bumping to `max_peaks=6` doesn't fix this — with enough low-threshold noise you can *still* fill 6 slots before reaching Hg. It just changes *which* garbage gets in.

**Raising the prominence threshold is the root fix:** noise never qualifies, so Pass 1 only ever collects the 4 real peaks, and `max_peaks=4` is exactly right. The critique's conclusion is correct; the left-to-right-fill behavior is the clinching argument for it.

## Verifying the threshold numbers
- ±2 µA synthetic noise → noise local maxima have prominence up to ~peak-to-trough ≈ **3–4 µA**. So **5 µA** (not 3) is the safe floor — 3 µA is cutting it close against ±2 µA noise.
- Smallest real peak Hg = 18 µA, drops to ~0 on both sides → prominence ≈ **18 µA** → **3.6× margin** over a 5 µA gate. Never threatened. ✅

So `peaks_find(..., 4, 5.0f)` is correct.

## One small correction *to* the critique (doesn't change its conclusion)
The critique says a spurious peak "would show up in the secondary peaks list as `P5: +2.1 µA`." On the **current** results screen that particular symptom wouldn't appear — scr_results.c caps the secondary loop at `i < 4`, so it only ever renders P2/P3/P4; a 5th entry is never drawn. So the *visible* failure isn't a "P5 ghost" — it's the **missed real peak** (via slot-fill) I described above, which is actually a stronger reason to reject `max_peaks=6`. Same verdict, more accurate mechanism.

## Agreed on the rest
- **Noise off by default** via `#define SIM_ADD_NOISE 0` — correct. Deterministic 4-peak output for visual iteration; flip on only to stress-test the finder. ✅
- **`scan_state.c` not needed** (only enum constants used) — matches my grep. ✅
- **`src/echem/` directory** over dumping into `src/screens/` — cleaner UI/electrochemistry boundary, self-documenting glob. ✅

## Bonus observation (firmware, not blocking)
That left-to-right-fill behavior applies to the **real firmware** too: if a real scan had many small qualifying bumps early in the sweep, `peaks_find` could fill its 4 slots before reaching a later genuine peak. Not a problem now, but worth a note for P8/P9 tuning — the firmware's `1.0 µA` threshold + `MAX_PEAKS 4` assumes few early sub-threshold features. Flagging it, not fixing it now.

---

## My opinion: the corrected plan is sound — proceed
1. **Link fix:** add peaks.c + metal_id.c to the sim via a new `src/echem/` dir + CMake glob; update the setup guide. (Fixes the pre-existing `metal_identify` link bug too.)
2. **Multi-Gaussian main_sim.c:** sum Cd/Pb/Cu/Hg Gaussians → `peaks_find(buf, ..., 4, 5.0f)` → `scr_results_set`; **`SIM_ADD_NOISE 0`** by default with an opt-in toggle. Keep `max_peaks=4`.

Want me to implement it exactly like this now?




I've already read peaks.c and scr_results.c in this session. Let me verify each claim against the code I have.

Claim 1 — Left-to-right fill halts at max_peaks.

Verified. peaks.c:67:
for (uint16_t i = 1; i + 1 < n && found < max_peaks; i++) {

The found < max_peaks guard is in the loop condition, so it short-circuits the scan entirely once slots are full. The sort at line 85-87 only runs on whatever was collected. This is correct — it's a collect-first-N-by-position, then sort-by-height algorithm, not a collect-all-then-keep-top-N algorithm. The analysis nails this.

Claim 2 — Noise prominence can reach 3-4 µA with ±2 µA noise.

This needs a sanity check. With ±2 µA uniform noise on a near-zero baseline between peaks, a noise local maximum could be ~+2 µA and its neighboring troughs ~-2 µA. Prominence = peak minus highest valley floor. The valley floor would be the minimum between this spike and the nearest taller peak. On a near-zero baseline with ±2 µA noise, the valley floor could be as low as -2 µA. Prominence of a +2 µA spike over a -2 µA valley = 4 µA. So the "3-4 µA" claim is correct. A 5.0 µA threshold provides clearance; 3.0 µA does not. Verified.

Claim 3 — scr_results.c caps secondary display at P2/P3/P4.

Verified. scr_results.c:282:
for (uint16_t i = 1; i < n_peaks && i < 4; i++) {

The loop runs indices 1, 2, 3 — rendering P2, P3, P4 only. A 5th peak (index 4) is never displayed. So the correction is accurate: the visible failure isn't a "P5 ghost line", it's a real peak being evicted from the 4-slot collection before the sort. More precisely: if noise fills slot 0-3 early in the sweep, a legitimate peak later in the sweep never enters out[] at all, so it can't be sorted to the top regardless of its height.

Claim 4 — Bonus observation about firmware left-to-right bias.

Valid concern, but let me check if it's actually a problem in practice. The firmware calls peaks_find at ui_tft.c:181-182 with MAX_PEAKS=4 and threshold 1.0f. In a real DPV scan from -600 to +600 mV, there aren't typically many sub-threshold noise bumps ahead of real peaks — the DPV background subtraction (I_pulse - I_base) already suppresses most baseline noise. But it's a legitimate design observation for P8/P9 where edge cases with noisy electrodes or degraded bismuth films could produce early artifacts. Worth noting, not worth fixing now. Agreed.

One thing not mentioned that I'll add: the analysis says "I withdraw my max_peaks=6 suggestion" — good. But the reason it gives for why max_peaks=6 fails is stronger than what I said earlier. I said it "lets garbage through" (meaning ghost peaks on screen). The analysis correctly identifies the real failure mode: slot exhaustion evicts real peaks that haven't been scanned yet. That's worse — you silently miss a metal, not just display noise. Credit where due, that's a better articulation of the problem.

The scr_results loop cap correction is right but doesn't change the verdict. Whether you'd see a P5 ghost or a missing-metal eviction, raising max_peaks is still the wrong fix. The threshold is the correct lever. We agree.

Let me do one final verification read — confirm `metal_id.h` is truly host-clean (no IDF includes), since the whole link fix depends on peaks.c/metal_id.c compiling on the PC. Then I'll give you the verdict.   

Read [](file:///c%3A/Users/z00541ce/Documents/Coding/ST%20Thesis/uBIOPOT-Firmware/components/echem_core/include/echem_core/metal_id.h#1-1), lines 1 to 80

Verified — **metal_id.h is pure C** (only `<stdint.h>`/`<stdbool.h>`/`<stddef.h>`, explicitly "no esp_* or FreeRTOS headers"). That was the last dependency to confirm. Here's the full verification verdict.

## ✅ Correct and complete — safe to implement as written

**Math / detection (I worked the numbers):**
- All four centers sit inside their metal_id.c windows: Cd −700 ∈[−800,−600], Pb −450 ∈[−550,−400], Cu 0 ∈[−100,+50], Hg +300 ∈[+200,+400]. ✔
- With σ=70 mV and your spacings, I computed each peak's **prominence**: Pb ≈ 50, Cd ≈ 14.5, Cu ≈ 25, Hg ≈ 13.75 µA. **Every one clears the 5 µA gate by ≥ 2.7×** — no real peak is at risk, and the smallest gaps (Cd–Pb valley ≈ 15 µA floor) still leave four clean, strictly-separated local maxima. ✔
- Crest potentials stay inside their windows even after neighboring-tail pull (shifts are a few mV; Pb's dominant perturbation actually nudges it *left*, away from the −400 edge). ✔
- Sorted output → **P1 Pb(−450/50) · P2 Cd(−700/30) · P3 Cu(0/25) · P4 Hg(+300/18)**, and each maps to the right metal via `metal_identify`. ✔

**API correctness:**
- `peaks_find(s_I_buf, s_E_buf, …, peaks, 4, 5.0f)` — argument order is right: **`I_uA` first, then `E_mV`** (easy to swap — you got it right, keep it that way). `peaks[4]` sized to `max_peaks=4`; `n_pk` passed to `scr_results_set(peaks, n_pk, 1)` matches its `(peaks, n_peaks, electrode)` signature. ✔
- Gaussian-sum + `#if SIM_ADD_NOISE` guard correct; `rand()/RAND_MAX` → ±2 µA; `<stdlib.h>` already included (line 29). ✔

**Link fix:**
- peaks.c (`<float.h>/<string.h>`) and metal_id.c (`<math.h>`) are both pure C; their `#include "echem_core/*.h"` resolve via `src/stubs/echem_core/` (Step 6 copies all headers) + the existing `src/stubs` include dir. No new `-lm` need (`expf` already used; `NAN` is a macro). ✔
- CMake `src/echem/*.c` glob is consistent with the existing screen glob. No duplicate symbols (`peaks_find`/`metal_identify` defined once). `scan_state.c` correctly excluded. ✔

## Minor gotchas to keep in mind (none blocking)
1. **Re-copy main_sim.c → `src/main.c`** in WSL after editing — the sim builds `src/main.c`, not main_sim.c directly.
2. **Run the new Step 6b + the updated CMake patch** before rebuilding, or you'll still hit the `metal_identify` link error.
3. When you later flip **`SIM_ADD_NOISE 1`**: it's unseeded, so the noise pattern is identical each run (fine for reproducibility). The 5 µA threshold covers ±2 µA; if you raise noise amplitude, re-check the margin.
4. With noise **off** (default) there are exactly 4 maxima, so the left-to-right-fill concern can't bite — that only matters in the noise-on stress test, which the 5 µA gate is sized for.

**Verdict: implement as-is.** The edits are correct, the detection math checks out, and the link fix resolves the pre-existing `metal_identify` gap at the same time.


One minor gap

scr_scan_reset() (line 384-427) resets s_lbl_live_i back to "+0.0 µA" and s_lbl_elapsed back to "0:00", but does not reset s_lbl_live_e back to "+0.000V". If a user runs a scan, then starts a second scan, the E label will still show the last potential from the previous scan until the first new data point arrives. Cosmetic only — the value updates within 200ms — but for consistency with the other resets, add this to scr_scan_reset():

if (s_lbl_live_e)   lv_label_set_text(s_lbl_live_e, "+0.000V");

Right after the s_lbl_live_i reset at line 415.