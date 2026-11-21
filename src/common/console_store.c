#include "console_store.h"
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#define SATURN_MAGIC 0x4E525453u /* "STRN" */
#define SATURN_VERSION 1u

static char g_path[MAX_PATH];

const char *saturn_console_path(void)
{
	if (!g_path[0])
	{
		char base[MAX_PATH];
		if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
		{
			snprintf(g_path, sizeof(g_path), "%s\\SaturnProject", base);
			CreateDirectoryA(g_path, NULL);
			strncat(g_path, "\\console.dat", sizeof(g_path) - strlen(g_path) - 1);
		}
	}
	return g_path;
}

bool saturn_console_save(const SaturnConsole *c)
{
	struct { uint32_t magic, version; SaturnConsole c; } plain = { SATURN_MAGIC, SATURN_VERSION, *c };
	DATA_BLOB in = { sizeof(plain), (BYTE *)&plain }, out = { 0 };
	if (!CryptProtectData(&in, L"SaturnProject console", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return false;
	SecureZeroMemory(&plain, sizeof(plain));
	FILE *f = fopen(saturn_console_path(), "wb");
	bool ok = f && fwrite(out.pbData, 1, out.cbData, f) == out.cbData;
	if (f) fclose(f);
	LocalFree(out.pbData);
	return ok;
}

bool saturn_console_load(SaturnConsole *c)
{
	FILE *f = fopen(saturn_console_path(), "rb");
	if (!f) return false;
	static BYTE buf[16384];
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	DATA_BLOB in = { (DWORD)n, buf }, out = { 0 };
	if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return false;
	bool ok = false;
	struct hdr { uint32_t magic, version; SaturnConsole c; } *p = (struct hdr *)out.pbData;
	if (out.cbData == sizeof(*p) && p->magic == SATURN_MAGIC && p->version == SATURN_VERSION)
	{
		*c = p->c;
		ok = true;
	}
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return ok;
}
