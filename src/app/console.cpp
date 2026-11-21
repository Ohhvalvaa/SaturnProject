#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include "console.h"
#include <chiaki/discovery.h>
#include <chiaki/regist.h>
#include <chiaki/base64.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static ChiakiLog *g_clog;
void console_set_log(ChiakiLog *log) { g_clog = log; }
static ChiakiLog *log_ptr() { return g_clog; }

static const char *SRCH = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";

static void header_value(const char *buf, const char *key, char *out, int len)
{
	out[0] = 0;
	const char *p = strstr(buf, key);
	if (!p) return;
	p += strlen(key);
	int i = 0;
	while (*p && *p != '\r' && *p != '\n' && i < len - 1) out[i++] = *p++;
	out[i] = 0;
}

static int parse_state(const char *buf)
{
	if (strstr(buf, "200 Ok")) return CS_READY;
	if (strstr(buf, "620")) return CS_STANDBY;
	return CS_UNKNOWN;
}

int console_state(const char *host, char *name_out, int name_len)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	DWORD to = 1000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
	sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(9302);
	if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { closesocket(s); return CS_OFFLINE; }
	sendto(s, SRCH, (int)strlen(SRCH), 0, (sockaddr *)&a, sizeof(a));
	char buf[1024]; int n = recv(s, buf, sizeof(buf) - 1, 0);
	closesocket(s);
	if (n <= 0) return CS_OFFLINE;
	buf[n] = 0;
	if (name_out) header_value(buf, "host-name:", name_out, name_len);
	return parse_state(buf);
}

void console_wakeup(const SaturnConsole &c)
{
	char hex[CHIAKI_SESSION_AUTH_SIZE + 1] = {};
	memcpy(hex, c.registered.rp_regist_key, CHIAKI_SESSION_AUTH_SIZE);
	uint64_t cred = strtoull(hex, nullptr, 16);
	chiaki_discovery_wakeup(log_ptr(), nullptr, c.host, cred, true);
}

int console_discover(FoundConsole *out, int max)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	BOOL yes = TRUE; setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
	DWORD to = 300; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
	sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_BROADCAST;
	int n = 0;
	DWORD end = GetTickCount() + 1500;
	for (int round = 0; GetTickCount() < end; round++)
	{
		if (round % 3 == 0)
		{
			a.sin_port = htons(9302); sendto(s, SRCH, (int)strlen(SRCH), 0, (sockaddr *)&a, sizeof(a));
		}
		char buf[1024]; sockaddr_in from = {}; int fl = sizeof(from);
		int r = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
		if (r <= 0) continue;
		buf[r] = 0;
		char ip[64]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
		bool dup = false;
		for (int i = 0; i < n; i++) if (!strcmp(out[i].host, ip)) dup = true;
		if (dup || n >= max) continue;
		FoundConsole &c = out[n++];
		memset(&c, 0, sizeof(c));
		strncpy(c.host, ip, sizeof(c.host) - 1);
		header_value(buf, "host-name:", c.name, sizeof(c.name));
		char type[32]; header_value(buf, "host-type:", type, sizeof(type));
		c.ps5 = strstr(type, "PS5") != nullptr;
		c.state = parse_state(buf);
	}
	closesocket(s);
	return n;
}

// ---------- pairing ----------
struct RegCtx { HANDLE done; bool ok; SaturnConsole *c; };

static void regist_cb(ChiakiRegistEvent *ev, void *user)
{
	auto *r = (RegCtx *)user;
	if (ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && ev->registered_host)
	{
		r->c->registered = *ev->registered_host;
		r->ok = true;
	}
	SetEvent(r->done);
}

bool console_register(const char *host, const char *account_b64, const char *pin, SaturnConsole *out, wchar_t *err, int err_len)
{
	SaturnConsole c = {};
	strncpy(c.host, host, sizeof(c.host) - 1);
	size_t sz = sizeof(c.psn_account_id);
	if (chiaki_base64_decode(account_b64, strlen(account_b64), c.psn_account_id, &sz) != CHIAKI_ERR_SUCCESS || sz != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		swprintf(err, err_len, L"Invalid Account ID");
		return false;
	}
	if (strlen(pin) != 8) { swprintf(err, err_len, L"The code has 8 digits"); return false; }
	ChiakiRegistInfo info = {};
	info.target = CHIAKI_TARGET_PS5_1;
	info.host = c.host;
	info.broadcast = false;
	memcpy(info.psn_account_id, c.psn_account_id, sizeof(info.psn_account_id));
	info.pin = (uint32_t)strtoul(pin, nullptr, 10);
	RegCtx r = { CreateEventA(nullptr, TRUE, FALSE, nullptr), false, &c };
	ChiakiRegist regist;
	if (chiaki_regist_start(&regist, log_ptr(), &info, regist_cb, &r) != CHIAKI_ERR_SUCCESS)
	{
		CloseHandle(r.done);
		swprintf(err, err_len, L"Pairing not started");
		return false;
	}
	WaitForSingleObject(r.done, 30000);
	chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);
	CloseHandle(r.done);
	if (!r.ok) { swprintf(err, err_len, L"Rejected by the PS5: expired code or wrong Account ID"); return false; }
	if (!saturn_console_save(&c)) { swprintf(err, err_len, L"Paired but not saved"); return false; }
	*out = c;
	return true;
}

// ---------- PSN: Account ID from the sign-in (same method as chiaki-ng) ----------
#define PSN_CLIENT_ID "ba495a24-818c-472b-b12d-ff231c1b5745"
#define PSN_CLIENT_SECRET "mvaiZkRsAsI1IBkY"
#define PSN_SCOPE "psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect sessionManager:remotePlaySession.system.update"
#define PSN_REDIRECT "https://remoteplay.dl.playstation.net/remoteplay/redirect"

const wchar_t *psn_login_url()
{
	return L"https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?service_entity=urn:service-entity:psn"
		L"&response_type=code&client_id=" L"ba495a24-818c-472b-b12d-ff231c1b5745"
		L"&redirect_uri=https://remoteplay.dl.playstation.net/remoteplay/redirect"
		L"&scope=psn:clientapp%20referenceDataService:countryConfig.read%20pushNotification:webSocket.desktop.connect%20sessionManager:remotePlaySession.system.update"
		L"&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal";
}

// HTTPS request to auth.api.sonyentertainmentnetwork.com; returns the response body (malloc) or null
static char *https(const wchar_t *verb, const wchar_t *path, const char *body)
{
	char *result = nullptr;
	HINTERNET ses = WinHttpOpen(L"SaturnProject", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET con = ses ? WinHttpConnect(ses, L"auth.api.sonyentertainmentnetwork.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
	HINTERNET req = con ? WinHttpOpenRequest(con, verb, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	if (req)
	{
		const char *cred = PSN_CLIENT_ID ":" PSN_CLIENT_SECRET;
		char b64[128] = {};
		chiaki_base64_encode((const uint8_t *)cred, strlen(cred), b64, sizeof(b64));
		wchar_t hdr[512];
		swprintf(hdr, 512, L"Authorization: Basic %hs\r\nContent-Type: application/x-www-form-urlencoded\r\n", b64);
		DWORD blen = body ? (DWORD)strlen(body) : 0;
		if (WinHttpSendRequest(req, hdr, (DWORD)-1L, (void *)body, blen, blen, 0) && WinHttpReceiveResponse(req, nullptr))
		{
			size_t cap = 4096, len = 0;
			result = (char *)malloc(cap);
			DWORD avail = 0;
			while (WinHttpQueryDataAvailable(req, &avail) && avail)
			{
				if (len + avail + 1 > cap) { cap = (len + avail + 1) * 2; result = (char *)realloc(result, cap); }
				DWORD rd = 0;
				if (!WinHttpReadData(req, result + len, avail, &rd) || !rd) break;
				len += rd;
			}
			result[len] = 0;
		}
	}
	if (req) WinHttpCloseHandle(req);
	if (con) WinHttpCloseHandle(con);
	if (ses) WinHttpCloseHandle(ses);
	return result;
}

static bool json_string(const char *json, const char *key, char *out, int len)
{
	char k[64]; snprintf(k, sizeof(k), "\"%s\"", key);
	const char *p = strstr(json, k);
	if (!p) return false;
	p = strchr(p + strlen(k), ':');
	if (!p) return false;
	p++;
	while (*p == ' ') p++;
	bool quoted = *p == '"';
	if (quoted) p++;
	int i = 0;
	while (*p && i < len - 1 && (quoted ? *p != '"' : (*p != ',' && *p != '}'))) out[i++] = *p++;
	out[i] = 0;
	return i > 0;
}

bool psn_account_from_redirect(const char *url, char *b64_out, int b64_len, wchar_t *err, int err_len)
{
	const char *p = strstr(url, "code=");
	if (!p) { swprintf(err, err_len, L"No code in the address"); return false; }
	p += 5;
	char code[256]; int i = 0;
	while (*p && *p != '&' && *p != ' ' && i < 255) code[i++] = *p++;
	code[i] = 0;
	char body[1024];
	snprintf(body, sizeof(body), "grant_type=authorization_code&code=%s&scope=%s&redirect_uri=%s&", code, PSN_SCOPE, PSN_REDIRECT);
	char *r = https(L"POST", L"/2.0/oauth/token", body);
	char token[512] = {};
	if (!r || !json_string(r, "access_token", token, sizeof(token)))
	{
		swprintf(err, err_len, L"PSN sign-in failed (expired code? try again)");
		free(r);
		return false;
	}
	free(r);
	wchar_t path[700]; swprintf(path, 700, L"/2.0/oauth/token/%hs", token);
	r = https(L"GET", path, nullptr);
	char uid[64] = {};
	if (!r || !json_string(r, "user_id", uid, sizeof(uid)))
	{
		swprintf(err, err_len, L"Could not read the PSN account");
		free(r);
		return false;
	}
	free(r);
	uint64_t id = _strtoui64(uid, nullptr, 10);
	uint8_t bytes[8];
	for (int k = 0; k < 8; k++) bytes[k] = (uint8_t)(id >> (8 * k)); // little endian, like chiaki-ng
	return chiaki_base64_encode(bytes, 8, b64_out, b64_len) == CHIAKI_ERR_SUCCESS;
}
