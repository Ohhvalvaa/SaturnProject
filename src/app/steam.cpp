// Steam integration: when started from Steam, the app relaunches itself outside Steam's process tree.
#include "steam.h"
#include <wbemidl.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <stdio.h>
#include <string>

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

// ---------- started from Steam ----------
static DWORD parent_pid(DWORD pid, wchar_t *name, int n)
{
	DWORD ppid = 0;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W e = { sizeof(e) };
	for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
		if (e.th32ProcessID == pid) { ppid = e.th32ParentProcessID; break; }
	if (name) { name[0] = 0; e.dwSize = sizeof(e);
		for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
			if (e.th32ProcessID == ppid) { wcsncpy(name, e.szExeFile, n - 1); name[n - 1] = 0; break; } }
	CloseHandle(snap);
	return ppid;
}

static bool launched_by_steam()
{
	wchar_t v[64];
	if (GetEnvironmentVariableW(L"SteamGameId", v, 64) || GetEnvironmentVariableW(L"SteamAppId", v, 64)) return true;
	wchar_t name[MAX_PATH];
	parent_pid(GetCurrentProcessId(), name, MAX_PATH);
	return !_wcsicmp(name, L"steam.exe");
}

// Win32_Process.Create: the process is born as a child of the WMI service, not of Steam
static DWORD wmi_create(const wchar_t *cmdline, const wchar_t *dir)
{
	DWORD pid = 0;
	CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
	IWbemLocator *loc = nullptr; IWbemServices *svc = nullptr; IWbemClassObject *cls = nullptr, *def = nullptr, *in = nullptr, *out = nullptr;
	BSTR ns = SysAllocString(L"ROOT\\CIMV2"), clsname = SysAllocString(L"Win32_Process"), meth = SysAllocString(L"Create");
	if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void **)&loc))
		&& SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)))
	{
		CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
		if (SUCCEEDED(svc->GetObject(clsname, 0, nullptr, &cls, nullptr)) && SUCCEEDED(cls->GetMethod(meth, 0, &def, nullptr))
			&& SUCCEEDED(def->SpawnInstance(0, &in)))
		{
			VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocString(cmdline);
			in->Put(L"CommandLine", 0, &v, 0); VariantClear(&v);
			if (dir) { v.vt = VT_BSTR; v.bstrVal = SysAllocString(dir); in->Put(L"CurrentDirectory", 0, &v, 0); VariantClear(&v); }
			if (SUCCEEDED(svc->ExecMethod(clsname, meth, 0, nullptr, in, &out, nullptr)) && out)
			{
				VARIANT r; VariantInit(&r);
				if (SUCCEEDED(out->Get(L"ReturnValue", 0, &r, nullptr, nullptr)) && r.vt == VT_I4 && r.lVal == 0)
				{
					VARIANT p; VariantInit(&p);
					if (SUCCEEDED(out->Get(L"ProcessId", 0, &p, nullptr, nullptr))) pid = p.vt == VT_I4 ? (DWORD)p.lVal : p.uintVal;
					VariantClear(&p);
				}
				VariantClear(&r);
			}
		}
	}
	release(out); release(in); release(def); release(cls); release(svc); release(loc);
	SysFreeString(ns); SysFreeString(clsname); SysFreeString(meth);
	return pid;
}

bool steam_relaunch_detached(const wchar_t *cmdline, int *exit_code)
{
	if (cmdline && (wcsstr(cmdline, L"--from-steam") || wcsstr(cmdline, L"--no-detach"))) return false;
	if (!launched_by_steam()) return false;
	wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
	wchar_t dir[MAX_PATH]; wcscpy(dir, exe); wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) *sl = 0;
	std::wstring cl = L"\"" + std::wstring(exe) + L"\" --from-steam " + std::to_wstring(GetCurrentProcessId()) + L" " + (cmdline ? cmdline : L"");
	DWORD pid = wmi_create(cl.c_str(), dir);
	if (!pid) return false; // WMI not available: keep running here (with Steam Input)
	AllowSetForegroundWindow(pid);
	HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	DWORD code = 0;
	if (h) { WaitForSingleObject(h, INFINITE); GetExitCodeProcess(h, &code); CloseHandle(h); }
	*exit_code = (int)code;
	return true;
}

DWORD steam_launcher_pid(const wchar_t *cmdline)
{
	const wchar_t *p = cmdline ? wcsstr(cmdline, L"--from-steam ") : nullptr;
	return p ? (DWORD)wcstoul(p + 13, nullptr, 10) : 0;
}
