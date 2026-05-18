/*
 * Auto Resolution Addon for ReShade
 *
 * Strategy: PCSX2 renders the PS2 framebuffer to an internal render target
 * sized at (native_resolution * upscale_factor). The swapchain only shows
 * the final stretched output, so we can't read the native resolution from it.
 *
 * Instead, we listen to init_resource and check every new render target: if
 * its width and height are both clean integer multiples of a known PS2 native
 * resolution (using the same factor), we've found the PS2 framebuffer and can
 * derive the native resolution. When it changes, we update Resolution_X/Y in
 * CRT-Guest-NTSC.fx.
 */

#include <reshade.hpp>
#include <charconv>
#include <atomic>

using namespace reshade::api;

static std::atomic<uint32_t> s_native_width{0};
static std::atomic<uint32_t> s_native_height{0};
static uint32_t s_applied_width  = 0;
static uint32_t s_applied_height = 0;

// Ordered large→small so we prefer the interpretation with the smallest
// upscale factor (i.e., the most specific native resolution).
static constexpr struct { uint32_t w, h; } k_ps2_modes[] = {
    {640, 480},
    {640, 448},
    {512, 448},
    {512, 240},
    {320, 240},
    {320, 224},
    {256, 240},
    {256, 224},
};

static bool try_snap_ps2(uint32_t w, uint32_t h, uint32_t &nw, uint32_t &nh)
{
    for (const auto &m : k_ps2_modes)
    {
        if (w % m.w != 0)
            continue;
        const uint32_t s = w / m.w;
        if (s > 0 && h == m.h * s)
        {
            nw = m.w;
            nh = m.h;
            return true;
        }
    }
    return false;
}

static void on_init_resource(device *, const resource_desc &desc,
    const subresource_data *, resource_usage, resource)
{
    if (desc.type != resource_type::texture_2d)
        return;
    if ((desc.usage & resource_usage::render_target) == resource_usage::undefined)
        return;

    uint32_t nw, nh;
    if (try_snap_ps2(desc.texture.width, desc.texture.height, nw, nh))
    {
        s_native_width.store(nw, std::memory_order_relaxed);
        s_native_height.store(nh, std::memory_order_relaxed);
    }
}

static void on_reshade_present(effect_runtime *runtime)
{
    const uint32_t w = s_native_width.load(std::memory_order_relaxed);
    const uint32_t h = s_native_height.load(std::memory_order_relaxed);

    if (w == 0 || (w == s_applied_width && h == s_applied_height))
        return;

    s_applied_width  = w;
    s_applied_height = h;

    char wb[12], hb[12];
    *std::to_chars(wb, wb + sizeof(wb), w).ptr = '\0';
    *std::to_chars(hb, hb + sizeof(hb), h).ptr = '\0';

    runtime->set_preprocessor_definition_for_effect("CRT-Guest-NTSC.fx", "Resolution_X", wb);
    runtime->set_preprocessor_definition_for_effect("CRT-Guest-NTSC.fx", "Resolution_Y", hb);
}

extern "C" __declspec(dllexport) const char *NAME        = "Auto Resolution";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Detects PS2 native resolution from render target creation and updates "
    "CRT-Guest-NTSC Resolution_X/Y preprocessor definitions automatically.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(hModule);
        break;
    }

    return TRUE;
}
