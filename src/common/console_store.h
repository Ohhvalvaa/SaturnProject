// Paired console saved in %LOCALAPPDATA%\SaturnProject\console.dat, encrypted with DPAPI
// (readable only by the Windows user who created it).
#pragma once
#include <chiaki/regist.h>
#include <stdbool.h>

typedef struct SaturnConsole
{
	char host[64];                       // PS5 IP address
	uint8_t psn_account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	ChiakiRegisteredHost registered;     // keys received when pairing
} SaturnConsole;

bool saturn_console_save(const SaturnConsole *c);
bool saturn_console_load(SaturnConsole *c);
const char *saturn_console_path(void);
