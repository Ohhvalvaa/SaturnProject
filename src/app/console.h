// PS5 on the network: state, wake-up, discovery, pairing (Pair Device) and PSN Account ID.
#pragma once
#include <stdint.h>
extern "C" {
#include "../common/console_store.h"
}

enum ConsoleState { CS_UNKNOWN = -2, CS_OFFLINE = -1, CS_STANDBY = 0, CS_READY = 1 };

struct chiaki_log_t;
void console_set_log(struct chiaki_log_t *log);

struct FoundConsole { char host[64]; char name[64]; bool ps5; int state; };

int console_state(const char *host, char *name_out = nullptr, int name_len = 0); // ConsoleState
void console_wakeup(const SaturnConsole &c);
// looks for consoles on the network (UDP broadcast 9302 and 987) for ~1.5 s. Returns how many were found.
int console_discover(FoundConsole *out, int max);

// pairing: blocks up to 30 s. err receives a readable message on failure.
bool console_register(const char *host, const char *account_b64, const char *pin, SaturnConsole *out, wchar_t *err, int err_len);

// PSN: address to open in the browser to sign in; after signing in Sony redirects to a page whose
// address contains ?code=... : psn_account_from_redirect turns it into the Account ID (Base64).
const wchar_t *psn_login_url();
bool psn_account_from_redirect(const char *redirect_url, char *b64_out, int b64_len, wchar_t *err, int err_len);
