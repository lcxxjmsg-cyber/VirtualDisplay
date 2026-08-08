#include "Device.h"

#include "DebugLog.h"
#include "DisplayConfigPolicy.h"
#include "Trace.h"

#include <dxgi1_6.h>

#define MAX_ADVERTISED_MODE_COUNT 16
#define SIGNAL_FREQ_DENOMINATOR 1000

static DeviceContext *g_DeviceContext = nullptr;
static constexpr INT32 DEFAULT_WIDTH = 1920;
static constexpr INT32 DEFAULT_HEIGHT = 1080;
static constexpr INT32 DEFAULT_VSYNC = 60000;
static constexpr UINT32 DEFAULT_MONITOR_SCALE_FACTOR = 100;
static constexpr UINT32 MIN_MODE_WIDTH = 320;
static constexpr UINT32 MIN_MODE_HEIGHT = 200;
static constexpr UINT32 MIN_MODE_VSYNC = 24000;
static constexpr UINT32 MAX_MODE_VSYNC = 1000000;

/**
 * @brief Pixel mode advertised to Windows for monitor and target mode lists.
 */
struct MonitorModeSpec {
  INT32 Width;  ///< Width in pixels.
  INT32 Height;  ///< Height in pixels.
  INT32 VSync;  ///< Refresh rate in millihertz.
};

/**
 * @brief Stable console-session modes kept across dynamic updates.
 *
 * GDI enumerates modes from the monitor description, so this table must cover
 * every resolution the control tool can switch to. Resolutions here appear in
 * the Display Settings page and are accepted by ChangeDisplaySettingsEx.
 */
static constexpr MonitorModeSpec BUILTIN_MODE_SPECS[] = {
  {1280, 720, 60000},
  {1280, 720, 120000},
  {1920, 1080, 60000},
  {1920, 1080, 120000},
  {2560, 1080, 60000},
  {2560, 1080, 120000},
  {2560, 1440, 60000},
  {2560, 1440, 120000},
  {3440, 1440, 60000},
  {3440, 1440, 100000},
  {3840, 2160, 60000},
  {3840, 2160, 120000},
};

/**
 * @brief EDID describing the VirtualDisplay monitor.
 *
 * This SDR-only descriptor intentionally carries no HDR static metadata.
 * Windows then treats the virtual monitor as a plain 8-bit SDR display and
 * the captured stream matches the RDS 8-bit path. The HDR-capable CTA block
 * is kept in the repository history; re-enable it only after the Sunshine
 * HDR capture path (YUV 4:4:4 -> 4:2:0) is fixed.
 *
 * Layout: 128-byte base block (DTD 1920x1080@60 and 3840x2160@60, monitor
 * name "VirtualDisp") followed by a 128-byte CTA-861 extension block with
 * audio, video, YCbCr 4:2:0 and colorimetry data blocks plus detailed
 * timing descriptors. Both checksums are valid.
 */
static constexpr BYTE VIRTUALDISPLAY_EDID[256] = {
  0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
  0x63, 0x54, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x24, 0x01, 0x04, 0x80, 0x30, 0x1B, 0x78,
  0x90, 0xBB, 0x46, 0xA3, 0x54, 0x4C, 0x99, 0x26,
  0x0F, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01,
  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3A,
  0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x94,
  0x45, 0x3C, 0x22, 0x1E, 0x00, 0x00, 0x00, 0x00,
  0x08, 0xE8, 0x00, 0x30, 0xF2, 0x70, 0x5A, 0x80,
  0x58, 0xB0, 0x8A, 0x7A, 0x45, 0x1E, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x56,
  0x69, 0x72, 0x74, 0x75, 0x61, 0x6C, 0x44, 0x69,
  0x73, 0x70, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFD,
  0x00, 0x32, 0x3C, 0x46, 0x60, 0x1E, 0x00, 0x0A,
  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x7C,
  0x02, 0x03, 0x44, 0xF0, 0x51, 0x5D, 0x5E, 0x5F,
  0x60, 0x61, 0x10, 0x1F, 0x22, 0x21, 0x20, 0x05,
  0x14, 0x04, 0x13, 0x12, 0x03, 0x01, 0x23, 0x0F,
  0x56, 0x05, 0x83, 0x0F, 0x08, 0x00, 0x6D, 0x03,
  0x0C, 0x00, 0x10, 0x00, 0x38, 0x78, 0x20, 0x00,
  0x60, 0x01, 0x02, 0x03, 0x67, 0xD8, 0x5D, 0xC4,
  0x01, 0x78, 0x80, 0x03, 0xE3, 0x05, 0xE0, 0x01,
  0xE4, 0x0F, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA,
};

/**
 * @brief Pipeline bandwidth value used when the adapter has no bandwidth limit.
 */
static constexpr UINT64 UNRESTRICTED_PIPELINE_BANDWIDTH = 0;

/**
 * @brief NTSTATUS returned when a VidPN path does not accept the requested modality.
 */
static constexpr NTSTATUS STATUS_VIRTUALDISPLAY_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED = static_cast<NTSTATUS>(0xC01E0306u);

static constexpr wchar_t ENDPOINT_FRIENDLY_NAME[] = L"VirtualDisplay Virtual Monitor";
static constexpr wchar_t ENDPOINT_MODEL_NAME[] = L"VirtualDisplay Virtual Display Adapter";
static constexpr wchar_t ENDPOINT_MANUFACTURER_NAME[] = L"VirtualDisplay Project";

static IDDCX_ENDPOINT_VERSION g_EndpointVersion = {
  sizeof(IDDCX_ENDPOINT_VERSION),
  1,
  0,
  0,
  0,
};

static IDDCX_ENDPOINT_DIAGNOSTIC_INFO g_EndpointDiagnostics = {
  sizeof(IDDCX_ENDPOINT_DIAGNOSTIC_INFO),
  IDDCX_TRANSMISSION_TYPE_WIRED_OTHER,
  ENDPOINT_FRIENDLY_NAME,
  ENDPOINT_MODEL_NAME,
  ENDPOINT_MANUFACTURER_NAME,
  &g_EndpointVersion,
  &g_EndpointVersion,
  IDDCX_FEATURE_IMPLEMENTATION_NONE,
};

/**
 * @brief Stable per-connector container IDs reported to IddCx.
 */
static const GUID g_MonitorContainerIds[MAX_MONITOR_COUNT] = {
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x01}},
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x02}},
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x03}},
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x04}},
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x05}},
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x06}},
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x07}},
  {0x8dc6a7b1, 0x82f4, 0x4a2a, {0x8d, 0x34, 0x28, 0x71, 0x9f, 0x64, 0x00, 0x08}},
};

/**
 * @brief Return the IddCx adapter flags for this driver build.
 *
 * @return Adapter flags advertised to IddCx during adapter initialization.
 */
static IDDCX_ADAPTER_FLAGS
  AdapterFlags(
    _In_ const IddRuntimeCapabilities *capabilities
  ) {
  UNREFERENCED_PARAMETER(capabilities);
  IDDCX_ADAPTER_FLAGS flags = IDDCX_ADAPTER_FLAGS_NONE;
  #if IDDCX_VERSION_MINOR >= 0x8
  if (capabilities && capabilities->PrecisePresentRegions) {
    flags = static_cast<IDDCX_ADAPTER_FLAGS>(flags | IDDCX_ADAPTER_FLAGS_PREFER_PRECISE_PRESENT_REGIONS);
  }
  #endif
  #if IDDCX_VERSION_MINOR >= 0xA
  if (capabilities && capabilities->Hdr10) {
    flags = static_cast<IDDCX_ADAPTER_FLAGS>(flags | IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16);
  }
  #endif
  return flags;
}

/**
 * @brief Return whether a DXGI adapter can host VirtualDisplay rendering.
 *
 * @param desc DXGI adapter description.
 * @return True for hardware adapters other than Microsoft Basic Render Driver.
 */
static bool
  IsHardwareRenderAdapter(
    _In_ const DXGI_ADAPTER_DESC1 &desc
  ) {
  const bool basic_render = desc.VendorId == 0x1414 && desc.DeviceId == 0x008C;
  return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && !basic_render;
}

/**
 * @brief Return whether an adapter is usable as the last-resort fallback.
 *
 * Virtualized environments (Hyper-V, GPU-P hosts) may expose only the
 * Microsoft Basic Render Driver or a virtual display adapter. Automatic
 * selection prefers real hardware but falls back to any non-software adapter.
 *
 * @param desc DXGI adapter description.
 * @return True when the adapter is not a software rasterizer.
 */
static bool
  IsUsableFallbackAdapter(
    _In_ const DXGI_ADAPTER_DESC1 &desc
  ) {
  return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
}

/**
 * @brief Match a DXGI adapter against a GPU selection request.
 *
 * @param desc Candidate DXGI adapter description.
 * @param request Requested hardware identity.
 * @return True when the candidate represents the requested GPU.
 */
static bool
  RenderAdapterMatches(
    _In_ const DXGI_ADAPTER_DESC1 &desc,
    _In_ const RenderAdapterDesc &request
  ) {
  if (request.VendorId == 0 || request.DeviceId == 0) {
    return false;
  }

  return desc.VendorId == request.VendorId &&
         desc.DeviceId == request.DeviceId &&
         (request.SubSysId == 0 || desc.SubSysId == request.SubSysId) &&
         (request.Revision == 0 || desc.Revision == request.Revision);
}

/**
 * @brief Resolve an automatic or explicit render adapter for the virtual display.
 *
 * The caller-provided LUID is only a hint because a physical GPU can expose a
 * different runtime LUID in different sessions. Hardware identity is used as
 * the safe fallback, while automatic mode follows Windows high-performance
 * order.
 *
 * @param request Automatic or explicit adapter request.
 * @return Add-refed matching adapter, or nullptr when no hardware match exists.
 */
static IDXGIAdapter1 *
  FindRenderAdapter(
    _In_ const RenderAdapterDesc &request
  ) {
  IDXGIFactory6 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }

  IDXGIAdapter1 *selected = nullptr;
  IDXGIAdapter1 *fallback = nullptr;
  const bool automatic = (request.Flags & VD_RENDER_AUTO) != 0;
  if (!automatic && (request.Flags & VD_RENDER_LUID_HINT_VALID) != 0) {
    IDXGIAdapter1 *candidate = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(request.RequestedLuid, IID_PPV_ARGS(&candidate)))) {
      DXGI_ADAPTER_DESC1 desc = {};
      if (SUCCEEDED(candidate->GetDesc1(&desc)) && IsHardwareRenderAdapter(desc) && RenderAdapterMatches(desc, request)) {
        selected = candidate;
      } else {
        candidate->Release();
      }
    }
  }

  for (UINT index = 0; !selected; ++index) {
    IDXGIAdapter1 *candidate = nullptr;
    const HRESULT enum_hr = factory->EnumAdapterByGpuPreference(
      index,
      DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
      IID_PPV_ARGS(&candidate)
    );
    if (enum_hr == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(enum_hr)) {
      break;
    }

    DXGI_ADAPTER_DESC1 desc = {};
    const bool usable = SUCCEEDED(candidate->GetDesc1(&desc)) && IsHardwareRenderAdapter(desc);
    if (usable && (automatic || RenderAdapterMatches(desc, request))) {
      selected = candidate;
    } else {
      if (!fallback && SUCCEEDED(candidate->GetDesc1(&desc)) && IsUsableFallbackAdapter(desc)) {
        fallback = candidate;
      } else {
        candidate->Release();
      }
    }
  }

  if (!selected && automatic && fallback) {
    selected = fallback;
  } else if (fallback && fallback != selected) {
    fallback->Release();
  }

  factory->Release();
  return selected;
}

/**
 * @brief Set the IddCx preferred render adapter and reset actual-assignment state.
 *
 * Microsoft documents this as a preference: Windows can override it and later
 * reports the actual swapchain adapter through EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN.
 *
 * @param ctx Device instance whose render preference is updated.
 * @param request Automatic or explicit adapter request.
 * @return STATUS_SUCCESS when a hardware adapter was selected.
 */
static NTSTATUS
  SetPreferredRenderAdapter(
    _Inout_ DeviceContext *ctx,
    _In_ const RenderAdapterDesc &request
  ) {
  if (!ctx || !ctx->IddCxAdapter) {
    return STATUS_INVALID_DEVICE_STATE;
  }

  IDXGIAdapter1 *selected = FindRenderAdapter(request);
  if (!selected) {
    VdIddLog(
      "SetPreferredRenderAdapter: no match flags=0x%08X vendor=0x%04X device=0x%04X",
      request.Flags,
      request.VendorId,
      request.DeviceId
    );
    return STATUS_NOT_FOUND;
  }

  DXGI_ADAPTER_DESC1 desc = {};
  selected->GetDesc1(&desc);
  selected->Release();

  IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
  args.PreferredRenderAdapter = desc.AdapterLuid;
  IddCxAdapterSetRenderAdapter(ctx->IddCxAdapter, &args);

  RtlZeroMemory(&ctx->RenderAdapter, sizeof(ctx->RenderAdapter));
  ctx->RenderAdapter.Size = sizeof(ctx->RenderAdapter);
  ctx->RenderAdapter.Flags = (request.Flags & VD_RENDER_AUTO) | VD_RENDER_REQUESTED_VALID;
  ctx->RenderAdapter.VendorId = desc.VendorId;
  ctx->RenderAdapter.DeviceId = desc.DeviceId;
  ctx->RenderAdapter.SubSysId = desc.SubSysId;
  ctx->RenderAdapter.Revision = desc.Revision;
  ctx->RenderAdapter.RequestedLuid = desc.AdapterLuid;

  VdIddLog(
    "SetPreferredRenderAdapter: mode=%s vendor=0x%04X device=0x%04X luid=%ld:%lu",
    (request.Flags & VD_RENDER_AUTO) != 0 ? "auto" : "specific",
    desc.VendorId,
    desc.DeviceId,
    desc.AdapterLuid.HighPart,
    desc.AdapterLuid.LowPart
  );
  return STATUS_SUCCESS;
}

/**
 * @brief Query and log the IddCx runtime version and optional DDIs selected for this driver instance.
 *
 * @param source Call-site label written to the diagnostic log.
 * @return Runtime IddCx capabilities for this device instance.
 */
static IddRuntimeCapabilities
  QueryIddCxRuntimeCapabilities(
    _In_z_ const char *source
  ) {
  IddRuntimeCapabilities capabilities = {};
  capabilities.MonitorUpdateModes = TRUE;

  IDARG_OUT_GETVERSION version = {};
  const NTSTATUS status = IddCxGetVersion(&version);
  if (!NT_SUCCESS(status)) {
    VdIddLog("%s: IddCxGetVersion failed status=0x%08X", source, status);
    return capabilities;
  }

  capabilities.RawVersion = version.IddCxVersion;
  capabilities.MajorVersion = (capabilities.RawVersion >> 12) & 0x0F;
  capabilities.MinorVersion = (capabilities.RawVersion >> 8) & 0x0F;
  capabilities.Revision = capabilities.RawVersion & 0xFF;

  #if IDDCX_VERSION_MINOR >= 0x4
  capabilities.AdapterDisplayConfigUpdate = IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterDisplayConfigUpdate);
  #endif
  #if IDDCX_VERSION_MINOR >= 0x6
  capabilities.SwapChainInSystemMemory = IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainInSystemMemory);
  #endif
  #if IDDCX_VERSION_MINOR >= 0x8
  capabilities.HardwareCursor2 = IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor2);
  capabilities.PrecisePresentRegions = capabilities.MajorVersion > 1 || capabilities.MinorVersion >= 8;
  #endif
  #if IDDCX_VERSION_MINOR >= 0xA
  capabilities.AdapterDisplayConfigUpdate2 = IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterDisplayConfigUpdate2);
  capabilities.MonitorUpdateModes2 = IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2);
  capabilities.SwapChainReleaseAndAcquireBuffer2 = IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2);
  capabilities.HardwareCursor3 = IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3);
  const virtualdisplay::iddcx::HdrDdiAvailability hdrDdis = {
    capabilities.MonitorUpdateModes2 != FALSE,
    capabilities.SwapChainReleaseAndAcquireBuffer2 != FALSE,
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2) != FALSE,
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo) != FALSE,
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2) != FALSE,
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorQueryTargetModes2) != FALSE,
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp) != FALSE,
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetDefaultHdrMetaData) != FALSE,
  };
  capabilities.Hdr10 = virtualdisplay::iddcx::SupportsConsoleHdr(hdrDdis) ? TRUE : FALSE;
  #endif

  VdIddLog(
    "%s: IddCxGetVersion status=0x%08X raw=0x%04X public=%u.%u revision=0x%02X",
    source,
    status,
    capabilities.RawVersion,
    capabilities.MajorVersion,
    capabilities.MinorVersion,
    capabilities.Revision
  );
  VdIddLog(
    "%s: capabilities displayConfig=%u displayConfig2=%u updateModes=%u updateModes2=%u sysmem=%u buffer2=%u cursor2=%u cursor3=%u preciseRegions=%u hdr10=%u",
    source,
    capabilities.AdapterDisplayConfigUpdate,
    capabilities.AdapterDisplayConfigUpdate2,
    capabilities.MonitorUpdateModes,
    capabilities.MonitorUpdateModes2,
    capabilities.SwapChainInSystemMemory,
    capabilities.SwapChainReleaseAndAcquireBuffer2,
    capabilities.HardwareCursor2,
    capabilities.HardwareCursor3,
    capabilities.PrecisePresentRegions,
    capabilities.Hdr10
  );
  return capabilities;
}

/**
 * @brief Return the connector technology exposed for newly created monitors.
 *
 * @return DisplayConfig output technology used in IDDCX_MONITOR_INFO.
 */
static DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY
  MonitorOutputTechnology() {
  return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
}

/**
 * @brief Close a Windows handle and reset the variable.
 *
 * @param handle Handle variable to close when it is non-null.
 */
static void
  CloseHandleIfSet(
    _Inout_ HANDLE *handle
  ) {
  if (*handle) {
    CloseHandle(*handle);
    *handle = nullptr;
  }
}

/**
 * @brief Release a COM pointer and reset the variable.
 *
 * @param object COM pointer variable to release when it is non-null.
 */
template<typename T>
static void
  ReleaseIfSet(
    _Inout_ T **object
  ) {
  if (*object) {
    (*object)->Release();
    *object = nullptr;
  }
}

/**
 * @brief Find the monitor context that owns an IddCx monitor object.
 *
 * @param MonitorObject IddCx monitor object to locate.
 * @return Matching monitor context, or `nullptr` when it is not active.
 */
static MonitorContext *
  FindMonitorById(
    _In_ IDDCX_MONITOR MonitorObject
  ) {
  if (!MonitorObject) {
    return nullptr;
  }

  MonitorObjectContext *objectContext = GetMonitorObjectContext(MonitorObject);
  if (objectContext && objectContext->Monitor && objectContext->Monitor->IddCxMonitor == MonitorObject && objectContext->Monitor->InUse) {
    return objectContext->Monitor;
  }

  if (!g_DeviceContext) {
    return nullptr;
  }

  for (UINT32 i = 0; i < MAX_MONITOR_COUNT; i++) {
    if (g_DeviceContext->Monitors[i].InUse && g_DeviceContext->Monitors[i].IddCxMonitor == MonitorObject) {
      return &g_DeviceContext->Monitors[i];
    }
  }
  return nullptr;
}

/**
 * @brief Fill a display signal description with VirtualDisplay's virtual timing.
 *
 * @param signal Output signal description to populate.
 * @param Width Width in pixels.
 * @param Height Height in pixels.
 * @param VSync Refresh rate in millihertz.
 * @param MonitorMode True when the signal is for an `IDDCX_MONITOR_MODE`.
 */
static void
  FillVideoSignalInfo(
    _Out_ DISPLAYCONFIG_VIDEO_SIGNAL_INFO *signal,
    _In_ INT32 Width,
    _In_ INT32 Height,
    _In_ INT32 VSync,
    _In_ bool MonitorMode
  ) {
  const auto width = Width > 0 ? Width : DEFAULT_WIDTH;
  const auto height = Height > 0 ? Height : DEFAULT_HEIGHT;
  const auto refresh = VSync > 0 ? VSync : DEFAULT_VSYNC;

  signal->activeSize.cx = width;
  signal->activeSize.cy = height;
  signal->totalSize.cx = width;
  signal->totalSize.cy = height;
  signal->hSyncFreq.Numerator = static_cast<UINT32>(refresh) * static_cast<UINT32>(height);
  signal->hSyncFreq.Denominator = SIGNAL_FREQ_DENOMINATOR;
  signal->vSyncFreq.Numerator = static_cast<UINT32>(refresh);
  signal->vSyncFreq.Denominator = SIGNAL_FREQ_DENOMINATOR;
  signal->AdditionalSignalInfo.vSyncFreqDivider = MonitorMode ? 0 : 1;
  signal->AdditionalSignalInfo.videoStandard = 255;
  signal->scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  signal->pixelRate =
    static_cast<UINT64>(width) *
    static_cast<UINT64>(height) *
    static_cast<UINT64>(refresh) /
    SIGNAL_FREQ_DENOMINATOR;
}

/**
 * @brief Fill an IddCx monitor mode with VirtualDisplay's fixed virtual timing.
 *
 * @param mode Output monitor mode to populate.
 * @param Width Width in pixels.
 * @param Height Height in pixels.
 * @param VSync Refresh rate in millihertz.
 * @param Origin Mode origin reported to IddCx.
 */
static void
  FillMonitorMode(
    _Out_ IDDCX_MONITOR_MODE *mode,
    _In_ INT32 Width,
    _In_ INT32 Height,
    _In_ INT32 VSync,
    _In_ IDDCX_MONITOR_MODE_ORIGIN Origin
  ) {
  mode->Size = IDD_STRUCTURE_SIZE(IDDCX_MONITOR_MODE);
  mode->Origin = Origin;
  FillVideoSignalInfo(&mode->MonitorVideoSignalInfo, Width, Height, VSync, true);
}

/**
 * @brief Fill an IddCx target mode with VirtualDisplay's virtual timing.
 *
 * @param mode Output target mode to populate.
 * @param Width Width in pixels.
 * @param Height Height in pixels.
 * @param VSync Refresh rate in millihertz.
 */
static void
  FillTargetMode(
    _Out_ IDDCX_TARGET_MODE *mode,
    _In_ INT32 Width,
    _In_ INT32 Height,
    _In_ INT32 VSync
  ) {
  mode->Size = IDD_STRUCTURE_SIZE(IDDCX_TARGET_MODE);
  FillVideoSignalInfo(&mode->TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);
  mode->RequiredBandwidth = UNRESTRICTED_PIPELINE_BANDWIDTH;
}

  #if IDDCX_VERSION_MINOR >= 0xA
/**
 * @brief Fill IddCx 1.10 wire-format bits for RGB output.
 *
 * @param bits Output wire bits structure to populate.
 * @param hdr10 True when the runtime is IddCx 1.10 and HDR10 modes should be advertised.
 */
static void
  FillWireBits(
    _Out_ IDDCX_WIRE_BITS_PER_COMPONENT *bits,
    _In_ bool hdr10
  ) {
  bits->Rgb = hdr10 ?
                static_cast<IDDCX_BITS_PER_COMPONENT>(IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10) :
                IDDCX_BITS_PER_COMPONENT_8;
  bits->YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits->YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits->YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
}

/**
 * @brief Fill an IddCx 1.10 monitor mode with SDR/HDR wire format information.
 *
 * @param mode Output monitor mode to populate.
 * @param Width Width in pixels.
 * @param Height Height in pixels.
 * @param VSync Refresh rate in millihertz.
 * @param Origin Mode origin reported to IddCx.
 * @param hdr10 True when the runtime is IddCx 1.10 and HDR10 modes should be advertised.
 */
static void
  FillMonitorMode2(
    _Out_ IDDCX_MONITOR_MODE2 *mode,
    _In_ INT32 Width,
    _In_ INT32 Height,
    _In_ INT32 VSync,
    _In_ IDDCX_MONITOR_MODE_ORIGIN Origin,
    _In_ bool hdr10
  ) {
  mode->Size = sizeof(IDDCX_MONITOR_MODE2);
  mode->Origin = Origin;
  FillVideoSignalInfo(&mode->MonitorVideoSignalInfo, Width, Height, VSync, true);
  FillWireBits(&mode->BitsPerComponent, hdr10);
}

/**
 * @brief Fill an IddCx 1.10 target mode with SDR/HDR wire format information.
 *
 * @param mode Output target mode to populate.
 * @param Width Width in pixels.
 * @param Height Height in pixels.
 * @param VSync Refresh rate in millihertz.
 * @param hdr10 True when the runtime is IddCx 1.10 and HDR10 modes should be advertised.
 */
static void
  FillTargetMode2(
    _Out_ IDDCX_TARGET_MODE2 *mode,
    _In_ INT32 Width,
    _In_ INT32 Height,
    _In_ INT32 VSync,
    _In_ bool hdr10
  ) {
  mode->Size = sizeof(IDDCX_TARGET_MODE2);
  FillVideoSignalInfo(&mode->TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);
  mode->RequiredBandwidth = UNRESTRICTED_PIPELINE_BANDWIDTH;
  FillWireBits(&mode->BitsPerComponent, hdr10);
}
  #endif

/**
 * @brief Validate a requested virtual monitor mode before advertising it to IddCx.
 *
 * @param desc Mode request to validate.
 * @return True when the mode is within the driver's broad virtual-display bounds.
 */
static bool
  IsValidMonitorMode(
    _In_ const MonitorDesc *desc
  ) {
  return desc &&
         desc->Width >= MIN_MODE_WIDTH &&
         desc->Height >= MIN_MODE_HEIGHT &&
         desc->VSync >= MIN_MODE_VSYNC &&
         desc->VSync <= MAX_MODE_VSYNC;
}

/**
 * @brief Normalize optional monitor geometry fields against defaults or an existing monitor.
 *
 * @param input Requested monitor geometry.
 * @param existing Existing monitor state, or `nullptr` when creating a monitor.
 * @return Normalized monitor descriptor with non-zero width, height, and refresh rate.
 */
static MonitorDesc
  NormalizeMonitorDesc(
    _In_opt_ const MonitorDesc *input,
    _In_opt_ const MonitorContext *existing
  ) {
  MonitorDesc desc = {};
  if (input) {
    desc = *input;
  }

  desc.Width = desc.Width ? desc.Width : static_cast<UINT32>(existing ? existing->Width : DEFAULT_WIDTH);
  desc.Height = desc.Height ? desc.Height : static_cast<UINT32>(existing ? existing->Height : DEFAULT_HEIGHT);
  desc.VSync = desc.VSync ? desc.VSync : static_cast<UINT32>(existing ? existing->VSync : DEFAULT_VSYNC);
  return desc;
}

/**
 * @brief Return whether two mode specs describe the same pixel mode.
 *
 * @param left First mode.
 * @param right Second mode.
 * @return True when width, height, and refresh are equal.
 */
static bool
  SameModeSpec(
    _In_ const MonitorModeSpec &left,
    _In_ const MonitorModeSpec &right
  ) {
  return left.Width == right.Width &&
         left.Height == right.Height &&
         left.VSync == right.VSync;
}

/**
 * @brief Return whether a mode spec is inside the virtual-display policy bounds.
 *
 * @param mode Mode spec to validate.
 * @return True when the mode is allowed.
 */
static bool
  IsValidModeSpec(
    _In_ const MonitorModeSpec &mode
  ) {
  MonitorDesc desc = {};
  desc.Width = static_cast<UINT32>(mode.Width);
  desc.Height = static_cast<UINT32>(mode.Height);
  desc.VSync = static_cast<UINT32>(mode.VSync);
  return IsValidMonitorMode(&desc);
}

/**
 * @brief Append a mode to an advertised mode list if it is valid and unique.
 *
 * @param modes Output mode array.
 * @param maxCount Capacity of the output mode array.
 * @param count Current number of populated modes.
 * @param mode Mode to append.
 * @return Index of the existing or appended mode, or `NO_PREFERRED_MODE` when not added.
 */
static UINT32
  AddAdvertisedModeSpec(
    _Out_writes_(maxCount) MonitorModeSpec *modes,
    _In_ UINT32 maxCount,
    _Inout_ UINT32 *count,
    _In_ const MonitorModeSpec &mode
  ) {
  if (!modes || !count || !IsValidModeSpec(mode)) {
    return NO_PREFERRED_MODE;
  }

  for (UINT32 i = 0; i < *count; i++) {
    if (SameModeSpec(modes[i], mode)) {
      return i;
    }
  }

  if (*count >= maxCount) {
    return NO_PREFERRED_MODE;
  }

  modes[*count] = mode;
  (*count)++;
  return *count - 1;
}

/**
 * @brief Build the monitor and target mode list exposed to IddCx.
 *
 * @param monCtx Optional monitor whose current dynamic mode should be included.
 * @param modes Output mode array.
 * @param maxCount Capacity of the output mode array.
 * @param preferredIndex Receives the preferred mode index.
 * @return Number of modes populated.
 */
static UINT32
  BuildAdvertisedModeSpecs(
    _In_opt_ const MonitorContext *monCtx,
    _Out_writes_(maxCount) MonitorModeSpec *modes,
    _In_ UINT32 maxCount,
    _Out_opt_ UINT32 *preferredIndex
  ) {
  UINT32 count = 0;
  UINT32 preferred = NO_PREFERRED_MODE;
  const MonitorModeSpec preferredMode = {
    DEFAULT_WIDTH,
    DEFAULT_HEIGHT,
    DEFAULT_VSYNC,
  };

  for (UINT32 i = 0; i < ARRAYSIZE(BUILTIN_MODE_SPECS); i++) {
    const UINT32 index = AddAdvertisedModeSpec(modes, maxCount, &count, BUILTIN_MODE_SPECS[i]);
    if (preferred == NO_PREFERRED_MODE && index != NO_PREFERRED_MODE && SameModeSpec(BUILTIN_MODE_SPECS[i], preferredMode)) {
      preferred = index;
    }
  }

  if (monCtx) {
    const MonitorModeSpec currentMode = {
      monCtx->Width,
      monCtx->Height,
      monCtx->VSync,
    };
    const UINT32 index = AddAdvertisedModeSpec(modes, maxCount, &count, currentMode);
    if (index != NO_PREFERRED_MODE) {
      preferred = index;
    }
  }

  if (preferredIndex) {
    *preferredIndex = preferred;
  }
  return count;
}

/**
 * @brief Create a D3D11 processing device for an IddCx swapchain.
 *
 * @param renderAdapterLuid Adapter LUID supplied by IddCx.
 * @param device Receives a referenced D3D device on success.
 * @return `S_OK` on success, otherwise the D3D or DXGI failure code.
 */
static HRESULT
  CreateProcessingDevice(
    _In_ LUID renderAdapterLuid,
    _COM_Outptr_ ID3D11Device **device
  ) {
  *device = nullptr;

  D3D_FEATURE_LEVEL featureLevels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
  };

  D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_11_0;
  IDXGIFactory4 *factory = nullptr;
  IDXGIAdapter *adapter = nullptr;

  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&factory));
  if (SUCCEEDED(hr)) {
    hr = factory->EnumAdapterByLuid(renderAdapterLuid, __uuidof(IDXGIAdapter), reinterpret_cast<void **>(&adapter));
  }

  if (SUCCEEDED(hr)) {
    hr = D3D11CreateDevice(
      adapter,
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      featureLevels,
      ARRAYSIZE(featureLevels),
      D3D11_SDK_VERSION,
      device,
      &selectedLevel,
      nullptr
    );
  }

  ReleaseIfSet(&adapter);
  ReleaseIfSet(&factory);

  if (SUCCEEDED(hr)) {
    return hr;
  }

  return D3D11CreateDevice(
    nullptr,
    D3D_DRIVER_TYPE_WARP,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    featureLevels,
    ARRAYSIZE(featureLevels),
    D3D11_SDK_VERSION,
    device,
    &selectedLevel,
    nullptr
  );
}

/**
 * @brief Worker that consumes IddCx frames so UMDF observes forward progress.
 *
 * @param context MonitorContext that owns the assigned swapchain.
 * @return Always zero.
 */
static DWORD WINAPI
  SwapChainThreadProc(
    _In_ LPVOID context
  ) {
  auto *monCtx = static_cast<MonitorContext *>(context);
  HANDLE waitHandles[] = {
    monCtx->hNextSurfaceEvent,
    monCtx->SwapChainTerminateEvent,
  };

  while (WaitForSingleObject(monCtx->SwapChainTerminateEvent, 0) != WAIT_OBJECT_0) {
    HRESULT hr = E_FAIL;
    IDXGIResource *surface = nullptr;
  #if IDDCX_VERSION_MINOR >= 0xA
    if (monCtx->UseHdrSwapChain) {
      IDARG_IN_RELEASEANDACQUIREBUFFER2 input = {};
      input.Size = sizeof(input);
      input.AcquireSystemMemoryBuffer = FALSE;
      IDARG_OUT_RELEASEANDACQUIREBUFFER2 output = {};
      output.MetaData.Size = sizeof(output.MetaData);
      hr = IddCxSwapChainReleaseAndAcquireBuffer2(monCtx->SwapChain, &input, &output);
      surface = output.MetaData.pSurface;
      const UINT32 validFlags = static_cast<UINT32>(output.MetaData.ValidFlags);
      const UINT32 hdrMetaType = static_cast<UINT32>(output.MetaData.Hdr10FrameMetaData.Type);
      if ((validFlags & IDDCX_METADATA2_VALID_FLAGS_HDR10METADATA) != 0) {
        VdIddLog(
          "SwapChainThreadProc: buffer2 surface=%p colorSpace=%u sdrWhiteLevel=%u hdrMeta=%u",
          output.MetaData.pSurface,
          static_cast<UINT32>(output.MetaData.SurfaceColorSpace),
          output.MetaData.SdrWhiteLevel,
          hdrMetaType
        );
      }
    } else
  #endif
    {
      IDARG_OUT_RELEASEANDACQUIREBUFFER output = {};
      hr = IddCxSwapChainReleaseAndAcquireBuffer(monCtx->SwapChain, &output);
      surface = output.MetaData.pSurface;
    }

    if (hr == E_PENDING) {
      const DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
      if (waitResult == WAIT_OBJECT_0 + 1 || waitResult == WAIT_FAILED) {
        break;
      }
      continue;
    }

    if (FAILED(hr)) {
      break;
    }

    ReleaseIfSet(&surface);
    if (FAILED(IddCxSwapChainFinishedProcessingFrame(monCtx->SwapChain))) {
      break;
    }
  }

  WdfObjectDelete(reinterpret_cast<WDFOBJECT>(monCtx->SwapChain));
  return 0;
}

/**
 * @brief Stop and release an active monitor swapchain worker.
 *
 * @param monCtx Monitor state whose worker should be stopped.
 */
static NTSTATUS
  StopSwapChainProcessor(
    _Inout_ MonitorContext *monCtx
  ) {
  if (monCtx->SwapChainTerminateEvent) {
    SetEvent(monCtx->SwapChainTerminateEvent);
  }

  if (monCtx->SwapChainThread) {
    WaitForSingleObject(monCtx->SwapChainThread, INFINITE);
    CloseHandleIfSet(&monCtx->SwapChainThread);
  } else if (monCtx->SwapChain) {
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(monCtx->SwapChain));
  }

  CloseHandleIfSet(&monCtx->SwapChainTerminateEvent);
  ReleaseIfSet(&monCtx->ProcessingDevice);
  monCtx->UseHdrSwapChain = FALSE;
  monCtx->SwapChain = nullptr;
  monCtx->hNextSurfaceEvent = nullptr;

  return STATUS_SUCCESS;
}

/**
 * @brief Return public capability flags for user-mode diagnostics.
 *
 * @param capabilities Runtime IddCx capability state.
 * @return Bitmask of VD_CAP_* values.
 */
static UINT32
  CapabilityFlags(
    _In_ const IddRuntimeCapabilities *capabilities
  ) {
  UINT32 flags = 0;
  if (!capabilities) {
    return flags;
  }
  if (capabilities->AdapterDisplayConfigUpdate) {
    flags |= VD_CAP_DISPLAY_CONFIG_UPDATE;
  }
  if (capabilities->AdapterDisplayConfigUpdate2) {
    flags |= VD_CAP_DISPLAY_CONFIG_UPDATE2;
  }
  if (capabilities->SwapChainInSystemMemory) {
    flags |= VD_CAP_SYSTEM_MEMORY_SWAPCHAIN;
  }
  if (capabilities->PrecisePresentRegions) {
    flags |= VD_CAP_PRECISE_PRESENT_REGIONS;
  }
  if (capabilities->Hdr10) {
    flags |= VD_CAP_HDR10;
  }
  return flags;
}

/**
 * @brief Fill a user-mode capability descriptor from runtime state.
 *
 * @param capabilities Runtime IddCx capability state.
 * @param output Output descriptor to populate.
 */
static void
  FillCapabilityDesc(
    _In_ const IddRuntimeCapabilities *capabilities,
    _Out_ IddCapabilityDesc *output
  ) {
  output->RuntimeMajor = static_cast<UINT32>(capabilities->MajorVersion);
  output->RuntimeMinor = static_cast<UINT32>(capabilities->MinorVersion);
  output->RuntimeRevision = static_cast<UINT32>(capabilities->Revision);
  output->CapabilityFlags = CapabilityFlags(capabilities);
}

/**
 * @brief Notify IddCx that a monitor's target mode list changed.
 *
 * @param monCtx Monitor with the current mode state.
 * @param source Log label describing the caller.
 * @return `STATUS_SUCCESS` on success, otherwise an IddCx error.
 */
static NTSTATUS
  UpdateMonitorModesLocked(
    _In_ MonitorContext *monCtx,
    _In_z_ const char *source
  ) {
  if (!monCtx || !monCtx->IddCxMonitor) {
    return STATUS_INVALID_DEVICE_STATE;
  }

  #if IDDCX_VERSION_MINOR >= 0xA
  if (g_DeviceContext && g_DeviceContext->Capabilities.MonitorUpdateModes2) {
    MonitorModeSpec modeSpecs[MAX_ADVERTISED_MODE_COUNT] = {};
    const UINT32 modeCount = BuildAdvertisedModeSpecs(monCtx, modeSpecs, ARRAYSIZE(modeSpecs), nullptr);
    if (modeCount == 0) {
      return STATUS_INVALID_PARAMETER;
    }

    IDDCX_TARGET_MODE2 targetModes2[MAX_ADVERTISED_MODE_COUNT] = {};
    const bool hdr10 = g_DeviceContext && g_DeviceContext->Capabilities.Hdr10;
    for (UINT32 i = 0; i < modeCount; i++) {
      FillTargetMode2(&targetModes2[i], modeSpecs[i].Width, modeSpecs[i].Height, modeSpecs[i].VSync, hdr10);
    }

    IDARG_IN_UPDATEMODES2 updateModes2 = {};
    updateModes2.Reason = IDDCX_UPDATE_REASON_CONFIGURATION_CONSTRAINTS;
    updateModes2.TargetModeCount = modeCount;
    updateModes2.pTargetModes = targetModes2;

    const NTSTATUS status2 = IddCxMonitorUpdateModes2(monCtx->IddCxMonitor, &updateModes2);
    VdIddLog(
      "%s: IddCxMonitorUpdateModes2 status=0x%08X monitor=%p modes=%u width=%d height=%d vsync=%d",
      source,
      status2,
      monCtx->IddCxMonitor,
      modeCount,
      monCtx->Width,
      monCtx->Height,
      monCtx->VSync
    );
    if (NT_SUCCESS(status2)) {
      return status2;
    }
  }
  #endif

  MonitorModeSpec modeSpecs[MAX_ADVERTISED_MODE_COUNT] = {};
  const UINT32 modeCount = BuildAdvertisedModeSpecs(monCtx, modeSpecs, ARRAYSIZE(modeSpecs), nullptr);
  if (modeCount == 0) {
    return STATUS_INVALID_PARAMETER;
  }

  IDDCX_TARGET_MODE targetModes[MAX_ADVERTISED_MODE_COUNT] = {};
  for (UINT32 i = 0; i < modeCount; i++) {
    FillTargetMode(&targetModes[i], modeSpecs[i].Width, modeSpecs[i].Height, modeSpecs[i].VSync);
  }

  IDARG_IN_UPDATEMODES updateModes = {};
  updateModes.Reason = IDDCX_UPDATE_REASON_CONFIGURATION_CONSTRAINTS;
  updateModes.TargetModeCount = modeCount;
  updateModes.pTargetModes = targetModes;

  const NTSTATUS status = IddCxMonitorUpdateModes(monCtx->IddCxMonitor, &updateModes);
  VdIddLog(
    "%s: IddCxMonitorUpdateModes status=0x%08X monitor=%p modes=%u width=%d height=%d vsync=%d",
    source,
    status,
    monCtx->IddCxMonitor,
    modeCount,
    monCtx->Width,
    monCtx->Height,
    monCtx->VSync
  );
  return status;
}

/**
 * @brief Apply the current monitor layout to an IddCx adapter.
 *
 * Console root IDDs do not own the remote-session desktop configuration, so
 * display-config updates are best-effort and their failure is tolerated by the
 * mode-update path.
 *
 * @param ctx Device state whose active monitors form the display config.
 * @param source Log label describing the caller.
 * @return `STATUS_SUCCESS` on success, otherwise an IddCx error.
 */
static NTSTATUS
  ApplyRemoteDisplayConfigLocked(
    _In_ DeviceContext *ctx,
    _In_z_ const char *source
  ) {
  UNREFERENCED_PARAMETER(ctx);
  UNREFERENCED_PARAMETER(source);
  VdIddLog("%s: display config update unsupported on console adapter", source);
  return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Decide whether a display-config update failure can be ignored.
 *
 * @param status Status returned by an IddCx display-config update call.
 * @return True when the monitor mode update should still be kept.
 */
static bool
  IsOptionalDisplayConfigFailure(
    _In_ NTSTATUS status
  ) {
  return status == STATUS_NOT_SUPPORTED ||
         status == STATUS_INVALID_PARAMETER ||
         status == STATUS_VIRTUALDISPLAY_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
}

/**
 * @brief Update an active monitor's mode and ask IddCx to reconfigure the path.
 *
 * @param ctx Device state that owns the monitor.
 * @param inputDesc Requested monitor mode and one-based monitor index.
 * @param outputDesc Optional descriptor receiving the normalized mode.
 * @return `STATUS_SUCCESS` on success, otherwise an IddCx or validation error.
 */
static NTSTATUS
  UpdateMonitorLocked(
    _Inout_ DeviceContext *ctx,
    _In_ const MonitorDesc *inputDesc,
    _Out_opt_ MonitorDesc *outputDesc
  ) {
  if (!ctx || !inputDesc) {
    return STATUS_INVALID_PARAMETER;
  }

  const UINT32 idx = inputDesc->MonitorIndex;
  if (idx == 0 || idx > MAX_MONITOR_COUNT || !ctx->Monitors[idx - 1].InUse) {
    VdIddLog(
      "UpdateMonitorLocked: monitor not found idx=%u max=%u inUse=%d ctx=%p",
      idx,
      MAX_MONITOR_COUNT,
      (idx >= 1 && idx <= MAX_MONITOR_COUNT) ? static_cast<int>(ctx->Monitors[idx - 1].InUse) : -1,
      ctx
    );
    return STATUS_NOT_FOUND;
  }

  MonitorContext *monCtx = &ctx->Monitors[idx - 1];
  MonitorDesc requested = NormalizeMonitorDesc(inputDesc, monCtx);
  requested.MonitorIndex = idx;
  if (!IsValidMonitorMode(&requested)) {
    return STATUS_INVALID_PARAMETER;
  }

  const INT32 oldWidth = monCtx->Width;
  const INT32 oldHeight = monCtx->Height;
  const INT32 oldVSync = monCtx->VSync;

  monCtx->Width = static_cast<INT32>(requested.Width);
  monCtx->Height = static_cast<INT32>(requested.Height);
  monCtx->VSync = static_cast<INT32>(requested.VSync);

  NTSTATUS status = UpdateMonitorModesLocked(monCtx, "UpdateMonitorLocked");
  if (NT_SUCCESS(status)) {
    const NTSTATUS displayConfigStatus = ApplyRemoteDisplayConfigLocked(ctx, "UpdateMonitorLocked");
    if (!NT_SUCCESS(displayConfigStatus)) {
      if (IsOptionalDisplayConfigFailure(displayConfigStatus)) {
        VdIddLog(
          "UpdateMonitorLocked: keeping mode update despite optional display config status=0x%08X",
          displayConfigStatus
        );
      } else {
        status = displayConfigStatus;
      }
    }
  }

  if (!NT_SUCCESS(status)) {
    VdIddLog(
      "UpdateMonitorLocked: rollback index=%u status=0x%08X old=%dx%d@%d new=%ux%u@%u",
      idx,
      status,
      oldWidth,
      oldHeight,
      oldVSync,
      requested.Width,
      requested.Height,
      requested.VSync
    );
    monCtx->Width = oldWidth;
    monCtx->Height = oldHeight;
    monCtx->VSync = oldVSync;
    UpdateMonitorModesLocked(monCtx, "UpdateMonitorLockedRollback");
    ApplyRemoteDisplayConfigLocked(ctx, "UpdateMonitorLockedRollback");
    return status;
  }

  if (outputDesc) {
    *outputDesc = requested;
  }
  VdIddLog(
    "UpdateMonitorLocked: updated index=%u width=%d height=%d vsync=%d",
    idx,
    monCtx->Width,
    monCtx->Height,
    monCtx->VSync
  );
  return STATUS_SUCCESS;
}

/**
 * @brief EDID describing the VirtualDisplay monitor.
 *
 * Windows derives monitor-level HDR capability from the EDID: an HDR10
 * static-metadata data block (CTA-861-G extended tag 0x06, EOTF mask
 * ST 2084 | HLG) is what makes Display Settings and DISPLAYCONFIG report
 * highDynamicRangeSupported. An EDID-less monitor is always treated as
 * SDR-only, so the driver must provide this descriptor for console HDR.
 *
 * Layout: 128-byte base block (DTD 1920x1080@60 and 3840x2160@60, monitor
 * name "VirtualDisp") followed by a 128-byte CTA-861 extension block with a
 * Video Data Block (1080p60 and 4K60 VICs), an HDR static metadata block
 * (MaxCLL 1000 nits, MaxFALL 400 nits, Min 0.0055 nits) and a BT.2020
 * colorimetry block. Both checksums are valid.
 */

static NTSTATUS
  CreateMonitorLocked(
    _Inout_ DeviceContext *ctx,
    _In_ const MonitorDesc *inputDesc,
    _Out_opt_ MonitorDesc *outputDesc
  ) {
  if (!ctx->IddCxAdapter) {
    return STATUS_DEVICE_NOT_READY;
  }

  if (ctx->MonitorCount >= MAX_MONITOR_COUNT) {
    return STATUS_NO_MORE_ENTRIES;
  }

  INT32 slot = -1;
  for (UINT32 i = 0; i < MAX_MONITOR_COUNT; i++) {
    if (!ctx->Monitors[i].InUse) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    return STATUS_NO_MORE_ENTRIES;
  }

  MonitorDesc requested = NormalizeMonitorDesc(inputDesc, nullptr);
  if (!IsValidMonitorMode(&requested)) {
    return STATUS_INVALID_PARAMETER;
  }

  MonitorContext *monCtx = &ctx->Monitors[slot];
  monCtx->Width = static_cast<INT32>(requested.Width);
  monCtx->Height = static_cast<INT32>(requested.Height);
  monCtx->VSync = static_cast<INT32>(requested.VSync);
  monCtx->DisplayConfigInitialized = FALSE;
  VdIddLog(
    "CreateMonitorLocked: slot=%d width=%d height=%d vsync=%d",
    slot,
    monCtx->Width,
    monCtx->Height,
    monCtx->VSync
  );

  IDDCX_MONITOR_INFO info = {};
  info.Size = sizeof(info);
  info.MonitorType = MonitorOutputTechnology();
  info.ConnectorIndex = slot;
  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = sizeof(VIRTUALDISPLAY_EDID);
  info.MonitorDescription.pData = const_cast<BYTE *>(VIRTUALDISPLAY_EDID);
  info.MonitorContainerId = g_MonitorContainerIds[slot];
  VdIddLog(
    "CreateMonitorLocked: connector=%u type=%u descType=%u descSize=%u containerTail=%02X",
    info.ConnectorIndex,
    static_cast<UINT32>(info.MonitorType),
    static_cast<UINT32>(info.MonitorDescription.Type),
    info.MonitorDescription.DataSize,
    info.MonitorContainerId.Data4[7]
  );

  WDF_OBJECT_ATTRIBUTES monitorAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monitorAttributes, MonitorObjectContext);

  IDARG_IN_MONITORCREATE createArgs = {};
  createArgs.ObjectAttributes = &monitorAttributes;
  createArgs.pMonitorInfo = &info;

  IDARG_OUT_MONITORCREATE createOut = {};
  NTSTATUS status = IddCxMonitorCreate(
    ctx->IddCxAdapter,
    &createArgs,
    &createOut
  );

  if (!NT_SUCCESS(status)) {
    VdIddLog("CreateMonitorLocked: IddCxMonitorCreate failed status=0x%08X", status);
    return status;
  }
  VdIddLog(
    "CreateMonitorLocked: IddCxMonitorCreate status=0x%08X monitor=%p",
    status,
    createOut.MonitorObject
  );

  monCtx->IddCxMonitor = createOut.MonitorObject;
  MonitorObjectContext *objectContext = GetMonitorObjectContext(monCtx->IddCxMonitor);
  objectContext->Slot = static_cast<UINT32>(slot);
  objectContext->Monitor = monCtx;
  monCtx->InUse = TRUE;
  ctx->MonitorCount++;

  IDARG_OUT_MONITORARRIVAL arrivalOut = {};
  status = IddCxMonitorArrival(monCtx->IddCxMonitor, &arrivalOut);
  if (!NT_SUCCESS(status)) {
    VdIddLog("CreateMonitorLocked: IddCxMonitorArrival failed status=0x%08X", status);
    monCtx->InUse = FALSE;
    ctx->MonitorCount--;
    objectContext->Monitor = nullptr;
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(monCtx->IddCxMonitor));
    monCtx->IddCxMonitor = nullptr;
    return status;
  }
  VdIddLog(
    "CreateMonitorLocked: IddCxMonitorArrival status=0x%08X osLuid=%u:%u target=%u",
    status,
    arrivalOut.OsAdapterLuid.HighPart,
    arrivalOut.OsAdapterLuid.LowPart,
    arrivalOut.OsTargetId
  );

  if (outputDesc) {
    *outputDesc = requested;
    outputDesc->MonitorIndex = slot + 1;
  }

  VdIddLog(
    "CreateMonitorLocked: monitor added index=%u monitorCount=%u",
    slot + 1,
    ctx->MonitorCount
  );
  return STATUS_SUCCESS;
}

  #pragma region Driver Entry Points

_Use_decl_annotations_
  NTSTATUS
  DriverDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
  ) {
  UNREFERENCED_PARAMETER(Driver);
  VdIddLog("DriverDeviceAdd: enter");

  WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
  pnpPowerCallbacks.EvtDeviceD0Entry = DriverDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

  IDD_CX_CLIENT_CONFIG config = {};
  IDD_CX_CLIENT_CONFIG_INIT(&config);
  config.EvtIddCxDeviceIoControl = IddDeviceIoControl;
  config.EvtIddCxParseMonitorDescription = IddParseMonitorDescription;
  config.EvtIddCxAdapterInitFinished = IddAdapterInitFinished;
  config.EvtIddCxAdapterCommitModes = IddAdapterCommitModes;
  config.EvtIddCxMonitorAssignSwapChain = IddMonitorAssignSwapChain;
  config.EvtIddCxMonitorUnassignSwapChain = IddMonitorUnassignSwapChain;
  config.EvtIddCxMonitorQueryTargetModes = IddMonitorQueryTargetModes;
  config.EvtIddCxMonitorGetDefaultDescriptionModes = IddMonitorGetDefaultDescModes;
  config.EvtIddCxMonitorGetPhysicalSize = IddMonitorGetPhysicalSize;
  #if IDDCX_VERSION_MINOR >= 0xA
  if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2)) {
    config.EvtIddCxParseMonitorDescription2 = IddParseMonitorDescription2;
  }
  if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo)) {
    config.EvtIddCxAdapterQueryTargetInfo = IddAdapterQueryTargetInfo;
  }
  if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2)) {
    config.EvtIddCxAdapterCommitModes2 = IddAdapterCommitModes2;
  }
  if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorQueryTargetModes2)) {
    config.EvtIddCxMonitorQueryTargetModes2 = IddMonitorQueryTargetModes2;
  }
  if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp)) {
    config.EvtIddCxMonitorSetGammaRamp = IddMonitorSetGammaRamp;
  }
  if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetDefaultHdrMetaData)) {
    config.EvtIddCxMonitorSetDefaultHdrMetaData = IddMonitorSetDefaultHdrMetadata;
  }
  #endif

  NTSTATUS status = IddCxDeviceInitConfig(DeviceInit, &config);
  if (!NT_SUCCESS(status)) {
    VdIddLog("DriverDeviceAdd: IddCxDeviceInitConfig failed status=0x%08X", status);
    return status;
  }
  VdIddLog("DriverDeviceAdd: IddCxDeviceInitConfig status=0x%08X", status);

  WDF_OBJECT_ATTRIBUTES deviceAttr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttr, DeviceContext);

  WDFDEVICE device;
  status = WdfDeviceCreate(&DeviceInit, &deviceAttr, &device);
  if (!NT_SUCCESS(status)) {
    VdIddLog("DriverDeviceAdd: WdfDeviceCreate failed status=0x%08X", status);
    return status;
  }
  VdIddLog("DriverDeviceAdd: WdfDeviceCreate status=0x%08X device=%p", status, device);

  DeviceContext *ctx = GetDeviceContext(device);
  ctx->IddCxAdapter = nullptr;
  ctx->MonitorCount = 0;
  RtlZeroMemory(ctx->Monitors, sizeof(ctx->Monitors));
  RtlZeroMemory(&ctx->Capabilities, sizeof(ctx->Capabilities));
  RtlZeroMemory(&ctx->RenderAdapter, sizeof(ctx->RenderAdapter));
  ctx->RenderAdapter.Size = sizeof(ctx->RenderAdapter);
  ctx->RenderAdapter.Flags = VD_RENDER_AUTO;
  g_DeviceContext = ctx;

  status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->Lock);
  if (!NT_SUCCESS(status)) {
    VdIddLog("DriverDeviceAdd: WdfWaitLockCreate failed status=0x%08X", status);
    return status;
  }
  VdIddLog("DriverDeviceAdd: WdfWaitLockCreate status=0x%08X lock=%p", status, ctx->Lock);

  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status)) {
    VdIddLog("DriverDeviceAdd: IddCxDeviceInitialize failed status=0x%08X", status);
    return status;
  }
  VdIddLog("DriverDeviceAdd: IddCxDeviceInitialize status=0x%08X", status);
  ctx->Capabilities = QueryIddCxRuntimeCapabilities("DriverDeviceAdd");

  status = WdfDeviceCreateDeviceInterface(
    device,
    &GUID_DEVINTERFACE_VIRTUALDISPLAY,
    nullptr
  );
  VdIddLog("DriverDeviceAdd: WdfDeviceCreateDeviceInterface status=0x%08X", status);

  return status;
}

_Use_decl_annotations_
  NTSTATUS
  DriverDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
  ) {
  UNREFERENCED_PARAMETER(PreviousState);

  DeviceContext *ctx = GetDeviceContext(Device);
  VdIddLog(
    "DriverDeviceD0Entry: enter previousState=%u existingAdapter=%p",
    static_cast<UINT32>(PreviousState),
    ctx->IddCxAdapter
  );
  if (ctx->IddCxAdapter) {
    VdIddLog("DriverDeviceD0Entry: adapter already initialized");
    return STATUS_SUCCESS;
  }

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = IDD_STRUCTURE_SIZE(IDDCX_ADAPTER_CAPS);
  caps.Flags = AdapterFlags(&ctx->Capabilities);
  caps.MaxMonitorsSupported = MAX_MONITOR_COUNT;
  caps.MaxDisplayPipelineRate = 0;
  caps.EndPointDiagnostics = g_EndpointDiagnostics;
  caps.StaticDesktopReencodeFrameCount = 0;
  VdIddLog("DriverDeviceD0Entry: caps flags=0x%08X", static_cast<UINT32>(caps.Flags));

  WDF_OBJECT_ATTRIBUTES adapterAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttributes, DeviceContext);

  IDARG_IN_ADAPTER_INIT adapterInit = {};
  adapterInit.WdfDevice = Device;
  adapterInit.pCaps = &caps;
  adapterInit.ObjectAttributes = &adapterAttributes;

  IDARG_OUT_ADAPTER_INIT adapterOut = {};
  NTSTATUS status = IddCxAdapterInitAsync(&adapterInit, &adapterOut);
  VdIddLog(
    "DriverDeviceD0Entry: IddCxAdapterInitAsync status=0x%08X adapter=%p",
    status,
    adapterOut.AdapterObject
  );
  if (!NT_SUCCESS(status)) {
    return status;
  }

  ctx->IddCxAdapter = adapterOut.AdapterObject;
  VdIddLog("DriverDeviceD0Entry: success adapter=%p", ctx->IddCxAdapter);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
  VOID
  DriverUnload(
    _In_ WDFDRIVER Driver
  ) {
  UNREFERENCED_PARAMETER(Driver);
  VdIddLog("DriverUnload: enter");
  if (g_DeviceContext) {
    for (UINT32 i = 0; i < MAX_MONITOR_COUNT; i++) {
      StopSwapChainProcessor(&g_DeviceContext->Monitors[i]);
    }
  }
  g_DeviceContext = nullptr;
}

  #pragma endregion

  #pragma region IddCx Callbacks

_Use_decl_annotations_
  VOID
  IddDeviceIoControl(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
  ) {
  UNREFERENCED_PARAMETER(OutputBufferLength);
  VdIddLog(
    "IddDeviceIoControl: code=0x%08X input=%llu output=%llu device=%p",
    IoControlCode,
    static_cast<unsigned long long>(InputBufferLength),
    static_cast<unsigned long long>(OutputBufferLength),
    Device
  );

  NTSTATUS status = STATUS_SUCCESS;
  DeviceContext *ctx = GetDeviceContext(Device);

  switch (IoControlCode) {
    case IOCTL_VD_ADD_MONITOR:
      {
        MonitorDesc *desc = nullptr;
        status = WdfRequestRetrieveInputBuffer(
          Request,
          sizeof(MonitorDesc),
          reinterpret_cast<PVOID *>(&desc),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }
        if (InputBufferLength < sizeof(MonitorDesc)) {
          status = STATUS_BUFFER_TOO_SMALL;
          break;
        }
        MonitorDesc inputDesc = *desc;

        MonitorDesc *outputDesc = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
          Request,
          sizeof(MonitorDesc),
          reinterpret_cast<PVOID *>(&outputDesc),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }

        WdfWaitLockAcquire(ctx->Lock, nullptr);
        status = CreateMonitorLocked(ctx, &inputDesc, outputDesc);
        WdfWaitLockRelease(ctx->Lock);
        if (NT_SUCCESS(status)) {
          WdfRequestSetInformation(Request, sizeof(MonitorDesc));
        }
        break;
      }

    case IOCTL_VD_UPDATE_MONITOR:
      {
        MonitorDesc *desc = nullptr;
        status = WdfRequestRetrieveInputBuffer(
          Request,
          sizeof(MonitorDesc),
          reinterpret_cast<PVOID *>(&desc),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          VdIddLog("IddDeviceIoControl: UPDATE retrieve input failed status=0x%08X", status);
          break;
        }
        if (InputBufferLength < sizeof(MonitorDesc)) {
          VdIddLog("IddDeviceIoControl: UPDATE input too small len=%llu", static_cast<unsigned long long>(InputBufferLength));
          status = STATUS_BUFFER_TOO_SMALL;
          break;
        }

        MonitorDesc *outputDesc = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
          Request,
          sizeof(MonitorDesc),
          reinterpret_cast<PVOID *>(&outputDesc),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          VdIddLog("IddDeviceIoControl: UPDATE retrieve output failed status=0x%08X", status);
          break;
        }

        WdfWaitLockAcquire(ctx->Lock, nullptr);
        status = UpdateMonitorLocked(ctx, desc, outputDesc);
        WdfWaitLockRelease(ctx->Lock);
        if (NT_SUCCESS(status)) {
          WdfRequestSetInformation(Request, sizeof(MonitorDesc));
        }
        break;
      }

    case IOCTL_VD_REMOVE_MONITOR:
      {
        MonitorDesc *desc = nullptr;
        status = WdfRequestRetrieveInputBuffer(
          Request,
          sizeof(MonitorDesc),
          reinterpret_cast<PVOID *>(&desc),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }
        if (InputBufferLength < sizeof(MonitorDesc)) {
          status = STATUS_BUFFER_TOO_SMALL;
          break;
        }

        WdfWaitLockAcquire(ctx->Lock, nullptr);

        UINT32 idx = desc->MonitorIndex;
        if (idx == 0 || idx > MAX_MONITOR_COUNT || !ctx->Monitors[idx - 1].InUse) {
          WdfWaitLockRelease(ctx->Lock);
          status = STATUS_NOT_FOUND;
          break;
        }

        MonitorContext *monCtx = &ctx->Monitors[idx - 1];
        StopSwapChainProcessor(monCtx);
        IddCxMonitorDeparture(monCtx->IddCxMonitor);
        monCtx->InUse = FALSE;
        monCtx->IddCxMonitor = nullptr;
        ctx->MonitorCount--;

        WdfWaitLockRelease(ctx->Lock);
        break;
      }

    case IOCTL_VD_GET_MONITOR_COUNT:
      {
        UINT32 *outputCount = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
          Request,
          sizeof(UINT32),
          reinterpret_cast<PVOID *>(&outputCount),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }

        WdfWaitLockAcquire(ctx->Lock, nullptr);
        *outputCount = ctx->MonitorCount;
        WdfWaitLockRelease(ctx->Lock);

        WdfRequestSetInformation(Request, sizeof(*outputCount));
        WdfRequestCompleteWithInformation(
          Request,
          STATUS_SUCCESS,
          sizeof(*outputCount)
        );
        return;
      }

    case IOCTL_VD_LIST_MONITORS:
      {
        UINT32 *outputIndexes = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
          Request,
          sizeof(UINT32) * MAX_MONITOR_COUNT,
          reinterpret_cast<PVOID *>(&outputIndexes),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }

        UINT32 listed = 0;
        WdfWaitLockAcquire(ctx->Lock, nullptr);
        for (UINT32 i = 0; i < MAX_MONITOR_COUNT; i++) {
          if (ctx->Monitors[i].InUse) {
            outputIndexes[listed++] = i + 1;
          }
        }
        WdfWaitLockRelease(ctx->Lock);

        WdfRequestSetInformation(Request, listed * sizeof(UINT32));
        WdfRequestCompleteWithInformation(
          Request,
          STATUS_SUCCESS,
          listed * sizeof(UINT32)
        );
        return;
      }

    case IOCTL_VD_GET_CAPABILITIES:
      {
        IddCapabilityDesc *output = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
          Request,
          sizeof(IddCapabilityDesc),
          reinterpret_cast<PVOID *>(&output),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }

        WdfWaitLockAcquire(ctx->Lock, nullptr);
        FillCapabilityDesc(&ctx->Capabilities, output);
        WdfWaitLockRelease(ctx->Lock);

        WdfRequestSetInformation(Request, sizeof(*output));
        WdfRequestCompleteWithInformation(
          Request,
          STATUS_SUCCESS,
          sizeof(*output)
        );
        return;
      }

    case IOCTL_VD_SET_RENDER_ADAPTER:
      {
        RenderAdapterDesc *input = nullptr;
        status = WdfRequestRetrieveInputBuffer(
          Request,
          sizeof(RenderAdapterDesc),
          reinterpret_cast<PVOID *>(&input),
          nullptr
        );
        if (!NT_SUCCESS(status) || InputBufferLength < sizeof(RenderAdapterDesc)) {
          status = STATUS_BUFFER_TOO_SMALL;
          break;
        }

        RenderAdapterDesc request = *input;
        if (request.Size < sizeof(RenderAdapterDesc)) {
          status = STATUS_INVALID_PARAMETER;
          break;
        }

        RenderAdapterDesc *output = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
          Request,
          sizeof(RenderAdapterDesc),
          reinterpret_cast<PVOID *>(&output),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }

        WdfWaitLockAcquire(ctx->Lock, nullptr);
        status = SetPreferredRenderAdapter(ctx, request);
        *output = ctx->RenderAdapter;
        WdfWaitLockRelease(ctx->Lock);
        if (NT_SUCCESS(status)) {
          WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*output));
          return;
        }
        break;
      }

    case IOCTL_VD_GET_RENDER_ADAPTER:
      {
        RenderAdapterDesc *output = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
          Request,
          sizeof(RenderAdapterDesc),
          reinterpret_cast<PVOID *>(&output),
          nullptr
        );
        if (!NT_SUCCESS(status)) {
          break;
        }

        WdfWaitLockAcquire(ctx->Lock, nullptr);
        *output = ctx->RenderAdapter;
        WdfWaitLockRelease(ctx->Lock);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*output));
        return;
      }

    case IOCTL_VD_CLEAR_ALL_MONITORS:
      {
        WdfWaitLockAcquire(ctx->Lock, nullptr);
        for (UINT32 i = 0; i < MAX_MONITOR_COUNT; i++) {
          if (ctx->Monitors[i].InUse) {
            StopSwapChainProcessor(&ctx->Monitors[i]);
            IddCxMonitorDeparture(ctx->Monitors[i].IddCxMonitor);
            ctx->Monitors[i].InUse = FALSE;
            ctx->Monitors[i].IddCxMonitor = nullptr;
          }
        }
        ctx->MonitorCount = 0;
        WdfWaitLockRelease(ctx->Lock);
        break;
      }

    default:
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }

  WdfRequestComplete(Request, status);
}

_Use_decl_annotations_
  NTSTATUS
  IddParseMonitorDescription(
    _In_ const IDARG_IN_PARSEMONITORDESCRIPTION *pInArgs,
    _Out_ IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs
  ) {
  VdIddLog(
    "IddParseMonitorDescription: type=%u dataSize=%u inputCount=%u",
    static_cast<UINT32>(pInArgs->MonitorDescription.Type),
    pInArgs->MonitorDescription.DataSize,
    pInArgs->MonitorModeBufferInputCount
  );

  MonitorModeSpec modeSpecs[MAX_ADVERTISED_MODE_COUNT] = {};
  UINT32 preferredIndex = NO_PREFERRED_MODE;
  const UINT32 modeCount = BuildAdvertisedModeSpecs(nullptr, modeSpecs, ARRAYSIZE(modeSpecs), &preferredIndex);

  if (pInArgs->MonitorModeBufferInputCount >= modeCount && pInArgs->pMonitorModes) {
    for (UINT32 i = 0; i < modeCount; i++) {
      FillMonitorMode(
        &pInArgs->pMonitorModes[i],
        modeSpecs[i].Width,
        modeSpecs[i].Height,
        modeSpecs[i].VSync,
        IDDCX_MONITOR_MODE_ORIGIN_DRIVER
      );
    }
    pOutArgs->MonitorModeBufferOutputCount = modeCount;
    pOutArgs->PreferredMonitorModeIdx = preferredIndex;
  } else {
    pOutArgs->MonitorModeBufferOutputCount = modeCount;
    pOutArgs->PreferredMonitorModeIdx = NO_PREFERRED_MODE;
  }

  return STATUS_SUCCESS;
}

  #if IDDCX_VERSION_MINOR >= 0xA
_Use_decl_annotations_
  NTSTATUS
  IddParseMonitorDescription2(
    _In_ const IDARG_IN_PARSEMONITORDESCRIPTION2 *pInArgs,
    _Out_ IDARG_OUT_PARSEMONITORDESCRIPTION *pOutArgs
  ) {
  VdIddLog(
    "IddParseMonitorDescription2: type=%u dataSize=%u inputCount=%u",
    static_cast<UINT32>(pInArgs->MonitorDescription.Type),
    pInArgs->MonitorDescription.DataSize,
    pInArgs->MonitorModeBufferInputCount
  );

  MonitorModeSpec modeSpecs[MAX_ADVERTISED_MODE_COUNT] = {};
  UINT32 preferredIndex = NO_PREFERRED_MODE;
  const UINT32 modeCount = BuildAdvertisedModeSpecs(nullptr, modeSpecs, ARRAYSIZE(modeSpecs), &preferredIndex);

  if (pInArgs->MonitorModeBufferInputCount >= modeCount && pInArgs->pMonitorModes) {
    const bool hdr10 = g_DeviceContext && g_DeviceContext->Capabilities.Hdr10;
    for (UINT32 i = 0; i < modeCount; i++) {
      FillMonitorMode2(
        &pInArgs->pMonitorModes[i],
        modeSpecs[i].Width,
        modeSpecs[i].Height,
        modeSpecs[i].VSync,
        IDDCX_MONITOR_MODE_ORIGIN_DRIVER,
        hdr10
      );
    }
    pOutArgs->MonitorModeBufferOutputCount = modeCount;
    pOutArgs->PreferredMonitorModeIdx = preferredIndex;
  } else {
    pOutArgs->MonitorModeBufferOutputCount = modeCount;
    pOutArgs->PreferredMonitorModeIdx = NO_PREFERRED_MODE;
  }

  return STATUS_SUCCESS;
}
  #endif

_Use_decl_annotations_
  NTSTATUS
  IddAdapterInitFinished(
    _In_ IDDCX_ADAPTER AdapterObject,
    _In_ const IDARG_IN_ADAPTER_INIT_FINISHED *pInArgs
  ) {
  UNREFERENCED_PARAMETER(AdapterObject);
  VdIddLog(
    "IddAdapterInitFinished: adapter=%p status=0x%08X",
    AdapterObject,
    pInArgs->AdapterInitStatus
  );
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
  NTSTATUS
  IddAdapterCommitModes(
    _In_ IDDCX_ADAPTER AdapterObject,
    _In_ const IDARG_IN_COMMITMODES *pInArgs
  ) {
  UNREFERENCED_PARAMETER(AdapterObject);
  VdIddLog(
    "IddAdapterCommitModes: adapter=%p pathCount=%u",
    AdapterObject,
    pInArgs->PathCount
  );

  return STATUS_SUCCESS;
}

  #if IDDCX_VERSION_MINOR >= 0xA
_Use_decl_annotations_
  NTSTATUS
  IddAdapterQueryTargetInfo(
    _In_ IDDCX_ADAPTER AdapterObject,
    _In_ IDARG_IN_QUERYTARGET_INFO *pInArgs,
    _Out_ IDARG_OUT_QUERYTARGET_INFO *pOutArgs
  ) {
  UNREFERENCED_PARAMETER(AdapterObject);
  if (pInArgs->ConnectorIndex >= MAX_MONITOR_COUNT) {
    VdIddLog(
      "IddAdapterQueryTargetInfo: invalid connector=%u",
      pInArgs->ConnectorIndex
    );
    return STATUS_INVALID_PARAMETER;
  }

  const bool hdr10 = g_DeviceContext && g_DeviceContext->Capabilities.Hdr10;
  pOutArgs->TargetCaps = hdr10 ?
                           static_cast<IDDCX_TARGET_CAPS>(IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE | IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE) :
                           IDDCX_TARGET_CAPS_NONE;
  // HDR policy requires the target to either carry 10 bits per component or
  // dither 10-bit content down to 8 bits. The virtual display consumes the
  // swapchain directly, so advertise 8-bit dithering to keep HDR enabled at
  // every mode and bit depth.
  pOutArgs->DitheringSupport.Rgb = IDDCX_BITS_PER_COMPONENT_8;
  pOutArgs->DitheringSupport.YCbCr444 = IDDCX_BITS_PER_COMPONENT_8;
  pOutArgs->DitheringSupport.YCbCr422 = IDDCX_BITS_PER_COMPONENT_8;
  pOutArgs->DitheringSupport.YCbCr420 = IDDCX_BITS_PER_COMPONENT_8;

  VdIddLog(
    "IddAdapterQueryTargetInfo: connector=%u targetCaps=0x%08X",
    pInArgs->ConnectorIndex,
    static_cast<UINT32>(pOutArgs->TargetCaps)
  );
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
  NTSTATUS
  IddAdapterCommitModes2(
    _In_ IDDCX_ADAPTER AdapterObject,
    _In_ const IDARG_IN_COMMITMODES2 *pInArgs
  ) {
  UNREFERENCED_PARAMETER(AdapterObject);
  VdIddLog(
    "IddAdapterCommitModes2: adapter=%p pathCount=%u",
    AdapterObject,
    pInArgs->PathCount
  );

  if (pInArgs->PathCount > 0 && !pInArgs->pPaths) {
    return STATUS_INVALID_PARAMETER;
  }

  for (UINT32 i = 0; i < pInArgs->PathCount; i++) {
    const IDDCX_PATH2 *path = &pInArgs->pPaths[i];
    VdIddLog(
      "IddAdapterCommitModes2: path=%u monitor=%p flags=0x%08X %ldx%ld v=%u/%u colorSpace=%u rgbBits=0x%08X",
      i,
      path->MonitorObject,
      static_cast<UINT32>(path->Flags),
      path->TargetVideoSignalInfo.activeSize.cx,
      path->TargetVideoSignalInfo.activeSize.cy,
      path->TargetVideoSignalInfo.vSyncFreq.Numerator,
      path->TargetVideoSignalInfo.vSyncFreq.Denominator,
      static_cast<UINT32>(path->WireFormatInfo.ColorSpace),
      static_cast<UINT32>(path->WireFormatInfo.BitsPerComponent.Rgb)
    );
  }

  return STATUS_SUCCESS;
}

_Use_decl_annotations_
  NTSTATUS
  IddMonitorSetGammaRamp(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_SET_GAMMARAMP *pInArgs
  ) {
  if (!pInArgs) {
    return STATUS_INVALID_PARAMETER;
  }

  switch (pInArgs->Type) {
    case IDDCX_GAMMARAMP_TYPE_DEFAULT:
      VdIddLog("IddMonitorSetGammaRamp: monitor=%p type=default", MonitorObject);
      break;

    case IDDCX_GAMMARAMP_TYPE_RGB256x3x16:
      VdIddLog(
        "IddMonitorSetGammaRamp: monitor=%p type=rgb256x3x16 size=%u",
        MonitorObject,
        pInArgs->GammaRampSizeInBytes
      );
      break;

    case IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM:
      {
        const auto *transform = static_cast<const IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM *>(pInArgs->pGammaRampData);
        if (!transform || pInArgs->GammaRampSizeInBytes < sizeof(*transform)) {
          return STATUS_INVALID_PARAMETER;
        }
        VdIddLog(
          "IddMonitorSetGammaRamp: monitor=%p type=3x4 matrixEnabled=%d lutEnabled=%d scalar=%f m11=%f m22=%f m33=%f m44=%f",
          MonitorObject,
          static_cast<int>(transform->MatrixEnabled),
          static_cast<int>(transform->LutEnabled),
          transform->ScalarMultiplier,
          transform->ColorMatrix3x4[0][0],
          transform->ColorMatrix3x4[1][1],
          transform->ColorMatrix3x4[2][2],
          transform->ColorMatrix3x4[2][3]
        );
        break;
      }

    default:
      VdIddLog("IddMonitorSetGammaRamp: monitor=%p unknown type=%u", MonitorObject, static_cast<UINT32>(pInArgs->Type));
      return STATUS_INVALID_PARAMETER;
  }

  return STATUS_SUCCESS;
}

_Use_decl_annotations_
  NTSTATUS
  IddMonitorSetDefaultHdrMetadata(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *pInArgs
  ) {
  if (!pInArgs) {
    return STATUS_INVALID_PARAMETER;
  }

  if (pInArgs->Type != IDDCX_HDRMETADATA_TYPE_HDR10 || !pInArgs->Data.pHdr10 || pInArgs->Size < sizeof(IDDCX_HDR10_METADATA)) {
    VdIddLog(
      "IddMonitorSetDefaultHdrMetadata: monitor=%p invalid type=%u size=%u",
      MonitorObject,
      static_cast<UINT32>(pInArgs->Type),
      pInArgs->Size
    );
    return STATUS_INVALID_PARAMETER;
  }

  const IDDCX_HDR10_METADATA *metadata = pInArgs->Data.pHdr10;
  VdIddLog(
    "IddMonitorSetDefaultHdrMetadata: monitor=%p maxLuminance=%u minLuminance=%u maxCLL=%u maxFALL=%u",
    MonitorObject,
    metadata->MaxMasteringLuminance,
    metadata->MinMasteringLuminance,
    metadata->MaxContentLightLevel,
    metadata->MaxFrameAverageLightLevel
  );
  return STATUS_SUCCESS;
}
  #endif

_Use_decl_annotations_
  NTSTATUS
  IddMonitorGetPhysicalSize(
    _In_ IDDCX_MONITOR MonitorObject,
    _Out_ IDARG_OUT_MONITORGETPHYSICALSIZE *pOutArgs
  ) {
  if (!pOutArgs) {
    return STATUS_INVALID_PARAMETER;
  }

  // 21.5 inch 16:9 virtual monitor: 476 x 268 mm.
  pOutArgs->PhysicalWidth = 476;
  pOutArgs->PhysicalHeight = 268;
  VdIddLog(
    "IddMonitorGetPhysicalSize: monitor=%p size=%ux%u mm",
    MonitorObject,
    pOutArgs->PhysicalWidth,
    pOutArgs->PhysicalHeight
  );
  return STATUS_SUCCESS;
}

_Use_decl_annotations_
  NTSTATUS
  IddMonitorAssignSwapChain(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_SETSWAPCHAIN *pInArgs
  ) {
  MonitorContext *monCtx = FindMonitorById(MonitorObject);
  if (!monCtx) {
    return STATUS_NOT_FOUND;
  }

  StopSwapChainProcessor(monCtx);

  monCtx->SwapChain = pInArgs->hSwapChain;
  monCtx->hNextSurfaceEvent = pInArgs->hNextSurfaceAvailable;
  monCtx->UseHdrSwapChain = g_DeviceContext && g_DeviceContext->Capabilities.Hdr10;
  if (g_DeviceContext) {
    g_DeviceContext->RenderAdapter.ActualLuid = pInArgs->RenderAdapterLuid;
    g_DeviceContext->RenderAdapter.Flags |= VD_RENDER_ACTUAL_VALID;
  }
  VdIddLog(
    "IddMonitorAssignSwapChain: monitor=%p renderLuid=%ld:%lu buffer2=%u",
    MonitorObject,
    pInArgs->RenderAdapterLuid.HighPart,
    pInArgs->RenderAdapterLuid.LowPart,
    monCtx->UseHdrSwapChain
  );

  HRESULT hr = CreateProcessingDevice(
    pInArgs->RenderAdapterLuid,
    &monCtx->ProcessingDevice
  );
  if (FAILED(hr)) {
    StopSwapChainProcessor(monCtx);
    return STATUS_SUCCESS;
  }

  IDXGIDevice *dxgiDevice = nullptr;
  hr = monCtx->ProcessingDevice->QueryInterface(
    __uuidof(IDXGIDevice),
    reinterpret_cast<void **>(&dxgiDevice)
  );

  if (FAILED(hr) || !dxgiDevice) {
    StopSwapChainProcessor(monCtx);
    return STATUS_SUCCESS;
  }

  IDARG_IN_SWAPCHAINSETDEVICE setDev = {};
  setDev.pDevice = dxgiDevice;
  hr = IddCxSwapChainSetDevice(pInArgs->hSwapChain, &setDev);
  dxgiDevice->Release();
  if (FAILED(hr)) {
    StopSwapChainProcessor(monCtx);
    return STATUS_SUCCESS;
  }

  monCtx->SwapChainTerminateEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!monCtx->SwapChainTerminateEvent) {
    StopSwapChainProcessor(monCtx);
    return STATUS_SUCCESS;
  }

  monCtx->SwapChainThread = CreateThread(
    nullptr,
    0,
    SwapChainThreadProc,
    monCtx,
    0,
    nullptr
  );
  if (!monCtx->SwapChainThread) {
    StopSwapChainProcessor(monCtx);
  }

  return STATUS_SUCCESS;
}

_Use_decl_annotations_
  NTSTATUS
  IddMonitorUnassignSwapChain(
    _In_ IDDCX_MONITOR MonitorObject
  ) {
  MonitorContext *monCtx = FindMonitorById(MonitorObject);
  if (!monCtx) {
    return STATUS_NOT_FOUND;
  }

  return StopSwapChainProcessor(monCtx);
}

_Use_decl_annotations_
  NTSTATUS
  IddMonitorQueryTargetModes(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_QUERYTARGETMODES *pInArgs,
    _Out_ IDARG_OUT_QUERYTARGETMODES *pOutArgs
  ) {
  VdIddLog(
    "IddMonitorQueryTargetModes: monitor=%p inputCount=%u",
    MonitorObject,
    pInArgs->TargetModeBufferInputCount
  );
  MonitorContext *monCtx = FindMonitorById(MonitorObject);
  if (!monCtx) {
    VdIddLog("IddMonitorQueryTargetModes: monitor not found");
    return STATUS_NOT_FOUND;
  }

  MonitorModeSpec modeSpecs[MAX_ADVERTISED_MODE_COUNT] = {};
  const UINT32 modeCount = BuildAdvertisedModeSpecs(monCtx, modeSpecs, ARRAYSIZE(modeSpecs), nullptr);

  if (pInArgs->TargetModeBufferInputCount >= modeCount && pInArgs->pTargetModes) {
    for (UINT32 i = 0; i < modeCount; i++) {
      FillTargetMode(&pInArgs->pTargetModes[i], modeSpecs[i].Width, modeSpecs[i].Height, modeSpecs[i].VSync);
    }
    pOutArgs->TargetModeBufferOutputCount = modeCount;
  } else {
    pOutArgs->TargetModeBufferOutputCount = modeCount;
  }

  return STATUS_SUCCESS;
}

  #if IDDCX_VERSION_MINOR >= 0xA
_Use_decl_annotations_
  NTSTATUS
  IddMonitorQueryTargetModes2(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_QUERYTARGETMODES2 *pInArgs,
    _Out_ IDARG_OUT_QUERYTARGETMODES *pOutArgs
  ) {
  VdIddLog(
    "IddMonitorQueryTargetModes2: monitor=%p inputCount=%u descType=%u descSize=%u",
    MonitorObject,
    pInArgs->TargetModeBufferInputCount,
    static_cast<UINT32>(pInArgs->MonitorDescription.Type),
    pInArgs->MonitorDescription.DataSize
  );

  MonitorContext *monCtx = FindMonitorById(MonitorObject);
  if (!monCtx) {
    VdIddLog("IddMonitorQueryTargetModes2: monitor not found");
    return STATUS_NOT_FOUND;
  }

  MonitorModeSpec modeSpecs[MAX_ADVERTISED_MODE_COUNT] = {};
  const UINT32 modeCount = BuildAdvertisedModeSpecs(monCtx, modeSpecs, ARRAYSIZE(modeSpecs), nullptr);

  if (pInArgs->TargetModeBufferInputCount >= modeCount && pInArgs->pTargetModes) {
    const bool hdr10 = g_DeviceContext && g_DeviceContext->Capabilities.Hdr10;
    for (UINT32 i = 0; i < modeCount; i++) {
      FillTargetMode2(&pInArgs->pTargetModes[i], modeSpecs[i].Width, modeSpecs[i].Height, modeSpecs[i].VSync, hdr10);
    }
    pOutArgs->TargetModeBufferOutputCount = modeCount;
  } else {
    pOutArgs->TargetModeBufferOutputCount = modeCount;
  }

  return STATUS_SUCCESS;
}
  #endif

_Use_decl_annotations_
  NTSTATUS
  IddMonitorGetDefaultDescModes(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *pInArgs,
    _Out_ IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *pOutArgs
  ) {
  VdIddLog(
    "IddMonitorGetDefaultDescModes: monitor=%p inputCount=%u",
    MonitorObject,
    pInArgs->DefaultMonitorModeBufferInputCount
  );
  MonitorContext *monCtx = FindMonitorById(MonitorObject);
  if (!monCtx) {
    VdIddLog("IddMonitorGetDefaultDescModes: monitor not found");
    return STATUS_NOT_FOUND;
  }

  MonitorModeSpec modeSpecs[MAX_ADVERTISED_MODE_COUNT] = {};
  UINT32 preferredIndex = NO_PREFERRED_MODE;
  const UINT32 modeCount = BuildAdvertisedModeSpecs(monCtx, modeSpecs, ARRAYSIZE(modeSpecs), &preferredIndex);

  if (pInArgs->DefaultMonitorModeBufferInputCount >= modeCount && pInArgs->pDefaultMonitorModes) {
    for (UINT32 i = 0; i < modeCount; i++) {
      FillMonitorMode(
        &pInArgs->pDefaultMonitorModes[i],
        modeSpecs[i].Width,
        modeSpecs[i].Height,
        modeSpecs[i].VSync,
        IDDCX_MONITOR_MODE_ORIGIN_DRIVER
      );
    }
    pOutArgs->DefaultMonitorModeBufferOutputCount = modeCount;
    pOutArgs->PreferredMonitorModeIdx = preferredIndex;
  } else {
    pOutArgs->DefaultMonitorModeBufferOutputCount = modeCount;
    pOutArgs->PreferredMonitorModeIdx = NO_PREFERRED_MODE;
  }

  return STATUS_SUCCESS;
}

  #pragma endregion
