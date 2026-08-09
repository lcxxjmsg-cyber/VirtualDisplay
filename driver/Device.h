#pragma once

#ifndef IDDCX_VERSION_MAJOR
  #define IDDCX_VERSION_MAJOR 1
#endif
#ifndef IDDCX_VERSION_MINOR
  #define IDDCX_VERSION_MINOR 0xA
#endif

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <wdf.h>

#if !defined(WDF_STRUCT_INFO)
typedef size_t *WDF_STRUCT_INFO;
#endif

#include "ioctl.h"

#include <d3d11.h>
#include <iddcx.h>

#define MAX_MONITOR_COUNT 8

/**
 * @brief Runtime state for one virtual IddCx monitor.
 */
struct MonitorContext {
  IDDCX_MONITOR IddCxMonitor;  ///< IddCx monitor object owned by the adapter.
  IDDCX_SWAPCHAIN SwapChain;  ///< Active swapchain assigned by the OS.
  HANDLE hNextSurfaceEvent;  ///< Event signaled when a new desktop surface is ready.
  HANDLE SwapChainThread;  ///< Worker thread that consumes swapchain frames.
  HANDLE SwapChainTerminateEvent;  ///< Event used to stop the swapchain worker.
  ID3D11Device *ProcessingDevice;  ///< D3D device supplied to IddCx for frame processing.
  BOOL UseHdrSwapChain;  ///< True when this monitor must consume frames through the IddCx 1.10 Buffer2 DDI.
  INT32 Width;  ///< Monitor width in pixels.
  INT32 Height;  ///< Monitor height in pixels.
  INT32 VSync;  ///< Refresh rate in millihertz.
  BOOL DisplayConfigInitialized;  ///< True after Windows accepts the path's initial display configuration.
  BOOL InUse;  ///< True when this slot owns an arrived monitor.
};

/**
 * @brief WDF context attached to an IddCx monitor object.
 */
struct MonitorObjectContext {
  UINT32 Slot;  ///< Zero-based virtual monitor slot represented by the object.
  MonitorContext *Monitor;  ///< Driver monitor state represented by the object.
};

/**
 * @brief IddCx features available from the runtime bound to this driver instance.
 */
struct IddRuntimeCapabilities {
  ULONG RawVersion;  ///< Raw IddCx version value reported by IddCxGetVersion.
  ULONG MajorVersion;  ///< Public IddCx major version.
  ULONG MinorVersion;  ///< Public IddCx minor version.
  ULONG Revision;  ///< IddCx revision value.
  BOOL AdapterDisplayConfigUpdate;  ///< True when IddCxAdapterDisplayConfigUpdate is callable.
  BOOL AdapterDisplayConfigUpdate2;  ///< True when IddCxAdapterDisplayConfigUpdate2 is callable.
  BOOL MonitorUpdateModes;  ///< True when monitor mode-list updates are callable.
  BOOL MonitorUpdateModes2;  ///< True when HDR-capable monitor mode-list updates are callable.
  BOOL SwapChainInSystemMemory;  ///< True when system-memory swapchain queries are callable.
  BOOL SwapChainReleaseAndAcquireBuffer2;  ///< True when the metadata2 acquire path is callable.
  BOOL HardwareCursor2;  ///< True when the second hardware-cursor query API is callable.
  BOOL HardwareCursor3;  ///< True when the third hardware-cursor query API is callable.
  BOOL PrecisePresentRegions;  ///< True when precise present-region tracking can be requested.
  BOOL Hdr10;  ///< True when the complete IddCx 1.10 HDR DDI contract is available.
};

/**
 * @brief Runtime state for the root IddCx adapter device.
 */
struct DeviceContext {
  IDDCX_ADAPTER IddCxAdapter;  ///< IddCx adapter object initialized in D0.
  MonitorContext Monitors[MAX_MONITOR_COUNT];  ///< Virtual monitor slots.
  UINT32 MonitorCount;  ///< Number of active virtual monitors.
  WDFWAITLOCK Lock;  ///< Guard for monitor slot mutations.
  IddRuntimeCapabilities Capabilities;  ///< Runtime IddCx feature set for this device.
  RenderAdapterDesc RenderAdapter;  ///< Requested and OS-assigned render adapter state.
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorObjectContext, GetMonitorObjectContext);

/**
 * @brief Create and configure the VirtualDisplay IddCx WDF device.
 *
 * @param Driver WDF driver object that owns the device.
 * @param DeviceInit WDF device initialization object.
 * @return `STATUS_SUCCESS` on success, otherwise a WDF or IddCx error.
 */
NTSTATUS DriverDeviceAdd(
  _In_ WDFDRIVER Driver,
  _Inout_ PWDFDEVICE_INIT DeviceInit
);

/**
 * @brief Initialize the IddCx adapter when the device enters D0.
 *
 * @param Device WDF device entering the fully powered state.
 * @param PreviousState Previous WDF power state.
 * @return `STATUS_SUCCESS` on success, otherwise an IddCx adapter initialization error.
 */
NTSTATUS DriverDeviceD0Entry(
  _In_ WDFDEVICE Device,
  _In_ WDF_POWER_DEVICE_STATE PreviousState
);

/**
 * @brief Release driver-global monitor state when the WDF driver unloads.
 *
 * @param Driver WDF driver being unloaded.
 */
VOID DriverUnload(
  _In_ WDFDRIVER Driver
);

/**
 * @brief Handle IOCTL requests from the VirtualDisplay control tool.
 *
 * @param Device WDF device that received the request.
 * @param Request WDF request to complete.
 * @param OutputBufferLength Output buffer size in bytes.
 * @param InputBufferLength Input buffer size in bytes.
 * @param IoControlCode IOCTL control code.
 */
VOID IddDeviceIoControl(
  _In_ WDFDEVICE Device,
  _In_ WDFREQUEST Request,
  _In_ size_t OutputBufferLength,
  _In_ size_t InputBufferLength,
  _In_ ULONG IoControlCode
);

/**
 * @brief Report fallback and current dynamic monitor modes for an IddCx monitor description.
 *
 * @param pInArgs Monitor description and output mode buffer supplied by IddCx.
 * @param pOutArgs Receives the number of reported modes and preferred mode index.
 * @return Always `STATUS_SUCCESS`.
 */
NTSTATUS IddParseMonitorDescription(
  _In_ const IDARG_IN_PARSEMONITORDESCRIPTION *pInArgs,
  _Out_ IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs
);

/**
 * @brief Handle completion of asynchronous IddCx adapter initialization.
 *
 * @param AdapterObject Adapter object whose initialization completed.
 * @param pInArgs Initialization status supplied by IddCx.
 * @return Adapter initialization status from IddCx.
 */
NTSTATUS IddAdapterInitFinished(
  _In_ IDDCX_ADAPTER AdapterObject,
  _In_ const IDARG_IN_ADAPTER_INIT_FINISHED *pInArgs
);

/**
 * @brief Acknowledge mode commits requested by IddCx.
 *
 * @param AdapterObject Adapter object receiving the committed paths.
 * @param pInArgs Committed path list supplied by IddCx.
 * @return Always `STATUS_SUCCESS`.
 */
NTSTATUS IddAdapterCommitModes(
  _In_ IDDCX_ADAPTER AdapterObject,
  _In_ const IDARG_IN_COMMITMODES *pInArgs
);

#if IDDCX_VERSION_MINOR >= 0xA
/**
 * @brief Report IddCx 1.10 target capabilities for a virtual connector.
 *
 * @param AdapterObject Adapter object receiving the query.
 * @param pInArgs Target connector query input.
 * @param pOutArgs Receives target color and dithering capabilities.
 * @return `STATUS_SUCCESS` when the connector index is valid.
 */
NTSTATUS IddAdapterQueryTargetInfo(
  _In_ IDDCX_ADAPTER AdapterObject,
  _In_ IDARG_IN_QUERYTARGET_INFO *pInArgs,
  _Out_ IDARG_OUT_QUERYTARGET_INFO *pOutArgs
);

/**
 * @brief Acknowledge IddCx 1.10 mode commits requested by the OS.
 *
 * @param AdapterObject Adapter object receiving the committed paths.
 * @param pInArgs Committed path list and wire-format details supplied by IddCx.
 * @return Always `STATUS_SUCCESS`.
 */
NTSTATUS IddAdapterCommitModes2(
  _In_ IDDCX_ADAPTER AdapterObject,
  _In_ const IDARG_IN_COMMITMODES2 *pInArgs
);

/**
 * @brief Report IddCx 1.10 monitor modes for a monitor description.
 *
 * @param pInArgs Monitor description and v2 output mode buffer supplied by IddCx.
 * @param pOutArgs Receives the number of reported modes and preferred mode index.
 * @return Always `STATUS_SUCCESS`.
 */
NTSTATUS IddParseMonitorDescription2(
  _In_ const IDARG_IN_PARSEMONITORDESCRIPTION2 *pInArgs,
  _Out_ IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs
);

/**
 * @brief Accept IddCx 1.10 HDR gamma-ramp state for a virtual monitor.
 *
 * Console drivers receive a 3x4 colorspace transform when an HDR mode is
 * committed. A virtual display has no physical panel, so the transform is
 * validated and recorded only.
 *
 * @param MonitorObject IddCx monitor receiving the gamma state.
 * @param pInArgs Gamma ramp type and payload supplied by IddCx.
 * @return `STATUS_SUCCESS` when the payload is well formed.
 */
NTSTATUS IddMonitorSetGammaRamp(
  _In_ IDDCX_MONITOR MonitorObject,
  _In_ const IDARG_IN_SET_GAMMARAMP *pInArgs
);

/**
 * @brief Store the IddCx 1.10 default HDR metadata for a virtual monitor.
 *
 * The OS provides the default SMPTE ST.2086 HDR metadata block once for each
 * monitor. The driver records it; per-frame metadata arrives with each buffer
 * through IddCxSwapChainReleaseAndAcquireBuffer2.
 *
 * @param MonitorObject IddCx monitor receiving the metadata.
 * @param pInArgs Default HDR metadata block supplied by IddCx.
 * @return `STATUS_SUCCESS` when the metadata block is well formed.
 */
NTSTATUS IddMonitorSetDefaultHdrMetadata(
  _In_ IDDCX_MONITOR MonitorObject,
  _In_ const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *pInArgs
);
#endif

/**
 * @brief Report the physical monitor size for an EDID-less virtual monitor.
 *
 * The OS calls this callback when the driver does not supply a monitor
 * description with a physical size.
 *
 * @param MonitorObject IddCx monitor being queried.
 * @param pOutArgs Receives the physical width and height in millimeters.
 * @return `STATUS_SUCCESS` when a size is reported.
 */
NTSTATUS IddMonitorGetPhysicalSize(
  _In_ IDDCX_MONITOR MonitorObject,
  _Out_ IDARG_OUT_MONITORGETPHYSICALSIZE *pOutArgs
);

/**
 * @brief Attach an OS-provided swapchain to a virtual monitor.
 *
 * @param MonitorObject IddCx monitor receiving the swapchain.
 * @param pInArgs Swapchain and render adapter data supplied by IddCx.
 * @return `STATUS_SUCCESS` on success or a monitor lookup error.
 */
NTSTATUS IddMonitorAssignSwapChain(
  _In_ IDDCX_MONITOR MonitorObject,
  _In_ const IDARG_IN_SETSWAPCHAIN *pInArgs
);

/**
 * @brief Detach and stop processing the active monitor swapchain.
 *
 * @param MonitorObject IddCx monitor whose swapchain is being removed.
 * @return `STATUS_SUCCESS` on success or a monitor lookup error.
 */
NTSTATUS IddMonitorUnassignSwapChain(
  _In_ IDDCX_MONITOR MonitorObject
);

/**
 * @brief Report target modes for an active virtual monitor.
 *
 * @param MonitorObject IddCx monitor being queried.
 * @param pInArgs Target mode buffer supplied by IddCx.
 * @param pOutArgs Receives the target mode count.
 * @return `STATUS_SUCCESS` on success or a monitor lookup error.
 */
NTSTATUS IddMonitorQueryTargetModes(
  _In_ IDDCX_MONITOR MonitorObject,
  _In_ const IDARG_IN_QUERYTARGETMODES *pInArgs,
  _Out_ IDARG_OUT_QUERYTARGETMODES *pOutArgs
);

#if IDDCX_VERSION_MINOR >= 0xA
/**
 * @brief Report IddCx 1.10 target modes for an active virtual monitor.
 *
 * @param MonitorObject IddCx monitor being queried.
 * @param pInArgs Target mode buffer supplied by IddCx.
 * @param pOutArgs Receives the target mode count.
 * @return `STATUS_SUCCESS` on success or a monitor lookup error.
 */
NTSTATUS IddMonitorQueryTargetModes2(
  _In_ IDDCX_MONITOR MonitorObject,
  _In_ const IDARG_IN_QUERYTARGETMODES2 *pInArgs,
  _Out_ IDARG_OUT_QUERYTARGETMODES *pOutArgs
);
#endif

/**
 * @brief Report default monitor description modes for a virtual monitor.
 *
 * @param MonitorObject IddCx monitor being queried.
 * @param pInArgs Default mode buffer supplied by IddCx.
 * @param pOutArgs Receives the default mode count and preferred mode index.
 * @return `STATUS_SUCCESS` on success or a monitor lookup error.
 */
NTSTATUS IddMonitorGetDefaultDescModes(
  _In_ IDDCX_MONITOR MonitorObject,
  _In_ const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *pInArgs,
  _Out_ IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *pOutArgs
);

/**
 * @brief Reserved WDF timer callback declaration.
 *
 * @param Timer WDF timer object.
 */
VOID IddTimerFunc(_In_ WDFTIMER Timer);
