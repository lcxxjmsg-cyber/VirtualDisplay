#include "Device.h"
#include "DebugLog.h"
#include "Trace.h"

extern "C" {
DRIVER_INITIALIZE DriverEntry;
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
  _In_ PDRIVER_OBJECT DriverObject,
  _In_ PUNICODE_STRING RegistryPath)
{
  VdIddLog("DriverEntry: enter");

  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, DriverDeviceAdd);
  config.DriverPoolTag = 'VDID';
  config.EvtDriverUnload = DriverUnload;

  const NTSTATUS status = WdfDriverCreate(
    DriverObject,
    RegistryPath,
    WDF_NO_OBJECT_ATTRIBUTES,
    &config,
    WDF_NO_HANDLE);
  VdIddLog("DriverEntry: WdfDriverCreate status=0x%08X", status);

  return status;
}
