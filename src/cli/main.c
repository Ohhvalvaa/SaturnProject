// saturn-cli: SaturnProject command line tool.
//   saturn-cli register <ip> <account-id-base64> <8-digit-code>     pairs the PS5 (Pair Device)
//   saturn-cli status                                                on / rest mode / no answer
//   saturn-cli wake                                                  wakes the PS5 from rest mode
//   saturn-cli play [seconds]                                        session: counts frames, then PS5 to rest mode
//   saturn-cli standby                                               opens a session only to put it in rest mode
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/regist.h>
#include <chiaki/session.h>
#include <chiaki/discovery.h>
#include <chiaki/base64.h>
#include "console_store.h"

static ChiakiLog g_log;
static bool g_verbose = false;

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	(void)user;
	if (!g_verbose && !(level & (CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING))) return; // levels are bits: DEBUG > INFO
	const char *l = level == CHIAKI_LOG_ERROR ? "E" : level == CHIAKI_LOG_WARNING ? "W" : level == CHIAKI_LOG_INFO ? "I" : "D";
	printf("[%s] %s\n", l, msg);
	fflush(stdout);
}

static uint64_t user_credential(const SaturnConsole *c)
{
	char hex[CHIAKI_SESSION_AUTH_SIZE + 1] = { 0 };
	memcpy(hex, c->registered.rp_regist_key, CHIAKI_SESSION_AUTH_SIZE);
	return strtoull(hex, NULL, 16);
}

// ---------- console state (UDP 9302) ----------
static const char *console_status(const char *host)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	DWORD to = 1500; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
	struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(9302); inet_pton(AF_INET, host, &a.sin_addr);
	const char *msg = "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n";
	sendto(s, msg, (int)strlen(msg), 0, (struct sockaddr *)&a, sizeof(a));
	char buf[1024]; int n = recv(s, buf, sizeof(buf) - 1, 0);
	closesocket(s);
	if (n <= 0) return "no answer";
	buf[n] = 0;
	if (strstr(buf, "200 Ok")) return "on";
	if (strstr(buf, "620")) return "rest mode";
	return "unknown";
}

// ---------- pairing ----------
static HANDLE g_regist_done;
static bool g_regist_ok;
static SaturnConsole g_new;

static void regist_cb(ChiakiRegistEvent *ev, void *user)
{
	(void)user;
	if (ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && ev->registered_host)
	{
		g_new.registered = *ev->registered_host;
		g_regist_ok = true;
	}
	SetEvent(g_regist_done);
}

static int cmd_register(const char *host, const char *account_b64, const char *pin)
{
	memset(&g_new, 0, sizeof(g_new));
	strncpy(g_new.host, host, sizeof(g_new.host) - 1);
	size_t sz = sizeof(g_new.psn_account_id);
	if (chiaki_base64_decode(account_b64, strlen(account_b64), g_new.psn_account_id, &sz) != CHIAKI_ERR_SUCCESS || sz != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		printf("Invalid Account ID (8 bytes in Base64 are needed, e.g. AbCdEfGhIjk=)\n");
		return 1;
	}
	ChiakiRegistInfo info = { 0 };
	info.target = CHIAKI_TARGET_PS5_1;
	info.host = host;
	info.broadcast = false;
	info.psn_online_id = NULL;
	memcpy(info.psn_account_id, g_new.psn_account_id, sizeof(info.psn_account_id));
	info.pin = (uint32_t)strtoul(pin, NULL, 10);
	info.console_pin = 0;
	g_regist_done = CreateEventA(NULL, TRUE, FALSE, NULL);
	ChiakiRegist regist;
	if (chiaki_regist_start(&regist, &g_log, &info, regist_cb, NULL) != CHIAKI_ERR_SUCCESS)
	{
		printf("Pairing not started\n");
		return 1;
	}
	WaitForSingleObject(g_regist_done, 30000);
	chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);
	if (!g_regist_ok) { printf("Pairing FAILED (expired code or wrong Account ID?)\n"); return 1; }
	if (!saturn_console_save(&g_new)) { printf("Paired but not saved!\n"); return 1; }
	printf("Paired: %s (%s). Saved encrypted in %s\n", g_new.registered.server_nickname, host, saturn_console_path());
	return 0;
}

// ---------- session ----------
typedef struct Stats
{
	volatile LONG frames, lost;
	volatile LONG64 bytes;
	HANDLE connected, quit;
	ChiakiQuitReason reason;
} Stats;

static bool video_cb(uint8_t *buf, size_t size, int32_t frames_lost, bool recovered, void *user)
{
	(void)buf; (void)recovered;
	Stats *s = user;
	InterlockedIncrement(&s->frames);
	InterlockedAdd(&s->lost, frames_lost);
	InterlockedAdd64(&s->bytes, (LONG64)size);
	return true;
}

static void event_cb(ChiakiEvent *ev, void *user)
{
	Stats *s = user;
	switch (ev->type)
	{
	case CHIAKI_EVENT_CONNECTED: SetEvent(s->connected); break;
	case CHIAKI_EVENT_QUIT: s->reason = ev->quit.reason; printf("Session closed: %s\n", chiaki_quit_reason_string(ev->quit.reason)); SetEvent(s->quit); break;
	case CHIAKI_EVENT_LOGIN_PIN_REQUEST: printf("The PS5 asks for the user login PIN: not supported\n"); break;
	default: break;
	}
}

static int run_session(const SaturnConsole *c, int seconds, bool only_standby)
{
	ChiakiConnectInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.ps5 = true;
	ci.host = c->host;
	memcpy(ci.regist_key, c->registered.rp_regist_key, sizeof(ci.regist_key));
	memcpy(ci.morning, c->registered.rp_key, sizeof(ci.morning));
	chiaki_connect_video_profile_preset(&ci.video_profile, CHIAKI_VIDEO_RESOLUTION_PRESET_1080p, CHIAKI_VIDEO_FPS_PRESET_60);
	ci.video_profile.codec = CHIAKI_CODEC_H265;
	ci.video_profile.bitrate = 50000;
	ci.video_profile_auto_downgrade = true;
	ci.enable_keyboard = false;
	ci.enable_dualsense = true;
	ci.audio_video_disabled = 0;
	ci.auto_regist = false;
	memcpy(ci.psn_account_id, c->psn_account_id, sizeof(ci.psn_account_id));
	ci.packet_loss_max = 0.05;
	ci.enable_idr_on_fec_failure = true;

	static ChiakiSession session;
	Stats st = { 0 };
	st.connected = CreateEventA(NULL, TRUE, FALSE, NULL);
	st.quit = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (chiaki_session_init(&session, &ci, &g_log) != CHIAKI_ERR_SUCCESS) { printf("session_init failed\n"); return 1; }
	chiaki_session_set_event_cb(&session, event_cb, &st);
	chiaki_session_set_video_sample_cb(&session, video_cb, &st);
	if (chiaki_session_start(&session) != CHIAKI_ERR_SUCCESS) { printf("session_start failed\n"); chiaki_session_fini(&session); return 1; }

	HANDLE h[2] = { st.connected, st.quit };
	if (WaitForMultipleObjects(2, h, FALSE, 20000) != WAIT_OBJECT_0)
	{
		printf("Connection failed\n");
		chiaki_session_stop(&session); chiaki_session_join(&session); chiaki_session_fini(&session);
		return 1;
	}
	printf("Connected to the PS5\n");
	if (!only_standby)
	{
		LONG last = 0; LONG64 lastb = 0;
		for (int t = 1; t <= seconds && WaitForSingleObject(st.quit, 1000) == WAIT_TIMEOUT; t++)
		{
			LONG f = st.frames; LONG64 b = st.bytes;
			printf("  %2d s: %3ld frames/s, %6.1f Mbit/s, total lost %ld\n", t, f - last, (b - lastb) * 8 / 1e6, st.lost);
			last = f; lastb = b;
		}
	}
	printf("Putting the PS5 in rest mode...\n");
	chiaki_session_goto_bed(&session);
	Sleep(1500);
	chiaki_session_stop(&session);
	chiaki_session_join(&session);
	chiaki_session_fini(&session);
	return 0;
}

int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);
	WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) g_verbose = true;
	chiaki_log_init(&g_log, CHIAKI_LOG_ALL, log_cb, NULL);
	if (chiaki_lib_init() != CHIAKI_ERR_SUCCESS) { printf("chiaki_lib_init failed\n"); return 1; }
	if (argc < 2) { printf("usage: saturn-cli register|status|wake|play [s]|standby\n"); return 1; }

	if (!strcmp(argv[1], "register"))
	{
		if (argc < 5) { printf("usage: saturn-cli register <ip> <account-id-base64> <code>\n"); return 1; }
		return cmd_register(argv[2], argv[3], argv[4]);
	}
	SaturnConsole c;
	if (!saturn_console_load(&c)) { printf("No paired PS5: use 'register' first\n"); return 1; }
	if (!strcmp(argv[1], "status")) { printf("PS5 %s: %s\n", c.host, console_status(c.host)); return 0; }
	if (!strcmp(argv[1], "wake"))
	{
		if (!strcmp(console_status(c.host), "on")) { printf("Already on\n"); return 0; }
		for (int i = 0; i < 45; i++)
		{
			chiaki_discovery_wakeup(&g_log, NULL, c.host, user_credential(&c), true);
			Sleep(2000);
			if (!strcmp(console_status(c.host), "on")) { printf("On in %d s\n", (i + 1) * 2); return 0; }
		}
		printf("The PS5 did not wake up (fully powered off?)\n");
		return 1;
	}
	if (!strcmp(argv[1], "play")) return run_session(&c, argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : 10, false);
	if (!strcmp(argv[1], "standby")) return run_session(&c, 0, true);
	printf("unknown command\n");
	return 1;
}
