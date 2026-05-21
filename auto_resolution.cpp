/*
 * Auto Resolution Addon for ReShade
 *
 * Detects the PS2's actual native display resolution by examining scissor
 * rectangles that PCSX2 programs for each draw call, accumulated over a
 * rolling window of frames.
 *
 * Why accumulate over multiple frames:
 *   Some PS2 display modes (e.g. 512x224) emit only ~1 scissor per frame for
 *   the native size; a per-frame threshold of 2 is never met. Accumulating
 *   over ACCUMULATE_FRAMES frames lets even sparse signals reach MIN_ACC_COUNT.
 *
 * Correct criterion — smallest qualifying over ACCUMULATE_FRAMES frames:
 *   The smallest mode whose accumulated count reaches MIN_ACC_COUNT is taken
 *   as the active native resolution.
 *
 *     • 512x224 mode → 512x224 scissors accumulate over the window;
 *       512x448 scissors also appear but 512x224 is smaller  →  512x224  ✓
 *     • 512x448 mode → only 512x448 scissors appear          →  512x448  ✓
 *
 * Why require STABLE_WINDOWS consecutive windows:
 *   Scene transitions can emit spurious small-resolution scissors for a single
 *   window (e.g. UI overlays during loading). Requiring the same result across
 *   two consecutive windows filters these one-off false positives while still
 *   responding promptly to real mode changes.
 *
 * Fallback — init_resource:
 *   Modes whose draw calls emit no detectable scissors (e.g. 512x256) are
 *   caught by monitoring newly created render targets.  The inferred native
 *   resolution is used when scissor analysis produces no qualifying result.
 *
 * Downgrade guard:
 *   Spurious sub-resolution scissors (from PCSX2 internal ops or particle
 *   effects) appear alongside the real display-mode scissors within the same
 *   window.  A candidate smaller than the applied mode is suppressed while
 *   the applied mode's scissors are still qualifying.  When they disappear —
 *   meaning the mode genuinely changed — the downgrade is allowed through.
 *   On suppression the RT baseline is restored to the confirmed display mode
 *   so that a corrupt s_rt_native cannot poison the scissor-less fallback.
 *   At startup (s_applied_width == 0), the RT-inferred native is used as the
 *   guard baseline when available, preventing BIOS opening-animation scissors
 *   from prematurely locking in a small resolution before the real mode
 *   stabilizes.
 *
 * Stuck-state escape:
 *   Once a spurious small mode is incorrectly applied (e.g. a 256x224
 *   loading-screen artifact after a FMV), the downgrade guard cannot help:
 *   there is no longer a downgrade to suppress.  When the smallest qualifying
 *   candidate equals the applied mode but a strictly wider qualifying mode has
 *   more accumulated hits — meaning it is the dominant display signal — the
 *   stuck mode is overridden in favour of the wider mode via the stability
 *   mechanism.  The strictly-wider condition (w > applied_w, not just larger)
 *   prevents 512x448 buffer co-occurrences from escaping a correct 512x224
 *   applied mode (same width, not wider).
 */

#include <reshade.hpp>
#include <charconv>
#include <cstring>

using namespace reshade::api;

// Ordered large→small. Reverse traversal in on_reshade_present finds the
// smallest qualifying mode efficiently.
static constexpr uint32_t k_mode_count = 14;
static constexpr struct { uint32_t w, h; } k_ps2_modes[k_mode_count] = {
    {640, 480},
    {640, 448},
    {640, 256},
    {640, 224},
    {512, 448},
    {512, 417},
    {512, 416},
    {512, 256},
    {512, 240},
    {512, 224},
    {320, 240},
    {320, 224},
    {256, 240},
    {256, 224},
};

static constexpr uint32_t ACCUMULATE_FRAMES = 8;
static constexpr uint32_t MIN_ACC_COUNT     = 2;
static constexpr uint32_t STABLE_WINDOWS    = 2;

static uint32_t s_acc_counts[k_mode_count] = {};
static uint32_t s_acc_frames = 0;

// Stability tracking: scissor result must repeat for STABLE_WINDOWS before applying
static int      s_pending_idx   = -1;
static uint32_t s_pending_count = 0;

// Fallback: native resolution inferred from render-target creation
static uint32_t s_rt_native_w = 0;
static uint32_t s_rt_native_h = 0;

static uint32_t s_applied_width  = 0;
static uint32_t s_applied_height = 0;

static int try_snap_ps2(uint32_t w, uint32_t h)
{
    for (uint32_t i = 0; i < k_mode_count; ++i)
    {
        if (w % k_ps2_modes[i].w != 0)
            continue;
        const uint32_t s = w / k_ps2_modes[i].w;
        if (s > 0 && h == k_ps2_modes[i].h * s)
            return static_cast<int>(i);
    }
    return -1;
}

static void on_init_resource(device *, const resource_desc &desc,
    const subresource_data *, resource_usage, resource)
{
    if (desc.type != resource_type::texture_2d)
        return;
    if ((desc.usage & resource_usage::render_target) == resource_usage::undefined)
        return;

    const int idx = try_snap_ps2(desc.texture.width, desc.texture.height);
    if (idx < 0)
        return;

    s_rt_native_w = k_ps2_modes[idx].w;
    s_rt_native_h = k_ps2_modes[idx].h;

    // Invalidate stale scissor data from the previous mode
    std::memset(s_acc_counts, 0, sizeof(s_acc_counts));
    s_acc_frames    = 0;
    s_pending_idx   = -1;
    s_pending_count = 0;
}

static void on_bind_scissor_rects(command_list *, uint32_t, uint32_t count, const rect *rects)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (rects[i].left != 0 || rects[i].top != 0)
            continue;

        const int idx = try_snap_ps2(rects[i].width(), rects[i].height());
        if (idx >= 0)
            ++s_acc_counts[idx];
    }
}

static void on_reshade_present(effect_runtime *runtime)
{
    if (++s_acc_frames < ACCUMULATE_FRAMES)
        return;
    s_acc_frames = 0;

    // Find smallest mode with sufficient accumulated hits
    int best_idx = -1;
    for (int i = static_cast<int>(k_mode_count) - 1; i >= 0; --i)
    {
        if (s_acc_counts[i] >= MIN_ACC_COUNT)
        {
            best_idx = i;
            break;
        }
    }

    // Downgrade guard: suppress a candidate smaller than the applied mode while
    // the applied mode's own scissors are still qualifying.  Spurious scissors
    // from PCSX2 internal ops or particle effects always co-occur with the real
    // display-mode scissors; a genuine mode change stops the old scissors first.
    // On suppression, restore the RT baseline to the confirmed display mode so
    // that a render target created by an effect cannot corrupt the fallback.
    // At startup (s_applied_width == 0), fall back to the RT-inferred native as
    // the guard baseline so BIOS animation scissors cannot prematurely win.
    const uint32_t guard_w = s_applied_width > 0 ? s_applied_width : s_rt_native_w;
    const uint32_t guard_h = s_applied_width > 0 ? s_applied_height : s_rt_native_h;

    if (best_idx >= 0 && guard_w > 0)
    {
        const bool is_downgrade =
            k_ps2_modes[best_idx].w < guard_w ||
            (k_ps2_modes[best_idx].w == guard_w &&
             k_ps2_modes[best_idx].h < guard_h);

        if (is_downgrade)
        {
            bool applied_still_active = false;
            for (uint32_t i = 0; i < k_mode_count && !applied_still_active; ++i)
            {
                if (k_ps2_modes[i].w == guard_w &&
                    k_ps2_modes[i].h == guard_h &&
                    s_acc_counts[i] >= MIN_ACC_COUNT)
                    applied_still_active = true;
            }
            if (applied_still_active)
            {
                best_idx      = -1;
                s_rt_native_w = guard_w;
                s_rt_native_h = guard_h;
            }
        }
    }

    // Stuck-state escape: when the smallest qualifying candidate equals the applied
    // mode but a strictly wider mode dominates in hit count, override to that wider
    // mode.  "Strictly wider" (w > applied_w) prevents 512x448 buffer hits from
    // escaping a correct 512x224 applied mode (same width, not wider).
    if (best_idx >= 0 &&
        k_ps2_modes[best_idx].w == s_applied_width &&
        k_ps2_modes[best_idx].h == s_applied_height)
    {
        for (int i = static_cast<int>(k_mode_count) - 1; i >= 0; --i)
        {
            if (k_ps2_modes[i].w > s_applied_width &&
                s_acc_counts[i] >= MIN_ACC_COUNT &&
                s_acc_counts[i] > s_acc_counts[best_idx])
            {
                best_idx = static_cast<int>(i);
                break;
            }
        }
    }

    std::memset(s_acc_counts, 0, sizeof(s_acc_counts));

    uint32_t w, h;
    if (best_idx >= 0)
    {
        // Require STABLE_WINDOWS consecutive windows before accepting
        if (best_idx == s_pending_idx)
            ++s_pending_count;
        else {
            s_pending_idx   = best_idx;
            s_pending_count = 1;
        }
        if (s_pending_count < STABLE_WINDOWS)
            return;

        w = k_ps2_modes[best_idx].w;
        h = k_ps2_modes[best_idx].h;
    }
    else
    {
        s_pending_idx   = -1;
        s_pending_count = 0;

        if (s_rt_native_w == 0)
            return;
        w = s_rt_native_w;
        h = s_rt_native_h;
    }

    if (w == s_applied_width && h == s_applied_height)
        return;

    s_applied_width  = w;
    s_applied_height = h;

    char wb[12], hb[12];
    *std::to_chars(wb, wb + sizeof(wb), w).ptr = '\0';
    *std::to_chars(hb, hb + sizeof(hb), h).ptr = '\0';

    struct SetResCtx { const char *wb; const char *hb; };
    SetResCtx ctx = { wb, hb };
    runtime->enumerate_techniques(nullptr,
        [](effect_runtime *rt, effect_technique tech, void *ud) {
            auto &c = *static_cast<SetResCtx *>(ud);
            char name[256];
            rt->get_technique_effect_name(tech, name);

            // Restrict to CRT-Guest family: filename must contain "crt" or "ntsc"
            // (case-insensitive). Covers all CRT-Guest ports/variants and NTSC-Adaptive.fx
            // from the same author. Prevents unrelated effects from being touched, avoiding
            // spurious reloads of effects whose required textures may be absent (e.g. UIMask.fx).
            bool matched = false;
            for (const char *p = name; *p && !matched; ++p)
            {
                if (p[1] && p[2])
                {
                    if ((p[0]|0x20)=='c' && (p[1]|0x20)=='r' && (p[2]|0x20)=='t')
                        matched = true;
                }
                if (p[1] && p[2] && p[3])
                {
                    if ((p[0]|0x20)=='n' && (p[1]|0x20)=='t' && (p[2]|0x20)=='s' && (p[3]|0x20)=='c')
                        matched = true;
                }
            }
            if (!matched)
                return;

            rt->set_preprocessor_definition_for_effect(name, "Resolution_X", c.wb);
            rt->set_preprocessor_definition_for_effect(name, "Resolution_Y", c.hb);
        }, &ctx);
}

extern "C" __declspec(dllexport) const char *NAME        = "Auto Resolution";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Detects PS2 native resolution from scissor rect analysis and updates "
    "Resolution_X/Y preprocessor definitions for all active effects automatically.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
        reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_bind_scissor_rects);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(hModule);
        break;
    }

    return TRUE;
}
