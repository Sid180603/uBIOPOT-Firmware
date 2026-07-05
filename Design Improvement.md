Let me look at the screens holistically to give design feedback.

  Thought for 20s, read 6 files

Here's a design review of the LVGL screens, evaluated against the constraints of this specific project: 320×240 px ILI9341, 2-button encoder, Montserrat 14/20/28, dark theme on a lab bench.

---
Design Assessment

What works well

The dark palette is the right call for a lab instrument — teal-on-black reads well under fluorescent lighting, and the high-contrast status colors (green/amber/red) are immediately legible at arm's length. The three-band layout (status bar / content / hint bar) is consistent across every screen, which is exactly right for a 2-button encoder — the user always knows where navigation hints are.

The toast overlay and spinner are nice touches for a 240px display. The ring-buffer approach for chart rendering is the right architecture for smooth drawing.

Specific improvements

1. Typographic hierarchy is flat — the 14pt font does too much

Almost every element uses Montserrat 14: status bar, menu items, hint bars, labels, section headers, secondary peaks, WiFi info. The only exceptions are the live current readout (20pt) and splash title/peak values (28pt). On a 320×240 display, 14pt text is legible but everything reads at the same visual weight, making it hard to scan.

Suggestion: Use the 20pt font for the primary menu item labels (Start DPV especially — it's the main action and should be the most prominent element on the home screen). Keep 14pt for secondary items, hints, and metadata. This costs nothing in flash (you already have the font enabled) and creates a real visual hierarchy.

2. The home menu "Start DPV" doesn't stand out as the primary action

All five menu items have identical styling: same height, same font, same spacing. "Start DPV" — the thing the user will press 95% of the time — is visually indistinguishable from "About." On a 2-button device where the user is cycling through items, the primary action should be visually differentiated.

Suggestion: Give "Start DPV" a distinct treatment — a subtle accent-colored left border (2px, UI_COLOR_ACCENT) in the default state, or use the 20pt font for its label while other items stay at 14pt. This makes it visually obvious even when another item has focus.

3. The "Electrode" menu item behaves differently but looks the same

"Electrode: 1" is a toggle/cycle control, not a navigation item. But it's styled identically to the buttons that trigger actions or navigate to other screens. The user has no visual cue that pressing this item cycles a value rather than starting something.

Suggestion: Add the current value in accent color on the right side of the button (like a key-value row: "Electrode" left-aligned in TEXT, "1" right-aligned in ACCENT). This pattern mirrors the settings screen's SSID/IP rows and signals "this is a value I can change" rather than "this triggers an action."

4. Status bar could use a 1px bottom divider

The status bar (UI_COLOR_SURFACE on UI_COLOR_BG) relies entirely on the background color difference for separation. On some ILI9341 panels with lower contrast ratios, 0x16213E on 0x0D0D0F can blend together. A single 1px line in UI_COLOR_BORDER at the bottom of each status bar would sharpen the boundary without adding visual weight. The settings screen already does this with its WiFi section divider — applying it to status bars would be consistent.

5. Results screen has a lot of dead space in the "no peaks" case

When n_peaks == 0, the screen shows "NO PEAKS DETECTED" in the small 14pt dim font at the PEAK 1 header position, with placeholder dashes below. The entire middle 130px is almost empty. For a scan that ran for 30+ seconds and found nothing, this is a deflating experience.

Suggestion: Center the "No peaks detected" message vertically in the content area using the 20pt font in UI_COLOR_DIM, and add a one-line hint below: "Try adjusting concentration or scan range." This fills the space meaningfully and gives the user direction.

6. Chart series colors could be more distinguishable

The three electrode series use teal (0x00BCD4), amber (0xFF9800), and green (0x4CAF50). On a 16-bit RGB565 display, amber and green can appear closer to each other than on a 24-bit monitor, especially on the small 2.4" panel at viewing distance. All three are mid-brightness, which makes simultaneous display (ELECTRODE_ALL mode) harder to read.

Suggestion: If the series will ever overlap on the same chart, consider shifting electrode 3 to a lighter value — perhaps a pale blue/lavender (0x7986CB — indigo-300) that's clearly cooler than the warm amber. The teal/amber/cool-blue triad separates better on low-color-depth panels. This only matters for ELECTRODE_ALL though; for single-electrode scans the current teal is excellent.

7. Hint bar text could be more direct

Current hint text uses labels like "NAV: cycle" and "START: select." These are internal names for the buttons, not descriptions of what happens. A bench user glancing down while handling a sample would benefit from verbs.

┌──────────────────────────┬────────────────────────────────────────┐
│         Current          │               Suggested                │
├──────────────────────────┼────────────────────────────────────────┤
│ NAV: cycle               │ Scroll or just the arrow symbols alone │
├──────────────────────────┼────────────────────────────────────────┤
│ START: select            │ Select or OK                           │
├──────────────────────────┼────────────────────────────────────────┤
│ Hold START (2s) to abort │ Hold to cancel                         │
└──────────────────────────┴────────────────────────────────────────┘

The symbols LV_SYMBOL_UP LV_SYMBOL_DOWN already communicate "navigate" visually. The word "NAV" is redundant. On a 320px-wide display, every character of hint text costs real space — keeping hints terse leaves room for the actually important labels above.

8. Splash screen subtitle competes with the title

The splash shows "uBIOPOT" at 28pt in accent, then "Multiplexed Potentiostat v2" at 14pt in near-white just 34px below. The subtitle in UI_COLOR_TEXT (bright near-white) is almost as visually prominent as the teal title, creating visual competition rather than a clear primary/secondary hierarchy.

Suggestion: Use UI_COLOR_DIM for the subtitle instead of UI_COLOR_TEXT. The splash only lives for 2.2s — the user should absorb "uBIOPOT" in teal, not read the subtitle. The version line at the bottom already uses UI_COLOR_DIM correctly.

---
Summary of priorities

┌──────────┬────────────────────────────────────┬─────────────────────┐
│ Priority │               Change               │        Cost         │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ High     │ Start DPV visual differentiation   │ 3 lines             │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ High     │ Hint bar: verbs not labels         │ String changes only │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ Medium   │ Electrode item as key-value toggle │ ~15 lines           │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ Medium   │ 1px status bar divider             │ 4 lines per screen  │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ Low      │ Splash subtitle dimmed             │ 1 line              │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ Low      │ Results empty-state centering      │ ~10 lines           │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ Low      │ Electrode 3 series color tweak     │ 1 line              │
├──────────┼────────────────────────────────────┼─────────────────────┤
│ Low      │ 20pt font for key menu items       │ 2 lines             │
└──────────┴────────────────────────────────────┴─────────────────────┘

The bones are good. The palette is functional, the layout structure is consistent, and the data-path architecture (ring buffer → chart) is solid. The improvements above are mostly about using the existing resources (20pt font, accent color, border color) more deliberately to create hierarchy and signal intent.

Let me re-read the current state of all screens after the rename to give a thorough design review.

  Thought for 56s, read 8 files

Now let me look at reference material for embedded instrument UIs to ground the recommendations.

● Web Search("PalmSens EmStat potentiostat embedded display UI design 2024 2025")
Did 1 search in 3s

● Web Search("LVGL dark theme embedded instrument dashboard design best practices 320x240")
Did 1 search in 3s

● Web Search("LVGL 9 best practices small display instrument UI encoder navigation")
Did 1 search in 2s

● Web Search("scientific instrument embedded display design water quality analyzer UI")
Did 1 search in 3s

Web search is blocked by the enterprise proxy. I'll work from my knowledge of embedded instrument UIs, LVGL architecture, and the actual code I've just read.

---
Aqua-HMET LVGL Screen Design Review — Extended

Evaluated against: 320×240 ILI9341, 2-button encoder, Montserrat 14/20/28, ~320 KB free RAM alongside WiFi + LVGL, dark palette, lab bench context.

The previous review covered 8 incremental improvements. You're asking for deeper, more structural ideas — here's what I'd change if this were a from-scratch redesign pass, grounded in what the code actually does.

---
9. Scan screen: the chart area is underused — add axis labels

The chart occupies 312×152 px but has no axis labels at all. No "E (V)" on X, no "ΔI (µA)" on Y. The user sees a curve with gridlines and numbers in the readout row below, but the chart itself is mute. For a thesis demo or a lab audience who isn't you, this is a gap.

What to add:
- A rotated "ΔI (µA)" label along the left edge in 14pt DIM text (LVGL supports lv_obj_set_style_transform_rotation)
- An "E (V)" label at bottom-right of the chart in 14pt DIM

Cost: ~10 lines, negligible RAM (two static labels).

Why it matters for a thesis: Figures in your thesis chapters will be screenshots of this display. Axis labels mean each screenshot is self-documenting without caption annotation.

---
10. Home screen: the menu layout should use LVGL flex instead of hard-coded Y positions

Currently each menu item is placed at y = 28 + i * 34. This works for exactly 5 items at 320×240 — but it's fragile:
- If you change the status bar height or hint bar height, items overlap or float.
- Adding a 6th item (e.g. "Calibration" in P8) requires recalculating all positions.
- The content area isn't responsive to any layout constraint.

Suggestion: Replace the hand-computed positioning with an lv_obj_set_flex_flow(content_area, LV_FLEX_FLOW_COLUMN) container. This is what LVGL 9's flex layout exists for. Items distribute evenly, spacing adapts, and adding new items is one create_menu_item() call with no Y arithmetic.

lv_obj_t *menu_cont = lv_obj_create(s_scr);
lv_obj_set_size(menu_cont, LV_HOR_RES, LV_VER_RES - 24 - 30);  // between bar/hbar
lv_obj_set_pos(menu_cont, 0, 24);
lv_obj_set_flex_flow(menu_cont, LV_FLEX_FLOW_COLUMN);
lv_obj_set_flex_align(menu_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
lv_obj_set_style_pad_row(menu_cont, 4, 0);

Cost: ~20 lines refactor, 0 extra RAM (flex is metadata on existing objects, not new allocations). LV_USE_FLEX is already enabled in lv_conf.h.

---
11. Toast: use accent color for informational toasts, red only for errors

Right now every toast uses UI_COLOR_ABORT (red). But half the toasts are informational — "Aqua-HMET v1 | BITS Pilani 2026", "Params: set via WiFi web app or serial." These aren't errors. Showing them in an alarm-red banner causes desensitization — when a real error toast appears, the user's trained to ignore it.

Suggestion: Add a severity parameter to scr_toast_show:

typedef enum { TOAST_INFO, TOAST_ERROR } toast_level_t;
void scr_toast_show(const char *msg, toast_level_t level);

- TOAST_INFO → UI_COLOR_SURFACE bg with UI_COLOR_ACCENT left strip (2px)
- TOAST_ERROR → current red bg (reserved for real failures)

The caller at each site decides. "About" → INFO. "Start failed" → ERROR.

Cost: ~15 lines in scr_toast.c + a typedef in screen_mgr.h + updating 4 callsites.

---
12. Results screen: show a mini voltammogram thumbnail alongside peaks

The results screen currently shows peak numbers in big text with dead space below. The user just watched a voltammogram draw for 30+ seconds, then it disappears and they get numbers. Showing a small replay of the curve next to the peak values lets the user visually confirm "yes, that bump at -400 mV is the peak it's reporting."

Layout concept:

┌──────────────────────────────────────────────────┐
│  RESULTS                              E1  DPV    │  h=24
├──────────────┬───────────────────────────────────┤
│              │  PEAK 1                            │
│  mini chart  │  +50.0 µA      -0.400 V           │  h=130
│  (120×100)   │                                    │
│              │  P2: +12.1 µA @ -0.250 V           │
├──────────────┴───────────────────────────────────┤
│  [< Back]                       [Run Again]       │  h=30
└──────────────────────────────────────────────────┘

Implementation: Reuse the same lv_chart approach — create a small 120×100 scatter chart on the results screen, pre-fill it from s_E_buf/s_I_buf in scr_results_set(). The data is already in memory (the peak detection buffer in ui_tft.c). You'd need to pass the buffer pointers or add a scr_results_set_curve() call.

Cost: ~40 lines. RAM: one tiny chart with ~200 points = chart metadata + 200×8 bytes (1.6 KB). Well within budget.

Why this is high-value: In your thesis, the results screen screenshot immediately demonstrates the device works — curve + peaks in one image, no caption gymnastics needed.

---
13. Scan screen: add an elapsed time indicator

The progress shows "10/240" (step/total), but the user doesn't know how long the scan has been running or how long remains. A DPV scan with t_period_ms = 200 and 240 steps takes ~48 seconds. The user standing at a bench holding a sample wants to know "how much longer?"

Suggestion: Add a simple elapsed counter in the status bar (right-aligned where +0.000 V currently is, since that info is redundant with the readout row below):

│ E1  DPV   ●●●   10/240          0:12 │

Implementation: Start a timer at SCAN_EVT_START, increment a seconds counter with an lv_timer_create(..., 1000, ...). Display as M:SS. Kill on SCAN_EVT_SCAN_DONE/SCAN_EVT_ABORTED.

Cost: ~20 lines, 0 meaningful RAM.

---
14. Splash: cascade the fade-in for layered drama

Currently only the title fades in (800 ms, linear). The top label, subtitle, and version tag appear instantly at full opacity. For a 2.2-second splash, you can orchestrate a staggered reveal:

0 ms       top label fades in (300 ms)
200 ms     title fades in (800 ms) — already done
800 ms     subtitle fades in (400 ms)
1200 ms    version tag fades in (400 ms)
2200 ms    transition to home

This creates a waterfall effect — top-down, each layer supporting the next. It reads as "polished lab instrument" rather than "student project." On a 240px display, the reveal controls the eye path and emphasizes the hierarchy.

Implementation: Create 3 additional lv_anim_t with lv_anim_set_delay():

lv_anim_t a_top;
lv_anim_init(&a_top);
lv_anim_set_var(&a_top, top);
lv_anim_set_exec_cb(&a_top, splash_set_opa);
lv_anim_set_values(&a_top, LV_OPA_TRANSP, LV_OPA_COVER);
lv_anim_set_duration(&a_top, 300);
lv_anim_set_delay(&a_top, 0);
lv_anim_start(&a_top);

// similar for sub (delay=800, dur=400) and ver (delay=1200, dur=400)

Cost: 20 lines, 3 animation structs (~96 bytes stack, freed after start).

---
15. Status bar: make READY/RUNNING/ERROR use a pulsing dot instead of text symbols

The status indicator uses LVGL font symbols (LV_SYMBOL_OK, LV_SYMBOL_REFRESH, LV_SYMBOL_WARNING). These are fine but look like generic UI — they don't feel like an instrument. Scientific instruments use a status LED metaphor: a dot that's green when ready, pulsing amber when measuring, red on error.

Suggestion: Replace the symbol text with a small lv_obj_t circle (12×12, lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE)) whose background color changes with state. For RUNNING, add a slow pulse animation (opacity 255→100→255, 1.5s cycle). For ERROR, add a fast blink (300ms cycle).

lv_obj_t *dot = lv_obj_create(bar);
lv_obj_set_size(dot, 10, 10);
lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
lv_obj_set_style_bg_color(dot, lv_color_hex(UI_COLOR_READY), 0);
lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
lv_obj_set_style_border_width(dot, 0, 0);

Keep the text label ("READY", "MEASURING") next to it for accessibility, but the dot becomes the primary visual signal. At arm's length on a bench, you see the dot color before you can read the text.

Cost: ~25 lines total, minimal RAM (one small obj + animation).

---
16. Settings screen: rethink the layout — it's the least useful screen

The settings screen currently shows:
- 3 WiFi rows (stubbed as "---")
- A QR code placeholder
- A "Zero" button
- A "Back" button

This is mostly placeholder content for P5/P8. But even when filled in, it's a settings dump, not a settings interface. On a 2-button device, you can't actually edit WiFi settings from this screen — it's read-only info. The Zero button does nothing yet.

Suggestion for now: Restructure into two clear sections:

┌──────────────────────────────────────────────────┐
│  SETTINGS                                         │
├──────────────────────────────────────────────────┤
│  WiFi                                             │
│  SSID   Aqua-HMET-A1B2     ▸ (QR placeholder)   │
│  IP     192.168.4.1                               │
│  URL    aquahmet.local                            │
├──────────────────────────────────────────────────┤
│  Calibration                                      │
│  ◉ Zero         [ Not calibrated ]                │
├──────────────────────────────────────────────────┤
│  [< Back]                                         │
└──────────────────────────────────────────────────┘

The two sections each have a header in accent + a 1px divider. This prepares the visual structure for P8 (calibration data will fill that section) without looking empty now.

---
17. Drastic option: replace the 5-item linear menu with a 2×2 + 1 grid

This is the "big risk" suggestion. The current home screen is a vertical list of 5 items, one focused at a time, cycled with a single button. This works but it's the most generic embedded UI pattern — it looks like every Arduino project ever.

Alternative — icon tiles:

┌──────────────────────────────────────────────────┐
│  Aqua-HMET       ● READY                  E1     │
├──────────────────────────────────────────────────┤
│  ┌────────────┐  ┌────────────┐                   │
│  │  ▶ START   │  │  ⚡ E:1    │                   │
│  │    DPV     │  │  Electrode │                   │
│  └────────────┘  └────────────┘                   │
│  ┌────────────┐  ┌────────────┐                   │
│  │  📋 Params │  │  ⚙ Settings│                   │
│  └────────────┘  └────────────┘                   │
├──────────────────────────────────────────────────┤
│  ▲▼ Navigate         OK Select                    │
└──────────────────────────────────────────────────┘

However — I'd actually advise against this for one critical reason: your 2-button encoder navigates linearly. A 2D grid requires 2D navigation (up/down + left/right), but you only have 1D input (rotate = next, enter = select). LVGL's lv_group would need to traverse the grid in reading order (left→right, top→bottom), which is confusing for a 2×2 grid — the user sees spatial layout but experiences linear traversal. The mental model breaks.

Verdict: Keep the linear list, but apply the earlier suggestions (20pt Start DPV, key-value electrode row, hint bar verbs) to make it feel intentional rather than default.

---
Summary of all improvements (original 8 + new 9)

┌─────┬────────────────────────────────┬────────────────────┬──────────────────┐
│  #  │             Change             │        Cost        │     Priority     │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 1   │ 20pt font for Start DPV label  │ 2 lines            │ High             │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 2   │ Start DPV accent left border   │ 3 lines            │ High             │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 3   │ Electrode as key-value toggle  │ ~15 lines          │ Medium           │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 4   │ 1px status bar bottom dividers │ 4 lines/screen     │ Medium           │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 5   │ Splash subtitle → UI_COLOR_DIM │ 1 line             │ Low              │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 6   │ Results empty-state centering  │ ~10 lines          │ Low              │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 7   │ Electrode 3 series color tweak │ 1 line             │ Low              │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 8   │ Hint bar: verbs not labels     │ String changes     │ High             │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 9   │ Chart axis labels (E/ΔI)       │ ~10 lines          │ High (thesis)    │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 10  │ Flex layout for menu items     │ ~20 lines refactor │ Medium           │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 11  │ Toast severity (info vs error) │ ~15 lines          │ High             │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 12  │ Mini voltammogram on results   │ ~40 lines, 1.6 KB  │ High (thesis)    │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 13  │ Elapsed time on scan screen    │ ~20 lines          │ Medium           │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 14  │ Cascading splash fade-in       │ ~20 lines          │ Low (polish)     │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 15  │ Status dot instead of symbols  │ ~25 lines          │ Medium           │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 16  │ Settings: two-section layout   │ ~20 lines refactor │ Low (P5/P8 prep) │
├─────┼────────────────────────────────┼────────────────────┼──────────────────┤
│ 17  │ 2×2 grid menu                  │ Rejected           │ —                │
└─────┴────────────────────────────────┴────────────────────┴──────────────────┘

Absolutely — and for a device called "Aqua-HMET" whose entire purpose is heavy metal detection in water, this is arguably the most important thing to show. The user doesn't care about µA; they care about "is this water safe?"

Here's how it works end-to-end:

What you need

1. Metal identification from peak potential

DPV peak potentials are characteristic for each metal ion (in a given supporting electrolyte). Common values in acetate buffer on a carbon/bismuth electrode:

┌───────┬────────────────────────────────┬──────────────────┐
│ Metal │ Typical E_peak (mV vs Ag/AgCl) │ WHO Limit (µg/L) │
├───────┼────────────────────────────────┼──────────────────┤
│ Cd²⁺  │ −800 to −600                   │ 3                │
├───────┼────────────────────────────────┼──────────────────┤
│ Pb²⁺  │ −550 to −400                   │ 10               │
├───────┼────────────────────────────────┼──────────────────┤
│ Cu²⁺  │ −100 to +50                    │ 2000             │
├───────┼────────────────────────────────┼──────────────────┤
│ As³⁺  │ +50 to +200                    │ 10               │
├───────┼────────────────────────────────┼──────────────────┤
│ Hg²⁺  │ +200 to +400                   │ 6                │
├───────┼────────────────────────────────┼──────────────────┤
│ Zn²⁺  │ −1100 to −900                  │ no guideline     │
└───────┴────────────────────────────────┴──────────────────┘

These windows shift with electrode material, electrolyte, and pH — so they'd be configurable in NVS (P8), not hardcoded.

2. Concentration from peak current

C = (I_peak - intercept) / slope — a simple linear calibration per metal. The slope comes from running known standards (P8 calibration). Until calibrated, show µA only with a "Not calibrated" note.

3. WHO comparison

A fixed lookup table: who_limit_ugL[METAL_Pb] = 10. Compare and show SAFE/UNSAFE.

What the results screen would look like

┌──────────────────────────────────────────────────┐
│  RESULTS                              E1  DPV    │
├──────────────────────────────────────────────────┤
│                                                   │
│   Pb  LEAD                         ⚠ UNSAFE     │  ← red if > WHO
│   42.3 µg/L                WHO limit: 10 µg/L    │
│   Peak: +50.0 µA @ −0.400 V                      │
│                                                   │
│   Cd  CADMIUM                       ✓ SAFE       │  ← green if ≤ WHO
│   1.2 µg/L                 WHO limit: 3 µg/L     │
│                                                   │
├──────────────────────────────────────────────────┤
│  [< Back]                       [Run Again]       │
└──────────────────────────────────────────────────┘

The key design choices:
- Metal name in accent color, large — the user identifies what was found
- Concentration in 20pt or 28pt — the primary number
- WHO limit in DIM text — context for the number
- SAFE/UNSAFE badge — green UI_COLOR_READY or red UI_COLOR_ABORT, right-aligned, impossible to miss

What's needed in code

Data layer (echem_core — pure C, host-testable):

typedef struct {
    const char *symbol;      // "Pb", "Cd", ...
    const char *name;        // "Lead", "Cadmium", ...
    float e_min_mV;          // peak window low
    float e_max_mV;          // peak window high
    float who_limit_ugL;     // WHO guideline (0 = no guideline)
    float slope;             // µA per µg/L (from calibration)
    float intercept;         // µA baseline (from calibration)
    bool  calibrated;        // false until user runs standards
} metal_profile_t;

This is a small static array (~6 entries, <200 bytes). The peaks_find() output already gives you E_mV and I_uA per peak — you just loop through metal profiles and match by potential window.

UI layer (scr_results.c):

Instead of showing raw "PEAK 1: +50.0 µA @ -0.400 V", you'd:
1. Look up which metal matches the peak potential
2. If calibrated → compute concentration, compare to WHO limit
3. If not calibrated → show µA with "Calibrate for concentration"

Memory cost: Negligible. The metal profile table is ~200 bytes. The results screen labels already exist — you're changing their content, not adding many new objects. Maybe 2-3 additional labels per detected metal (concentration, WHO limit, SAFE/UNSAFE badge).

What to do now vs later

┌───────────────────────────────────────────────────┬────────────────────────────────────────┬────────────────────────────────────┐
│                 Now (P4 UI prep)                  │            P8 (calibration)            │          P9 (validation)           │
├───────────────────────────────────────────────────┼────────────────────────────────────────┼────────────────────────────────────┤
│ Add metal identification by peak potential window │ Load slopes from NVS                   │ Validate against known standards   │
├───────────────────────────────────────────────────┼────────────────────────────────────────┼────────────────────────────────────┤
│ Show metal name + "Not calibrated" on results     │ Implement calib_I_to_concentration()   │ Generate thesis comparison figures │
├───────────────────────────────────────────────────┼────────────────────────────────────────┼────────────────────────────────────┤
│ Show WHO limit reference value                    │ Compute and display real concentration │ Accuracy characterization          │
├───────────────────────────────────────────────────┼────────────────────────────────────────┼────────────────────────────────────┤
│ Design the SAFE/UNSAFE badge layout               │ Enable the badge with real data        │                                    │
└───────────────────────────────────────────────────┴────────────────────────────────────────┴────────────────────────────────────┘

The UI can be built now with placeholder calibration. The moment P8 fills in real slopes, concentration and WHO comparison light up automatically.