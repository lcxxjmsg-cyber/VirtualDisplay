#include <windows.h>

BOOL
WINAPI
DllMain(
  _In_ HINSTANCE hinstDll,
  _In_ DWORD dwReason,
  _In_ LPVOID lpvReserved
)
{
  UNREFERENCED_PARAMETER(hinstDll);
  UNREFERENCED_PARAMETER(lpvReserved);

  switch (dwReason) {
  case DLL_PROCESS_ATTACH:
  case DLL_THREAD_ATTACH:
  case DLL_THREAD_DETACH:
  case DLL_PROCESS_DETACH:
    break;
  }

  return TRUE;
}
