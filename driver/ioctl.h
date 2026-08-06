#pragma once
#include <windows.h>
#include <winioctl.h>

// initguid.h must follow the SDK GUID declarations to avoid redefining storage interface GUIDs.
#include <initguid.h>

/// Device interface used by the user-mode control utility to reach the VirtualDisplay IddCx driver.
DEFINE_GUID(GUID_DEVINTERFACE_VIRTUALDISPLAY, 0x6a69acbc, 0xf02d, 0x4921, 0xba, 0x1b, 0xf2, 0xf8, 0xbe, 0x21, 0x4b, 0x6c);

#define IOCTL_VD_ADD_MONITOR \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_REMOVE_MONITOR \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_GET_MONITOR_COUNT \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_CLEAR_ALL_MONITORS \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_UPDATE_MONITOR \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_GET_CAPABILITIES \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_SET_RENDER_ADAPTER \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_GET_RENDER_ADAPTER \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VD_LIST_MONITORS \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define VD_CAP_DISPLAY_CONFIG_UPDATE 0x00000002u
#define VD_CAP_DISPLAY_CONFIG_UPDATE2 0x00000004u
#define VD_CAP_SYSTEM_MEMORY_SWAPCHAIN 0x00000008u
#define VD_CAP_PRECISE_PRESENT_REGIONS 0x00000010u
#define VD_CAP_HDR10 0x00000020u

#define MAX_MONITORS 8
#define MONITOR_INDEX_ANY 0xFFFFFFFF

/**
 * @brief Monitor geometry exchanged with the VirtualDisplay IddCx control utility.
 */
struct MonitorDesc {
  UINT32 Width;  ///< Width in pixels.
  UINT32 Height;  ///< Height in pixels.
  UINT32 VSync;  ///< in millihz (60000 = 60 Hz)
  UINT32 MonitorIndex;  ///< One-based monitor index, or assigned index for IOCTL_VD_ADD_MONITOR.
};

/**
 * @brief Runtime IddCx capability summary returned to user-mode tooling.
 */
struct IddCapabilityDesc {
  UINT32 RuntimeMajor;  ///< Public IddCx runtime major version.
  UINT32 RuntimeMinor;  ///< Public IddCx runtime minor version.
  UINT32 RuntimeRevision;  ///< IddCx runtime revision byte.
  UINT32 CapabilityFlags;  ///< Bitmask of VD_CAP_* values.
};

#define VD_RENDER_AUTO 0x00000001u
#define VD_RENDER_REQUESTED_VALID 0x00000002u
#define VD_RENDER_ACTUAL_VALID 0x00000004u
#define VD_RENDER_LUID_HINT_VALID 0x00000008u

/**
 * @brief Preferred and actual render adapters for the VirtualDisplay adapter.
 *
 * A set request uses `VD_RENDER_AUTO`, or supplies a runtime LUID together
 * with the adapter hardware identity. The driver returns the LUID it requested
 * from IddCx and the actual LUID later reported by Windows in
 * `EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN`.
 */
struct RenderAdapterDesc {
  UINT32 Size;  ///< Size of this versioned structure in bytes.
  UINT32 Flags;  ///< Bitmask of `VD_RENDER_*` values.
  UINT32 VendorId;  ///< PCI vendor identifier of the requested adapter.
  UINT32 DeviceId;  ///< PCI device identifier of the requested adapter.
  UINT32 SubSysId;  ///< PCI subsystem identifier of the requested adapter.
  UINT32 Revision;  ///< PCI revision of the requested adapter.
  LUID RequestedLuid;  ///< LUID passed to `IddCxAdapterSetRenderAdapter`.
  LUID ActualLuid;  ///< LUID supplied by Windows for the active swapchain.
};
