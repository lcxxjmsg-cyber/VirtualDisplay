#pragma once

#define WPP_CONTROL_GUIDS \
    WPP_DEFINE_CONTROL_GUID(VirtualDisplayIddTraceGuid, \
        (0x9a3b5c7d, 0x8e9f, 0x4a2b, 0x8c, 0x3d, 0x1e, 0x5f, 0x7a, 0x9b, 0x2c, 0x4d), \
        WPP_DEFINE_BIT(TRACE_DRIVER)   \
        WPP_DEFINE_BIT(TRACE_DEVICE)   \
        WPP_DEFINE_BIT(TRACE_MONITOR)  \
        WPP_DEFINE_BIT(TRACE_IOCTL)    \
        WPP_DEFINE_BIT(TRACE_SWAPCHAIN))

#define WPP_FLAG_LEVEL_LOGGER(flag, level) \
    WPP_LEVEL_LOGGER(flag)

#define WPP_FLAG_LEVEL_ENABLED(flag, level) \
    (WPP_LEVEL_ENABLED(flag) && WPP_CONTROL(WPP_BIT_##flag).Enabled)

#define WPP_LEVEL_FLAGS_LOGGER(lvl, flags) \
    WPP_LEVEL_LOGGER(flags)

#define WPP_LEVEL_FLAGS_ENABLED(lvl, flags) \
    (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_##flags).Enabled)
