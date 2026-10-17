/* wifi_proto: the ELRS/Crossfire WiFi joystick engine, without sockets.
 * Every test of ELRS-Crossfire-WiFi-Joystick-Windows/ELRSWifiJoystick.Tests that applies to the
 * engine is ported here under its C# name (Beacon, ChannelParsing, SourceLock, Stats, Misc), then
 * the value mapping, the parsers and builders, the deliberate deviations and more edge cases. */
#include "t.h"
#include "wifi_proto.h"

#include <math.h>
#include <stdint.h>

/* ---------------------------------------------------------------- harness (TestHelpers.cs) */
#define MAXREC 4096
typedef struct { uint16_t v[WIFI_MAX_CH]; int n; uint32_t src; } rec_frame_t;
typedef struct { wifi_state_t s; char d[64]; } rec_state_t;

static rec_frame_t applied[MAXREC];
static int napplied;
static char logs[MAXREC][320];
static int nlogs;
static rec_state_t states[MAXREC];
static int nstates;
static uint32_t activated[MAXREC];
static int64_t act_at[MAXREC];
static int nactivated;
static int act_result;
static struct { double hz, jit; int frames, count; } st;
static int64_t now_us;
static wifi_proto_t P;

static void on_log(void *ud, const char *msg) {
  if (nlogs < MAXREC) snprintf(logs[nlogs], sizeof logs[0], "%s", msg);
  nlogs++;
}
static void on_state(void *ud, wifi_state_t s, const char *d) {
  if (nstates < MAXREC) { states[nstates].s = s; snprintf(states[nstates].d, sizeof states[0].d, "%s", d); }
  nstates++;
}
static void on_frame(void *ud, uint32_t src, const uint16_t *raw, int n) {
  if (napplied < MAXREC) {
    memcpy(applied[napplied].v, raw, sizeof(uint16_t) * (size_t)n);
    applied[napplied].n = n;
    applied[napplied].src = src;
  }
  napplied++;
}
static void on_stats(void *ud, double hz, double jit, int frames) {
  st.hz = hz; st.jit = jit; st.frames = frames; st.count++;
}
static int on_activate(void *ud, uint32_t ip) {
  if (nactivated < MAXREC) { activated[nactivated] = ip; act_at[nactivated] = now_us; }
  nactivated++;
  return act_result;
}
static const wifi_hooks_t HOOKS = { NULL, on_log, on_state, on_frame, on_stats, on_activate };

static void H(void) {
  napplied = nlogs = nstates = nactivated = 0;
  memset(&st, 0, sizeof st);
  act_result = WIFI_ACT_OK;
  now_us = 1000000000;            /* nonzero, like the C# clock */
  wifi_proto_init(&P, &HOOKS);
}

static uint32_t IP(const char *s) {
  uint32_t ip = 0;
  if (wifi_parse_ip(s, &ip) != 1) { fprintf(stderr, "bad test IP %s\n", s); exit(2); }
  return ip;
}
static void adv(double sec) { now_us += (int64_t)llround(sec * 1e6); }
static void tick(void) { wifi_proto_tick(&P, now_us); }

#define DEF_IP "192.168.1.50"
static void pkt(const uint8_t *d, size_t n, const char *ip) { wifi_proto_packet(&P, d, n, IP(ip), now_us, now_us); }
static void str(const char *s, const char *ip) { pkt((const uint8_t *)s, strlen(s), ip); }

static size_t frame_n(uint8_t *out, int n, const int *v) {
  out[0] = 1;
  out[1] = (uint8_t)n;
  for (int i = 0; i < n; i++) { out[2 + 2 * i] = (uint8_t)(v[i] & 0xFF); out[3 + 2 * i] = (uint8_t)((v[i] >> 8) & 0xFF); }
  return 2 + 2 * (size_t)n;
}
#define NV(...) ((int)(sizeof((int[]){__VA_ARGS__}) / sizeof(int)))
#define FRAME(buf, ...) frame_n(buf, NV(__VA_ARGS__), (int[]){__VA_ARGS__})
#define SENDF(ip, ...) do { uint8_t b_[64]; size_t n_ = FRAME(b_, __VA_ARGS__); pkt(b_, n_, ip); } while (0)

static void send_same(const char *ip, int n, int v) {
  int vals[WIFI_MAX_CH + 1];
  uint8_t b[64];
  for (int i = 0; i < n; i++) vals[i] = v;
  pkt(b, frame_n(b, n, vals), ip);
}
#define PH 0xF26A
static void send_placeholder(const char *ip) { send_same(ip, 16, PH); }

static bool frame_eq(int i, int n, const int *v) {
  if (i < 0 || i >= napplied || applied[i].n != n) return false;
  for (int k = 0; k < n; k++) if (applied[i].v[k] != v[k]) return false;
  return true;
}
#define FRAME_IS(i, ...) frame_eq(i, NV(__VA_ARGS__), (int[]){__VA_ARGS__})

static int count_logs(const char *sub) {
  int c = 0;
  for (int i = 0; i < nlogs && i < MAXREC; i++) if (strstr(logs[i], sub)) c++;
  return c;
}
static int count_prefix(const char *pre) {
  int c = 0;
  for (int i = 0; i < nlogs && i < MAXREC; i++) if (!strncmp(logs[i], pre, strlen(pre))) c++;
  return c;
}
static int count_state(wifi_state_t s) {
  int c = 0;
  for (int i = 0; i < nstates && i < MAXREC; i++) if (states[i].s == s) c++;
  return c;
}
static bool has_state(wifi_state_t s, const char *d) {
  for (int i = 0; i < nstates && i < MAXREC; i++) if (states[i].s == s && !strcmp(states[i].d, d)) return true;
  return false;
}
static int count_activated(uint32_t ip) {
  int c = 0;
  for (int i = 0; i < nactivated && i < MAXREC; i++) if (activated[i] == ip) c++;
  return c;
}
static wifi_state_t last_state(void) { return nstates ? states[nstates - 1].s : (wifi_state_t)-1; }
static const char *last_detail(void) { return nstates ? states[nstates - 1].d : ""; }
static uint32_t source(void) { return P.bound ? P.bound_ip : 0; }

/* ---------------------------------------------------------------- BeaconTests.cs */
static void ElrsBeacon_TriggersActivation(void) {
  H();
  str("ELRS", "192.168.1.10");
  CHECK_INT(nactivated, 1);
  CHECK(activated[0] == IP("192.168.1.10"));
  CHECK_INT(napplied, 0);
  CHECK(count_logs("ELRS") > 0);
}

static void CrossfireBeacon_TriggersActivation_AndIsIdentified(void) {
  H();
  str("VELOCIDRONE", "192.168.2.138");
  CHECK_INT(nactivated, 1);
  CHECK(activated[0] == IP("192.168.2.138"));
  CHECK(count_logs("Crossfire") > 0);
}

static void BeaconWithTrailingBytes_IsStillABeacon(void) {
  H();
  const uint8_t d[] = { 'E', 'L', 'R', 'S', 1, 2, 3, 4, 5 };
  pkt(d, sizeof d, DEF_IP);
  CHECK_INT(nactivated, 1);
  CHECK_INT(napplied, 0);
}

static void RepeatedBeacon_IsThrottled_WithinFiveSeconds(void) {
  H();
  str("VELOCIDRONE", DEF_IP);
  adv(2);
  str("VELOCIDRONE", DEF_IP);
  CHECK_INT(nactivated, 1);
}

static void Beacon_ReactivatesAfterThrottleExpiry_WhenStillNoData(void) {
  H();
  str("VELOCIDRONE", DEF_IP);
  adv(6);
  str("VELOCIDRONE", DEF_IP);
  CHECK_INT(nactivated, 2);
}

static void Beacon_DoesNotReactivate_WhileDataIsFlowing(void) {
  H();
  str("VELOCIDRONE", "192.168.2.138");
  SENDF("192.168.2.138", 1, 2, 3, 4);
  adv(1);
  str("VELOCIDRONE", "192.168.2.138");
  CHECK_INT(nactivated, 1);
}

static void Beacon_Reactivates_AfterStreamWentStale(void) {
  H();
  str("VELOCIDRONE", "192.168.2.138");
  SENDF("192.168.2.138", 1, 2, 3, 4);
  adv(6);
  str("VELOCIDRONE", "192.168.2.138");
  CHECK_INT(nactivated, 2);
}

static void Beacon_FromSecondModule_IgnoredWhileLockedToFirst(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  adv(1);
  str("ELRS", "192.168.1.99");
  CHECK_INT(count_activated(IP("192.168.1.99")), 0);
}

static void ForcedModule_BeaconFromOtherIp_IsIgnored(void) {
  H();
  P.target = IP("10.0.0.9");
  str("VELOCIDRONE", "10.0.0.7");
  CHECK_INT(nactivated, 0);
  str("VELOCIDRONE", "10.0.0.9");
  CHECK_INT(nactivated, 1);
  CHECK(activated[0] == IP("10.0.0.9"));
}

static void FailedActivation_DoesNotStartFirewallHintTimer(void) {
  H();
  act_result = WIFI_ACT_FAILED;
  str("VELOCIDRONE", DEF_IP);
  adv(10);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 0);
}

/* ---------------------------------------------------------------- ChannelParsingTests.cs */
static void Decodes8Channels_LittleEndian(void) {
  H();
  SENDF(DEF_IP, 0x1234, 0, 32767, 5000, 6000, 7000, 8000, 9000);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 0x1234, 0, 32767, 5000, 6000, 7000, 8000, 9000));
}

static void Accepts16Channels_CrossfireAlwaysSends16(void) {
  H();
  int v[16];
  uint8_t b[64];
  for (int i = 0; i < 16; i++) v[i] = 100 + i;
  pkt(b, frame_n(b, 16, v), DEF_IP);
  CHECK_INT(napplied, 1);
  CHECK(frame_eq(0, 16, v));
}

static void AcceptsMinimumOf4Channels(void) {
  H();
  SENDF(DEF_IP, 10, 20, 30, 40);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 10, 20, 30, 40));
}

static void RejectsFewerThan4Channels(void) {
  H();
  SENDF(DEF_IP, 10, 20, 30);
  CHECK_INT(napplied, 0);
}

static void RejectsMoreThan16Channels(void) {
  H();
  uint8_t d[2 + 17 * 2] = { 1, 17 };
  pkt(d, sizeof d, DEF_IP);
  CHECK_INT(napplied, 0);
}

static void RejectsTruncatedFrame(void) {
  H();
  uint8_t b[64];
  FRAME(b, 1, 2, 3, 4, 5, 6, 7, 8);
  pkt(b, 9, DEF_IP);
  CHECK_INT(napplied, 0);
}

static void RejectsTooShortPackets_WithoutCrashing(void) {
  static const uint8_t p1[] = { 1 }, p2[] = { 1, 8 };
  const uint8_t *pk[3] = { NULL, p1, p2 };
  const size_t len[3] = { 0, 1, 2 };
  for (int i = 0; i < 3; i++) {
    H();
    pkt(pk[i], len[i], DEF_IP);
    CHECK_INT(napplied, 0);
    CHECK_INT(nactivated, 0);
  }
}

static void ClampsOverrangeValues_To15BitMax(void) {
  H();
  SENDF(DEF_IP, 0x816B, 0xFFFF, 100, 32767);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 32767, 32767, 100, 32767));
}

static void AllPlaceholderFrame_IsNotAJoystickSource(void) {
  H();
  send_placeholder(DEF_IP);
  CHECK_INT(napplied, 0);                 /* neither Apply nor ChannelsUpdated */
  CHECK_INT(count_state(WIFI_ST_STREAMING), 0);
  CHECK(!P.bound);
  CHECK(count_logs("no stick data") > 0);
}

static void AllPlaceholderFrame_WarnsOnce_NotPerFrame(void) {
  H();
  for (int i = 0; i < 50; i++) send_placeholder(DEF_IP);
  CHECK_INT(count_logs("no stick data"), 1);
}

static void RealStickData_StillLocksAndStreams_AfterPlaceholders(void) {
  H();
  send_placeholder(DEF_IP);
  SENDF(DEF_IP, 16384, 16384, 0, 16384);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 16384, 16384, 0, 16384));
  CHECK_INT(last_state(), WIFI_ST_STREAMING);
}

static void OvershootFrame_IsClampedAndApplied(void) {
  H();
  SENDF(DEF_IP, 0x816B, 1000, 2000, 3000);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 32767, 1000, 2000, 3000));
  CHECK_INT(last_state(), WIFI_ST_STREAMING);
}

static void CorruptFrame_MidStream_IsDroppedSilently(void) {
  H();
  SENDF(DEF_IP, 1000, 2000, 3000, 4000);
  int sb = nstates, lb = nlogs;
  SENDF(DEF_IP, 62058, 62058, 62058, 56051, 556, 35904, 212, 11347);
  CHECK_INT(napplied, 1);
  CHECK_INT(nstates, sb);
  CHECK_INT(nlogs, lb);
  CHECK_INT(last_state(), WIFI_ST_STREAMING);
}

static void PlaceholderFlood_ReleasesLock_ThenWarns(void) {
  H();
  SENDF(DEF_IP, 1000, 2000, 3000, 4000);
  adv(3.1);
  send_placeholder(DEF_IP);
  CHECK(!P.bound);
  CHECK_INT(last_state(), WIFI_ST_SEARCHING);
  CHECK(count_logs("no stick data") > 0);
}

static void FrameTypeByte_IsNotValidated(void) {
  H();
  uint8_t b[64];
  size_t n = FRAME(b, 1, 2, 3, 4);
  b[0] = 0;
  pkt(b, n, DEF_IP);
  CHECK_INT(napplied, 1);
}

static void ChannelsUpdatedEvent_CarriesTheSameValues(void) {
  H();
  SENDF(DEF_IP, 11, 22, 33, 44);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 11, 22, 33, 44));
  CHECK(applied[0].src == IP(DEF_IP));
}

static void ZeroValues_PassThrough(void) {
  H();
  SENDF(DEF_IP, 0, 0, 0, 0);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 0, 0, 0, 0));
}

/* ---------------------------------------------------------------- SourceLockTests.cs */
static void FirstChannelFrame_LocksSource_AndFiresStreaming(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  CHECK(source() == IP("192.168.1.10"));
  CHECK(has_state(WIFI_ST_STREAMING, "192.168.1.10"));
}

static void SecondSource_IsIgnoredWhileLockIsFresh(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  adv(1);
  SENDF("192.168.1.99", 9, 9, 9, 9);
  CHECK_INT(napplied, 1);
  CHECK(source() == IP("192.168.1.10"));
}

static void SecondSource_WarnsOnlyOnce(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  adv(1);
  for (int i = 0; i < 3; i++) SENDF("192.168.1.99", 9, 9, 9, 9);
  CHECK_INT(count_logs("Ignoring 2nd source"), 1);
  CHECK_INT(count_logs("Ignoring 2nd source 192.168.1.99 (locked to 192.168.1.10)"), 1);
}

static void StaleLock_ReleasesToNewSource(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  adv(3.5);
  SENDF("192.168.1.99", 5, 6, 7, 8);
  CHECK(source() == IP("192.168.1.99"));
  CHECK_INT(count_state(WIFI_ST_STREAMING), 2);
  CHECK_INT(napplied, 2);
  CHECK_INT(count_logs("went quiet"), 0);       /* the reference releases this one silently */
}

static void ForcedModule_ChannelsFromOtherIp_AreIgnored(void) {
  H();
  P.target = IP("10.0.0.9");
  SENDF("10.0.0.7", 1, 2, 3, 4);
  CHECK_INT(napplied, 0);
  SENDF("10.0.0.9", 1, 2, 3, 4);
  CHECK_INT(napplied, 1);
  CHECK(source() == IP("10.0.0.9"));
}

static void SessionReset_AllowsRelock_RegressionForStopStartBug(void) {
  H();
  SENDF("192.168.2.138", 1, 2, 3, 4);
  CHECK_INT(count_state(WIFI_ST_STREAMING), 1);
  wifi_proto_reset(&P);
  SENDF("192.168.2.138", 1, 2, 3, 4);
  CHECK_INT(count_state(WIFI_ST_STREAMING), 2);
  CHECK(source() == IP("192.168.2.138"));
}

static void Tick_ReleasesStaleLock_AndReturnsToSearching(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  adv(4);
  tick();
  CHECK(!P.bound);
  CHECK(count_logs("went quiet") > 0);
  CHECK_INT(count_logs("Source 192.168.1.10 went quiet - releasing lock"), 1);
  CHECK(has_state(WIFI_ST_SEARCHING, "waiting for module"));
}

static void Tick_DoesNotReleaseFreshLock(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  adv(1);
  tick();
  CHECK(source() == IP("192.168.1.10"));
}

static void FirewallHint_ShownOnce_WhenActivatedButNoData(void) {
  H();
  str("VELOCIDRONE", "192.168.2.138");
  adv(5);
  tick();
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 1);
  CHECK_INT(last_state(), WIFI_ST_SEARCHING);
}

static void FirewallHint_NotShown_WhenDataArrived(void) {
  H();
  str("VELOCIDRONE", "192.168.2.138");
  SENDF("192.168.2.138", 1, 2, 3, 4);
  adv(5);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 0);
}

static void SetTarget_TrimsIp_AndUnlocksCurrentSource(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  CHECK_INT(wifi_proto_set_target(&P, "  192.168.2.138  ", now_us), 0);
  CHECK(P.target == IP("192.168.2.138"));
  CHECK(!P.bound);
  SENDF("192.168.1.10", 9, 9, 9, 9);
  CHECK_INT(napplied, 1);
}

static void SetTarget_NullOrWhitespace_ClearsTarget(void) {
  const char *cases[3] = { NULL, "", "   " };
  for (int i = 0; i < 3; i++) {
    H();
    P.target = IP("10.0.0.9");
    CHECK_INT(wifi_proto_set_target(&P, cases[i], now_us), 0);
    CHECK(P.target == 0);
    CHECK_INT(nlogs, 0);
    CHECK_INT(nactivated, 0);
  }
}

/* ---------------------------------------------------------------- StatsTests.cs */
static void SteadyStream_Reports100Hz_ZeroJitter(void) {
  H();
  for (int i = 0; i <= 100; i++) wifi_proto_add_arrival(&P, (int64_t)i * 10000);
  wifi_proto_emit_stats(&P, now_us);
  CHECK(st.hz >= 99.0 && st.hz <= 101.0);
  CHECK(st.jit >= 0.0 && st.jit <= 0.05);
  CHECK_INT(st.frames, 101);
}

static void AlternatingGaps_ReportCorrectJitter(void) {
  H();
  int64_t t = 0;
  wifi_proto_add_arrival(&P, 0);
  for (int i = 0; i < 100; i++) {
    t += (i % 2 == 0 ? 5 : 15) * 1000;
    wifi_proto_add_arrival(&P, t);
  }
  wifi_proto_emit_stats(&P, now_us);
  CHECK(st.hz >= 99.0 && st.hz <= 101.0);
  CHECK(st.jit >= 4.9 && st.jit <= 5.1);
}

static void StatsReset_AfterEachEmit(void) {
  H();
  for (int i = 0; i < 10; i++) wifi_proto_add_arrival(&P, (int64_t)i * 10000);
  wifi_proto_emit_stats(&P, now_us);
  CHECK_INT(st.frames, 10);
  wifi_proto_emit_stats(&P, now_us);
  CHECK_INT(st.frames, 0);
  CHECK(st.hz == 0);
}

static void TooFewSamples_ReportZeroRate_WithoutCrashing(void) {
  H();
  wifi_proto_add_arrival(&P, 12345);
  wifi_proto_emit_stats(&P, now_us);
  CHECK(st.hz == 0);
  CHECK(st.jit == 0);
  CHECK_INT(st.frames, 1);
}

static void RealPackets_AreCountedInStats(void) {
  H();
  for (int i = 0; i < 5; i++) SENDF(DEF_IP, 1, 2, 3, 4);
  wifi_proto_emit_stats(&P, now_us);
  CHECK_INT(st.frames, 5);
}

/* ---------------------------------------------------------------- MiscTests.cs */
static void StartsWith_Cases(void) {
  struct { const char *d, *p; bool want; } c[] = {
    { "ELRS", "ELRS", true }, { "ELRS123", "ELRS", true }, { "ELR", "ELRS", false }, { "XLRS", "ELRS", false },
    { "VELOCIDRONE", "VELOCIDRONE", true }, { "VELOCIDRONX", "VELOCIDRONE", false }, { "anything", "", true },
  };
  for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
    CHECK(wifi_starts_with((const uint8_t *)c[i].d, strlen(c[i].d), c[i].p) == c[i].want);
  CHECK(wifi_starts_with(NULL, 0, ""));
}

static void OutputOverride_ReceivesEveryFrame(void) {
  H();
  for (int i = 0; i < 3; i++) SENDF(DEF_IP, i, i, i, i);
  CHECK_INT(napplied, 3);
}

static uint32_t xs = 1234;
static uint32_t xorshift(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static void LogNeverThrows_OnGarbageInput(void) {
  H();
  uint8_t d[64];
  int total = 0;
  for (int i = 0; i < 500; i++) {
    size_t n = xorshift() % 64;
    for (size_t k = 0; k < n; k++) d[k] = (uint8_t)xorshift();
    pkt(d, n, DEF_IP);
    total++;
  }
  /* table overflow: more beacons from distinct IPs than any table holds */
  for (int i = 0; i < 1000; i++) {
    char ip[32];
    snprintf(ip, sizeof ip, "10.0.%d.%d", i / 250, i % 250);
    str("VELOCIDRONE", ip);
    total++;
  }
  /* random bytes from random sources, also mid-stream */
  SENDF(DEF_IP, 1, 2, 3, 4);
  total++;
  for (int i = 0; i < 2000; i++) {
    size_t n = xorshift() % 40;
    for (size_t k = 0; k < n; k++) d[k] = (uint8_t)xorshift();
    if (n > 1 && (i & 1)) d[1] = (uint8_t)(4 + xorshift() % 13);    /* plausible count */
    uint32_t src = xorshift() | 1;
    wifi_proto_packet(&P, d, n, src, now_us, now_us);
    tick();
    adv(0.003);
    total++;
  }
  CHECK_INT(P.rx, total);
  CHECK(P.nact <= WIFI_TABLE && P.nseen <= WIFI_TABLE && P.nwarned <= WIFI_TABLE && P.nnostick <= WIFI_TABLE);
  CHECK(count_logs("Ignoring 2nd source") <= WIFI_TABLE * 2);
}

/* ---------------------------------------------------------------- view-model equivalents */
static void TickBars_Percent(void) {
  CHECK_INT(wifi_raw_percent(32767), 100);
  CHECK_INT(wifi_raw_percent(16384), 50);
  CHECK_INT(wifi_raw_percent(0), 0);
  CHECK_INT(wifi_raw_percent(8192), 25);
  CHECK_INT(wifi_raw_percent(-5), 0);
  CHECK_INT(wifi_raw_percent(70000), 100);
}

/* ---------------------------------------------------------------- N1 value mapping */
static int elrs_raw_of(int ch) {        /* EdgeTX crossfire.cpp + ELRS wifiJoystick.cpp */
  int crsf = 992 + (ch * 4) / 5;
  if (crsf < 0) crsf = 0;
  if (crsf > 1984) crsf = 1984;
  int c = crsf < 172 ? 172 : crsf > 1811 ? 1811 : crsf;
  return (int)((long)(c - 172) * 32767 / 1639);
}

static void RawToStick_Vectors(void) {
  static const int v[][2] = {
    { 0, -1024 }, { 1, -1024 }, { 19, -1024 }, { 20, -1024 }, { 39, -1023 }, { 8192, -513 }, { 8216, -511 },
    { 16383, -1 }, { 16384, 0 }, { 16393, 0 }, { 16413, 1 }, { 24570, 511 }, { 24576, 511 }, { 32747, 1023 },
    { 32766, 1024 }, { 32767, 1024 }, { 33131, 1024 }, { 65535, 1024 }, { -1, -1024 }, { -100000, -1024 },
  };
  for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
    int got = wifi_raw_to_stick(v[i][0]);
    if (got != v[i][1]) fprintf(stderr, "raw %d -> %d, expected %d\n", v[i][0], got, v[i][1]);
    CHECK_INT(got, v[i][1]);
  }
  /* the EdgeTX -> CRSF -> ELRS chain end to end */
  CHECK_INT(elrs_raw_of(-1024), 19);
  CHECK_INT(elrs_raw_of(0), 16393);
  CHECK_INT(elrs_raw_of(1024), 32767);
  CHECK_INT(elrs_raw_of(-512), 8216);
  CHECK_INT(elrs_raw_of(512), 24570);
  int maxerr = 0, prev = -2000;
  bool mono = true;
  for (int ch = -1024; ch <= 1024; ch++) {
    int back = wifi_raw_to_stick(elrs_raw_of(ch));
    int e = abs(back - ch);
    if (e > maxerr) maxerr = e;
    if (back < prev) mono = false;
    prev = back;
  }
  CHECK(maxerr <= 1);
  CHECK(mono);
  CHECK_INT(wifi_raw_to_stick(elrs_raw_of(0)), 0);
  CHECK_INT(wifi_raw_to_stick(elrs_raw_of(1024)), 1024);
  CHECK_INT(wifi_raw_to_stick(elrs_raw_of(-1024)), -1024);
  /* extended limits clamp */
  CHECK_INT(wifi_raw_to_stick(elrs_raw_of(1536)), 1024);
  CHECK_INT(wifi_raw_to_stick(elrs_raw_of(-1536)), -1024);
  /* monotonic over the whole raw range, symmetric around the center */
  prev = -2000;
  mono = true;
  for (int r = 0; r <= 32767; r++) { int s = wifi_raw_to_stick(r); if (s < prev) mono = false; prev = s; }
  CHECK(mono);
}

/* ---------------------------------------------------------------- N2 ELRS beacon */
static void ElrsBeacon_Parse(void) {
  const uint8_t b[] = { 0x45, 0x4c, 0x52, 0x53, 0x01, 0xf8, 0x2a, 0x0a, 'U', 'n', 'i', 'f', 'i', 'e', 'd', ' ', 'T', 'X' };
  int ver = 0, port = 0;
  char name[32];
  CHECK_INT(wifi_parse_elrs_beacon(b, sizeof b, &ver, &port, name, sizeof name), 1);
  CHECK_INT(ver, 1);
  CHECK_INT(port, 11000);
  CHECK_STR(name, "Unified TX");
  /* "ELRS" alone: still a beacon, no header */
  CHECK_INT(wifi_parse_elrs_beacon(b, 4, &ver, &port, name, sizeof name), 0);
  CHECK_INT(ver, -1);
  CHECK_INT(port, -1);
  CHECK_STR(name, "");
  CHECK_INT(wifi_parse_elrs_beacon(b, 7, &ver, &port, name, sizeof name), 0);
  /* exactly the header, name_len says 10 but nothing follows */
  CHECK_INT(wifi_parse_elrs_beacon(b, 8, &ver, &port, name, sizeof name), 1);
  CHECK_STR(name, "");
  /* name_len 200 with 3 bytes present; non-printables -> '?' */
  const uint8_t c[] = { 'E', 'L', 'R', 'S', 1, 0x34, 0x12, 200, 'A', 0x07, 0xC3 };
  CHECK_INT(wifi_parse_elrs_beacon(c, sizeof c, &ver, &port, name, sizeof name), 1);
  CHECK_INT(port, 0x1234);
  CHECK_STR(name, "A??");
  /* long name: at most namecap - 1 */
  uint8_t d[8 + 60];
  memcpy(d, "ELRS\x01\xf8\x2a", 7);
  d[7] = 60;
  for (int i = 0; i < 60; i++) d[8 + i] = (uint8_t)('a' + i % 26);
  CHECK_INT(wifi_parse_elrs_beacon(d, sizeof d, &ver, &port, name, sizeof name), 1);
  CHECK_INT(strlen(name), 31);
  char tiny[1] = { 'x' };
  CHECK_INT(wifi_parse_elrs_beacon(d, sizeof d, NULL, NULL, tiny, sizeof tiny), 1);
  CHECK_INT(tiny[0], 0);
  CHECK_INT(wifi_parse_elrs_beacon(d, sizeof d, NULL, NULL, NULL, 0), 1);
  /* name_len 0 */
  d[7] = 0;
  CHECK_INT(wifi_parse_elrs_beacon(d, sizeof d, NULL, NULL, name, sizeof name), 1);
  CHECK_STR(name, "");
  /* not a beacon */
  CHECK_INT(wifi_parse_elrs_beacon((const uint8_t *)"VELOCIDRONE", 11, &ver, &port, name, sizeof name), -1);
  CHECK_INT(wifi_parse_elrs_beacon((const uint8_t *)"ELR", 3, &ver, &port, name, sizeof name), -1);
  CHECK_INT(wifi_parse_elrs_beacon(NULL, 0, &ver, &port, name, sizeof name), -1);
}

/* the engine remembers kind and name per beacon source; bad beacon lengths never crash it */
static void Beacons_KindAndName(void) {
  H();
  const uint8_t b[] = { 'E', 'L', 'R', 'S', 1, 0xf8, 0x2a, 4, 'M', 'y', 'T', 'X' };
  pkt(b, sizeof b, "10.0.0.1");
  str("VELOCIDRONE", "10.0.0.2");
  CHECK_INT(wifi_proto_kind(&P, IP("10.0.0.1")), WIFI_KIND_ELRS);
  CHECK_STR(wifi_proto_module_name(&P, IP("10.0.0.1")), "MyTX");
  CHECK_INT(wifi_proto_kind(&P, IP("10.0.0.2")), WIFI_KIND_XF);
  CHECK_STR(wifi_proto_module_name(&P, IP("10.0.0.2")), "");
  CHECK_INT(wifi_proto_kind(&P, IP("10.0.0.3")), WIFI_KIND_UNKNOWN);
  CHECK_STR(wifi_proto_module_name(&P, IP("10.0.0.3")), "");
  CHECK_INT(nactivated, 2);
  CHECK_INT(count_logs("Discovery beacon from ELRS module 10.0.0.1 - activating"), 1);
  CHECK_INT(count_logs("Discovery beacon from Crossfire module 10.0.0.2 - activating"), 1);
  /* name_len larger than the packet, and a header cut short: still beacons, no channels */
  const uint8_t lie[] = { 'E', 'L', 'R', 'S', 1, 0xf8, 0x2a, 250, 'Z' };
  pkt(lie, sizeof lie, "10.0.0.4");
  pkt(b, 5, "10.0.0.5");
  str("ELRS", "10.0.0.6");
  CHECK_STR(wifi_proto_module_name(&P, IP("10.0.0.4")), "Z");
  CHECK_INT(wifi_proto_kind(&P, IP("10.0.0.5")), WIFI_KIND_ELRS);
  CHECK_INT(nactivated, 5);
  CHECK_INT(napplied, 0);
  CHECK_INT(P.beacons, 5);
  /* "EL" / "ELR" are not beacons: too short, or a frame with count 'L' */
  str("EL", "10.0.0.7");
  str("ELR", "10.0.0.7");
  CHECK_INT(P.beacons, 5);
  CHECK_INT(P.malformed, 2);
  /* reset forgets them */
  wifi_proto_reset(&P);
  CHECK_INT(wifi_proto_kind(&P, IP("10.0.0.1")), WIFI_KIND_UNKNOWN);
  /* more modules than the table: the oldest is forgotten, never an overflow */
  for (int i = 0; i < WIFI_TABLE + 3; i++) {
    char ip[32];
    snprintf(ip, sizeof ip, "10.1.0.%d", i + 1);
    str("VELOCIDRONE", ip);
    adv(0.01);
  }
  CHECK_INT(P.nseen, WIFI_TABLE);
  CHECK_INT(wifi_proto_kind(&P, IP("10.1.0.1")), WIFI_KIND_UNKNOWN);
  CHECK_INT(wifi_proto_kind(&P, IP("10.1.0.11")), WIFI_KIND_XF);
}

/* ---------------------------------------------------------------- N3 request builders */
static void Post_Bytes(void) {
  char body[64], buf[512];
  CHECK_INT(wifi_build_body(body, sizeof body, "joystick_begin", 4000, 16), 47);
  CHECK_STR(body, "action=joystick_begin&interval=4000&channels=16");
  CHECK_INT(wifi_build_body(body, sizeof body, "joystick_begin", 10000, 8), 47);
  CHECK_INT(wifi_build_body(body, 47, "joystick_begin", 4000, 16), -1);     /* no room for the NUL */
  CHECK_INT(wifi_build_body(body, 48, "joystick_begin", 4000, 16), 47);
  const char *want =
    "POST /udpcontrol HTTP/1.1\r\n"
    "Host: 192.168.1.10\r\n"
    "User-Agent: sticktime-pi\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 47\r\n"
    "Connection: close\r\n"
    "\r\n"
    "action=joystick_begin&interval=4000&channels=16";
  wifi_build_body(body, sizeof body, "joystick_begin", 4000, 16);
  int n = wifi_build_post(buf, sizeof buf, IP("192.168.1.10"), 80, body);
  CHECK_INT(n, (int)strlen(want));
  CHECK_STR(buf, want);
  n = wifi_build_post(buf, sizeof buf, IP("127.0.0.2"), 8080, "action=joystick_end&interval=1000&channels=4");
  CHECK(n > 0);
  CHECK(strstr(buf, "\r\nHost: 127.0.0.2:8080\r\n") != NULL);
  CHECK(strstr(buf, "\r\nContent-Length: 44\r\n") != NULL);
  CHECK(strstr(buf, "\r\n\r\naction=joystick_end&interval=1000&channels=4") != NULL);
  CHECK_INT(wifi_build_post(buf, 20, IP("127.0.0.2"), 8080, body), -1);
  CHECK_INT(wifi_build_post(buf, (size_t)strlen(want), IP("192.168.1.10"), 80, body), -1);
  CHECK_INT(wifi_build_post(buf, (size_t)strlen(want) + 1, IP("192.168.1.10"), 80, body), (int)strlen(want));
}

/* ---------------------------------------------------------------- N4 HTTP status */
static void HttpStatus_Parse(void) {
  char r[32];
#define HS(s, rc) wifi_parse_http_status(s, strlen(s), r, rc)
  CHECK_INT(HS("HTTP/1.1 200 OK\r\n", sizeof r), 200); CHECK_STR(r, "OK");
  CHECK_INT(HS("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n", sizeof r), 404); CHECK_STR(r, "Not Found");
  CHECK_INT(HS("HTTP/1.1 200\r\n", sizeof r), 200); CHECK_STR(r, "");
  CHECK_INT(HS("HTTP/1.1 20", sizeof r), -1);
  CHECK_INT(HS("", sizeof r), -1);
  CHECK_INT(HS("HTT", sizeof r), -1);
  CHECK_INT(HS("HTTP/1.1 200 OK", sizeof r), -1);
  CHECK_INT(HS("garbage\r\n", sizeof r), 0);
  CHECK_INT(HS("garbage", sizeof r), 0);
  CHECK_INT(HS("HTTX/1.1 200 OK\r\n", sizeof r), 0);
  CHECK_INT(HS("HTTP/1.1 999 X\r\n", sizeof r), 999); CHECK_STR(r, "X");
  CHECK_INT(HS("HTTP/1.1 099 X\r\n", sizeof r), 0);
  CHECK_INT(HS("HTTP/1.1 2000\r\n", sizeof r), 0);
  CHECK_INT(HS("HTTP/1.1 abc\r\n", sizeof r), 0);
  CHECK_INT(HS("HTTP/1.1\r\n", sizeof r), 0);
  CHECK_INT(HS("HTTP/ 200 OK\r\n", sizeof r), 0);
  CHECK_INT(HS("HTTP/1.1 20\r\n", sizeof r), 0);
  CHECK_INT(HS("HTTP/1.1  500  Internal Server Error  \r\n", sizeof r), 500); CHECK_STR(r, "Internal Server Error");
  CHECK_INT(HS("HTTP/1.0 204 No Content\n\n", sizeof r), 204); CHECK_STR(r, "No Content");
  CHECK_INT(HS("HTTP/1.1 200 A very long reason phrase that does not fit\r\n", 8), 200); CHECK_STR(r, "A very");    /* cut, then trailing blanks trimmed */
  CHECK_INT(HS("HTTP/1.1 200 \x01\x02\r\n", sizeof r), 200); CHECK_STR(r, "??");
  CHECK_INT(wifi_parse_http_status("HTTP/1.1 301 Moved\r\n", 20, NULL, 0), 301);
  CHECK_INT(wifi_parse_http_status("\n", 1, r, sizeof r), 0);
#undef HS
  /* Content-Length of a head */
#define CL(s) wifi_http_content_length(s, strlen(s))
  CHECK_INT(CL("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok"), 2);
  CHECK_INT(CL("HTTP/1.1 200 OK\r\ncontent-length:   12  \r\n\r\n"), 12);
  CHECK_INT(CL("HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 0\r\n\r\n"), 0);
  CHECK_INT(CL("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nContent-Length: 5\r\n"), -1);   /* in the body */
  CHECK_INT(CL("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n"), -1);
  CHECK_INT(CL("HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n"), -1);
  CHECK_INT(CL("HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n"), -1);
  CHECK_INT(CL("HTTP/1.0 200 OK\nContent-Length: 7\n\n"), 7);
  CHECK_INT(CL(""), -1);
#undef CL
}

/* ---------------------------------------------------------------- N5 IP parsing */
static void ParseIp_Cases(void) {
  uint32_t ip = 0;
  CHECK_INT(wifi_parse_ip("192.168.1.10", &ip), 1);
  uint8_t b[4];
  memcpy(b, &ip, 4);
  CHECK(b[0] == 192 && b[1] == 168 && b[2] == 1 && b[3] == 10);     /* network byte order */
  CHECK_INT(wifi_parse_ip(" 10.0.0.1\t", &ip), 1);
  CHECK_INT(wifi_parse_ip("1.2.3.4\r\n", &ip), 1);
  CHECK_INT(wifi_parse_ip("0.0.0.0", &ip), 1);
  CHECK_INT(wifi_parse_ip("255.255.255.255", &ip), 1);
  CHECK_INT(wifi_parse_ip("", &ip), 0);
  CHECK_INT(wifi_parse_ip("  ", &ip), 0);
  CHECK_INT(wifi_parse_ip("\t\r\n", &ip), 0);
  CHECK_INT(wifi_parse_ip(NULL, &ip), 0);
  const char *bad[] = { "10.0.0", "256.1.1.1", "elrs_tx.local", "10.0.0.1:80", "01.2.3.4", "1.2.3.04", "1.2.3.4.5",
                        "1..2.3", "-1.2.3.4", "+1.2.3.4", "1.2.3.4 x", "1 .2.3.4", "1.2.3.", ".1.2.3", "1.2.3.0400",
                        "1.2.3.1000", "abc", "1,2,3,4", "::1", "0x7f.0.0.1", "1.2.3.4/24" };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    ip = 0x5a5a5a5a;
    int r = wifi_parse_ip(bad[i], &ip);
    if (r != -1) fprintf(stderr, "parse_ip(\"%s\") = %d\n", bad[i], r);
    CHECK_INT(r, -1);
    CHECK(ip == 0x5a5a5a5a);           /* untouched */
  }
  CHECK_INT(wifi_parse_ip("10.0.0.1", NULL), 1);
  char s[16];
  wifi_ip_str(IP("192.168.1.10"), s);
  CHECK_STR(s, "192.168.1.10");
  wifi_ip_str(IP("255.255.255.255"), s);
  CHECK_STR(s, "255.255.255.255");
  wifi_ip_str(0, s);
  CHECK_STR(s, "0.0.0.0");
}

/* ---------------------------------------------------------------- N6 tick starvation (deviation 1) */
static void TickStarvation_IgnoredTrafficCannotBlockTheTick(void) {
  H();
  P.target = IP("10.0.0.9");
  SENDF("10.0.0.9", 1, 2, 3, 4);
  int64_t t0 = now_us, released = 0;
  for (int i = 1; i <= 1000; i++) {          /* 10 s of 100 Hz frames from another module */
    now_us = t0 + (int64_t)i * 10000;
    SENDF("10.0.0.7", 5, 5, 5, 5);
    tick();
    if (!released && !P.bound) released = now_us - t0;
  }
  CHECK(released > 3000000 && released <= 3010000);
  CHECK_INT(count_logs("went quiet"), 1);
  CHECK_INT(napplied, 1);
  CHECK_INT(nactivated, 2);
  CHECK(activated[0] == IP("10.0.0.9") && activated[1] == IP("10.0.0.9"));
  CHECK(act_at[0] - t0 == released);
  CHECK(act_at[1] - act_at[0] >= WIFI_THROTTLE_US && act_at[1] - act_at[0] <= WIFI_THROTTLE_US + 10000);
  CHECK_INT(count_logs("Ignoring 2nd source"), 0);   /* filtered before the lock logic */
  CHECK(P.dropped_other >= 1000);
}

/* ---------------------------------------------------------------- N7 set_target POSTs at once (deviation 1, 5) */
static void SetTarget_PostsAtOnce_AndReportsState(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  CHECK_INT(wifi_proto_set_target(&P, "192.168.2.138", now_us), 0);
  CHECK_INT(nactivated, 1);
  CHECK(activated[0] == IP("192.168.2.138"));
  CHECK_INT(last_state(), WIFI_ST_SEARCHING);
  CHECK_STR(last_detail(), "connecting to 192.168.2.138");
  CHECK_INT(count_logs("Target module set to 192.168.2.138 - connecting..."), 1);
  for (int i = 0; i < 5; i++) SENDF("192.168.1.10", 9, 9, 9, 9);
  CHECK_INT(napplied, 1);
  /* the throttle entry of the new target is dropped, so a repeat posts again at once */
  adv(1);
  CHECK_INT(wifi_proto_set_target(&P, "192.168.2.138", now_us), 0);
  CHECK_INT(nactivated, 2);
  /* while unbound in explicit mode, the tick re-posts every 5 s (not before) */
  adv(4.9);
  tick();
  CHECK_INT(nactivated, 2);
  adv(0.1);
  tick();
  CHECK_INT(nactivated, 3);
  /* data from the target locks it; no more posts */
  SENDF("192.168.2.138", 1, 1, 1, 1);
  CHECK(source() == IP("192.168.2.138"));
  adv(6);
  SENDF("192.168.2.138", 1, 1, 1, 1);
  tick();
  CHECK_INT(nactivated, 3);
  /* back to discovery: the lock stays, no log, no post */
  int nl = nlogs;
  CHECK_INT(wifi_proto_set_target(&P, "", now_us), 0);
  CHECK(P.target == 0 && P.bound);
  CHECK_INT(nlogs, nl);
}

static void SetTarget_Invalid_KeepsTarget(void) {
  H();
  P.target = IP("10.0.0.9");
  CHECK_INT(wifi_proto_set_target(&P, "abc", now_us), -1);
  CHECK(P.target == IP("10.0.0.9"));
  CHECK_INT(count_logs("Invalid module IP \"abc\" - use a.b.c.d"), 1);
  CHECK_INT(wifi_proto_set_target(&P, "10.0.0", now_us), -1);
  CHECK_INT(wifi_proto_set_target(&P, "0.0.0.0", now_us), -1);
  CHECK_INT(wifi_proto_set_target(&P, "elrs_tx.local", now_us), -1);
  CHECK(P.target == IP("10.0.0.9"));
  CHECK_INT(nactivated, 0);
  /* control characters never reach the log as such; long text is cut */
  CHECK_INT(wifi_proto_set_target(&P, "1.2.3.4\nfake line", now_us), -1);
  CHECK(count_logs("1.2.3.4?fake line") == 1);
  for (int i = 0; i < nlogs; i++) CHECK(strchr(logs[i], '\n') == NULL);
  char longs[300];
  memset(longs, 'x', sizeof longs - 1);
  longs[sizeof longs - 1] = 0;
  CHECK_INT(wifi_proto_set_target(&P, longs, now_us), -1);
  CHECK(strlen(logs[nlogs - 1]) < 80);
}

static void SetTarget_SameAsBound_Relocks(void) {
  H();
  SENDF("10.0.0.9", 1, 2, 3, 4);
  CHECK_INT(wifi_proto_set_target(&P, "10.0.0.9", now_us), 0);
  CHECK(!P.bound);
  CHECK_INT(nactivated, 1);
  SENDF("10.0.0.9", 1, 2, 3, 4);
  CHECK(source() == IP("10.0.0.9"));
  CHECK_INT(count_state(WIFI_ST_STREAMING), 2);
}

/* ---------------------------------------------------------------- N8 asynchronous activation */
static void ActivationAsync_HintTimerStartsAtDone(void) {
  H();
  act_result = WIFI_ACT_QUEUED;
  str("VELOCIDRONE", "192.168.2.138");
  CHECK_INT(nactivated, 1);
  CHECK(!P.first_act_set);
  str("VELOCIDRONE", "192.168.2.138");      /* a queued request is throttled like a finished one */
  CHECK_INT(nactivated, 1);
  adv(1);
  wifi_proto_activation_done(&P, IP("192.168.2.138"), true, now_us);
  adv(3.5);                 /* 4.5 s after the request, 3.5 s after done */
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 0);
  adv(0.6);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 1);
  CHECK(strstr(last_detail(), "no data") != NULL);
}

static void ActivationAsync_FailedDone_NoHint(void) {
  H();
  act_result = WIFI_ACT_QUEUED;
  str("VELOCIDRONE", "192.168.2.138");
  adv(1);
  wifi_proto_activation_done(&P, IP("192.168.2.138"), false, now_us);
  adv(10);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 0);
}

/* deviation: ELRS streams before it answers, so the answer may come after the lock */
static void ActivationDone_WhileStreamingFromIt_DoesNotArmHint(void) {
  H();
  act_result = WIFI_ACT_QUEUED;
  str("ELRS", "10.0.0.1");
  SENDF("10.0.0.1", 1, 2, 3, 4);
  wifi_proto_activation_done(&P, IP("10.0.0.1"), true, now_us);
  CHECK(!P.first_act_set);
  adv(3.5);
  tick();
  CHECK(!P.bound);
  CHECK_INT(count_logs("went quiet"), 1);
  adv(5);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 0);
  CHECK_STR(last_detail(), "waiting for module");
  /* the answer of another module still arms it (as the reference) */
  H();
  act_result = WIFI_ACT_QUEUED;
  str("ELRS", "10.0.0.1");
  str("ELRS", "10.0.0.2");
  SENDF("10.0.0.1", 1, 2, 3, 4);
  wifi_proto_activation_done(&P, IP("10.0.0.2"), true, now_us);
  CHECK(P.first_act_set && P.first_act_ip == IP("10.0.0.2"));
  /* a stale lock is not data flowing: a re-activation of the same module arms it */
  H();
  SENDF("10.0.0.1", 1, 2, 3, 4);
  adv(6);
  str("ELRS", "10.0.0.1");                 /* nothing ran the tick: still bound, but stale */
  CHECK_INT(nactivated, 1);
  CHECK(P.first_act_set);
  tick();
  adv(4.1);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 1);
  /* the answer exactly 3 s after the last frame still counts as streaming (strict release) */
  H();
  act_result = WIFI_ACT_QUEUED;
  str("ELRS", "10.0.0.1");
  SENDF("10.0.0.1", 1, 2, 3, 4);
  now_us += WIFI_SOURCE_TIMEOUT_US;
  wifi_proto_activation_done(&P, IP("10.0.0.1"), true, now_us);
  CHECK(!P.first_act_set);
  now_us += 1;
  wifi_proto_activation_done(&P, IP("10.0.0.1"), true, now_us);
  CHECK(P.first_act_set);
}

static void ActivationBusy_IsNotThrottled(void) {
  H();
  act_result = WIFI_ACT_BUSY;
  str("ELRS", "10.0.0.1");
  str("ELRS", "10.0.0.1");
  CHECK_INT(nactivated, 2);          /* nothing went out, so the next beacon tries again */
  CHECK_INT(P.nact, 0);
  act_result = WIFI_ACT_OK;
  str("ELRS", "10.0.0.1");
  str("ELRS", "10.0.0.1");
  CHECK_INT(nactivated, 3);
  CHECK_INT(P.nact, 1);
  /* no activate hook at all: counts as failed, and is throttled */
  wifi_hooks_t h = HOOKS;
  h.activate = NULL;
  wifi_proto_init(&P, &h);
  str("ELRS", "10.0.0.1");
  CHECK_INT(P.nact, 1);
  CHECK(!P.first_act_set);
}

static void FirewallHint_NotShown_ForPlaceholderModule(void) {
  H();
  str("VELOCIDRONE", "192.168.2.138");
  for (int i = 0; i < 10; i++) send_placeholder("192.168.2.138");
  adv(5);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 0);
  CHECK_STR(last_detail(), "module found, but it is sending no stick data");
  CHECK(P.nostick_ip == IP("192.168.2.138"));
  CHECK_INT(P.dropped_filter, 10);
}

static void FirewallHint_AgainAfterNextLockLoss(void) {
  H();
  str("VELOCIDRONE", "10.0.0.1");
  adv(4.5);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 1);
  CHECK(P.first_act_ip == IP("10.0.0.1"));
  SENDF("10.0.0.1", 1, 2, 3, 4);     /* data: the hint state is reset */
  adv(3.5);
  tick();                            /* went quiet */
  CHECK(!P.bound);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 1);     /* no new activation yet: no timer */
  str("VELOCIDRONE", "10.0.0.1");
  CHECK_INT(nactivated, 2);
  adv(4.1);
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 2);
}

/* ---------------------------------------------------------------- N9 throttle table (LRU) */
static void ThrottleTable_EvictsLeastRecentlyUsed(void) {
  H();
  char ip[32];
  for (int i = 1; i <= WIFI_TABLE + 1; i++) {
    snprintf(ip, sizeof ip, "10.0.0.%d", i);
    str("ELRS", ip);
    adv(0.1);
  }
  CHECK_INT(nactivated, WIFI_TABLE + 1);
  CHECK_INT(P.nact, WIFI_TABLE);
  str("ELRS", "10.0.0.9");           /* the newest is still throttled */
  CHECK_INT(nactivated, WIFI_TABLE + 1);
  str("ELRS", "10.0.0.1");           /* the oldest was evicted by the 9th: posted again (documented) */
  CHECK_INT(nactivated, WIFI_TABLE + 2);
  str("ELRS", "10.0.0.5");           /* still in the table */
  CHECK_INT(nactivated, WIFI_TABLE + 2);
  /* expired entries are reused first: after 5 s everything posts again exactly once */
  adv(5);
  for (int i = 1; i <= WIFI_TABLE; i++) {
    snprintf(ip, sizeof ip, "10.0.0.%d", i);
    str("ELRS", ip);
    str("ELRS", ip);
  }
  CHECK_INT(nactivated, WIFI_TABLE + 2 + WIFI_TABLE);
}

/* ---------------------------------------------------------------- N10 count change */
static void CountChange_FrameHookGetsNewCount(void) {
  H();
  send_same(DEF_IP, 16, 1000);
  CHECK_INT(applied[0].n, 16);
  CHECK_INT(P.nch, 16);
  SENDF(DEF_IP, 1, 2, 3, 4);
  CHECK_INT(napplied, 2);
  CHECK_INT(applied[1].n, 4);
  CHECK_INT(P.nch, 4);
  CHECK(FRAME_IS(1, 1, 2, 3, 4));
  CHECK_INT(count_state(WIFI_ST_STREAMING), 1);
}

/* ---------------------------------------------------------------- N11 exact boundaries */
static void Boundaries_AreStrict(void) {
  H();
  SENDF("192.168.1.10", 1, 2, 3, 4);
  adv(3.0);
  tick();
  CHECK(P.bound);                    /* lock released only when now - last > 3 s */
  str("ELRS", "192.168.1.10");       /* "streaming" needs now - last < 3 s */
  CHECK_INT(nactivated, 1);
  now_us += 1;
  tick();
  CHECK(!P.bound);
  /* throttle: exactly 5 s after the last POST is allowed */
  H();
  str("ELRS", "10.0.0.1");
  now_us += WIFI_THROTTLE_US - 1;
  str("ELRS", "10.0.0.1");
  CHECK_INT(nactivated, 1);
  now_us += 1;
  str("ELRS", "10.0.0.1");
  CHECK_INT(nactivated, 2);
  /* the hint needs strictly more than 4 s */
  H();
  str("ELRS", "10.0.0.1");
  now_us += WIFI_FW_HINT_US;
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 0);
  now_us += 1;
  tick();
  CHECK_INT(count_prefix("!! Activated OK"), 1);
  /* a second source exactly 3 s after the last frame is still ignored */
  H();
  SENDF("10.0.0.1", 1, 2, 3, 4);
  adv(3.0);
  SENDF("10.0.0.2", 1, 2, 3, 4);
  CHECK(source() == IP("10.0.0.1"));
  now_us += 1;
  SENDF("10.0.0.2", 1, 2, 3, 4);
  CHECK(source() == IP("10.0.0.2"));
}

/* ---------------------------------------------------------------- N12 filter before the placeholder check */
static void ExplicitTarget_PlaceholdersFromOtherIp_AreSilent(void) {
  H();
  P.target = IP("10.0.0.9");
  for (int i = 0; i < 20; i++) send_placeholder("10.0.0.7");
  CHECK_INT(nlogs, 0);
  CHECK_INT(nstates, 0);
  CHECK_INT(P.dropped_filter, 0);
  CHECK_INT(P.dropped_other, 20);
  CHECK_INT(nactivated, 0);
}

/* ---------------------------------------------------------------- more edge cases */
static void Frames_LengthAndCountEdges(void) {
  H();
  uint8_t b[64];
  for (int count = 0; count <= 255; count++) {       /* only 4..16 with enough bytes are frames */
    memset(b, 0, sizeof b);
    b[0] = 1;
    b[1] = (uint8_t)count;
    size_t need = 2 + 2 * (size_t)count;
    int before = napplied;
    pkt(b, need <= sizeof b ? need : sizeof b, DEF_IP);
    CHECK_INT(napplied - before, count >= 4 && count <= 16 ? 1 : 0);
    if (count >= 4 && count <= 16) {
      before = napplied;
      pkt(b, need - 1, DEF_IP);                       /* one byte short */
      CHECK_INT(napplied - before, 0);
    }
  }
  /* trailing bytes are ignored */
  size_t n = FRAME(b, 1, 2, 3, 4);
  b[n] = 0xFF; b[n + 1] = 0xFF; b[n + 2] = 0x7F;
  pkt(b, n + 3, DEF_IP);
  CHECK(FRAME_IS(napplied - 1, 1, 2, 3, 4));
  /* exactly 3 bytes: a header that promises more */
  const uint8_t three[] = { 1, 4, 0 };
  int before = napplied;
  pkt(three, 3, DEF_IP);
  CHECK_INT(napplied, before);
  CHECK(P.malformed > 0);
  CHECK_INT(nactivated, 0);
}

static void Frames_AllOutOfRange_AreDroppedAndWarn(void) {
  H();
  send_same(DEF_IP, 8, 32768);
  CHECK_INT(napplied, 0);
  CHECK_INT(count_logs("no stick data"), 1);
  CHECK_INT(P.dropped_filter, 1);
  /* one placeholder anywhere drops the whole frame */
  SENDF(DEF_IP, 1, 2, 3, PH);
  CHECK_INT(napplied, 0);
  /* 32767 is in range; most channels over it but not all: applied, clamped */
  SENDF(DEF_IP, 32767, 40000, 50000, 60000);
  CHECK_INT(napplied, 1);
  CHECK(FRAME_IS(0, 32767, 32767, 32767, 32767));
}

static void Placeholder_WhileLockedToOtherFreshSource_IsSilent(void) {
  H();
  SENDF("10.0.0.1", 1, 2, 3, 4);
  int nl = nlogs, ns = nstates;
  adv(1);
  for (int i = 0; i < 5; i++) send_placeholder("10.0.0.2");
  CHECK_INT(nlogs, nl);
  CHECK_INT(nstates, ns);
  CHECK(source() == IP("10.0.0.1"));
}

static void Beacon_FromSecondModule_WhileLockIsStaleButNotReleased(void) {
  H();
  SENDF("10.0.0.1", 1, 2, 3, 4);
  adv(10);                            /* stale, but nothing ran the tick yet */
  str("ELRS", "10.0.0.2");
  CHECK_INT(nactivated, 0);           /* reference: lockedOther has no staleness check */
  tick();
  str("ELRS", "10.0.0.2");
  CHECK_INT(nactivated, 1);
}

static void SecondSources_WarnTableNeverSpams(void) {
  H();
  SENDF("10.0.0.1", 1, 2, 3, 4);
  char ip[32];
  for (int round = 0; round < 3; round++)
    for (int i = 0; i < 20; i++) {
      snprintf(ip, sizeof ip, "10.0.1.%d", i + 1);
      SENDF(ip, 9, 9, 9, 9);
    }
  CHECK_INT(count_logs("Ignoring 2nd source"), WIFI_TABLE);
  CHECK_INT(napplied, 1);
  /* placeholder sources while unlocked: at most one warning per source, table-limited */
  H();
  for (int round = 0; round < 3; round++)
    for (int i = 0; i < 20; i++) {
      snprintf(ip, sizeof ip, "10.0.2.%d", i + 1);
      send_placeholder(ip);
    }
  CHECK_INT(count_logs("no stick data"), WIFI_TABLE);
}

static void MultipleModules_LockToFirst_ThenHandover(void) {
  H();
  str("ELRS", "10.0.0.2");
  str("ELRS", "10.0.0.3");
  CHECK_INT(nactivated, 2);
  for (int i = 0; i < 100; i++) {     /* both stream, interleaved, 250 Hz each */
    SENDF("10.0.0.3", 3, 3, 3, 3);
    SENDF("10.0.0.2", 2, 2, 2, 2);
    tick();
    adv(0.004);
  }
  CHECK(source() == IP("10.0.0.3"));
  CHECK_INT(napplied, 100);
  CHECK_INT(count_logs("Ignoring 2nd source 10.0.0.2 (locked to 10.0.0.3)"), 1);
  CHECK_INT(count_state(WIFI_ST_STREAMING), 1);
  /* .3 stops: once its last frame is over 3 s old, the next .2 frame takes over (silently, as the
   * reference: frames are drained before the tick, so "went quiet" never fires here) */
  int64_t t0 = now_us - 4000;
  for (int i = 0; i < 2000 && source() != IP("10.0.0.2"); i++) { SENDF("10.0.0.2", 2, 2, 2, 2); tick(); adv(0.004); }
  CHECK(source() == IP("10.0.0.2"));
  CHECK(now_us - t0 > 3000000 && now_us - t0 < 3020000);
  CHECK_INT(count_logs("went quiet"), 0);
  CHECK_INT(count_logs("Joystick source locked to 10.0.0.2"), 1);
  CHECK_INT(count_state(WIFI_ST_STREAMING), 2);
  CHECK(has_state(WIFI_ST_STREAMING, "10.0.0.2"));
  CHECK_INT(applied[napplied - 1].src, IP("10.0.0.2"));
}

static void Reset_KeepsTargetAndCounters_ClearsThrottle(void) {
  H();
  P.target = IP("10.0.0.9");
  tick();
  CHECK_INT(nactivated, 1);
  SENDF("10.0.0.9", 1, 2, 3, 4);
  str("ELRS", "10.0.0.7");
  uint32_t rx = P.rx;
  wifi_proto_reset(&P);
  CHECK(P.target == IP("10.0.0.9"));
  CHECK(!P.bound);
  CHECK_INT(P.rx, rx);
  CHECK_INT(P.nch, 0);
  tick();                              /* the throttle is forgotten: posts at once */
  CHECK_INT(nactivated, 2);
}

static void States_AndLifecycle(void) {
  H();
  CHECK_INT(P.state, WIFI_ST_STOPPED);
  wifi_proto_set_state(&P, WIFI_ST_ERROR, "UDP port 11000 in use");
  CHECK_INT(last_state(), WIFI_ST_ERROR);
  CHECK_STR(P.detail, "UDP port 11000 in use");
  char longd[200];
  memset(longd, 'd', sizeof longd - 1);
  longd[sizeof longd - 1] = 0;
  wifi_proto_set_state(&P, WIFI_ST_SEARCHING, longd);
  CHECK_INT(strlen(P.detail), sizeof P.detail - 1);
  wifi_proto_set_state(&P, WIFI_ST_STOPPED, NULL);
  CHECK_STR(P.detail, "");
  CHECK_STR(wifi_state_name(WIFI_ST_STOPPED), "stopped");
  CHECK_STR(wifi_state_name(WIFI_ST_SEARCHING), "searching");
  CHECK_STR(wifi_state_name(WIFI_ST_STREAMING), "streaming");
  CHECK_STR(wifi_state_name(WIFI_ST_ERROR), "error");
  CHECK_STR(wifi_kind_name(WIFI_KIND_ELRS), "ELRS");
  CHECK_STR(wifi_kind_name(WIFI_KIND_XF), "Crossfire");
  CHECK_STR(wifi_kind_name(WIFI_KIND_UNKNOWN), "WiFi");
  /* hooks may be NULL */
  wifi_hooks_t none;
  memset(&none, 0, sizeof none);
  wifi_proto_init(&P, &none);
  SENDF(DEF_IP, 1, 2, 3, 4);
  str("ELRS", "10.0.0.1");
  send_placeholder("10.0.0.2");
  adv(10);
  tick();
  wifi_proto_poll_stats(&P, now_us);
  wifi_proto_poll_stats(&P, now_us + 2000000);
  CHECK_INT(P.seq, 1);
  wifi_proto_init(&P, NULL);
  SENDF(DEF_IP, 1, 2, 3, 4);
  CHECK_INT(P.seq, 1);
}

static void Counters_AndLastFrame(void) {
  H();
  SENDF(DEF_IP, 10, 20, 30, 40, 50);
  str("ELRS", "10.0.0.1");
  const uint8_t junk[] = { 9, 99, 1, 2 };
  pkt(junk, sizeof junk, DEF_IP);
  send_placeholder(DEF_IP);
  SENDF("10.0.0.5", 1, 1, 1, 1);
  CHECK_INT(P.rx, 5);
  CHECK_INT(P.beacons, 1);
  CHECK_INT(P.malformed, 1);
  CHECK_INT(P.dropped_filter, 1);
  CHECK_INT(P.dropped_other, 1);
  CHECK_INT(P.seq, 1);
  CHECK_INT(P.nch, 5);
  CHECK_INT(P.raw[4], 50);
  CHECK(P.last_src == IP(DEF_IP));
  wifi_proto_packet(&P, (const uint8_t *)"\x01\x04\x01\x00\x02\x00\x03\x00\x04\x00", 10, IP(DEF_IP), now_us, now_us - 1234);
  CHECK(P.last_arrival_us == now_us - 1234);
}

static void Stats_EdgeCases(void) {
  H();
  wifi_proto_add_arrival(&P, 0);
  wifi_proto_add_arrival(&P, 10000);
  wifi_proto_emit_stats(&P, now_us);
  CHECK(st.hz == 0);                       /* the reference needs more than 2 arrivals */
  CHECK_INT(st.frames, 2);
  for (int i = 0; i < 3; i++) wifi_proto_add_arrival(&P, 5000);   /* zero span */
  wifi_proto_emit_stats(&P, now_us);
  CHECK(st.hz == 0 && st.jit == 0);
  CHECK_INT(st.frames, 3);
  /* the period: nothing on the first poll, then every >= 1 s */
  H();
  wifi_proto_poll_stats(&P, now_us);
  CHECK_INT(st.count, 0);
  wifi_proto_poll_stats(&P, now_us + 999999);
  CHECK_INT(st.count, 0);
  wifi_proto_poll_stats(&P, now_us + 1000000);
  CHECK_INT(st.count, 1);
  wifi_proto_poll_stats(&P, now_us + 1500000);
  CHECK_INT(st.count, 1);
  wifi_proto_poll_stats(&P, now_us + 2000000);
  CHECK_INT(st.count, 2);
  /* only applied frames count; 250 Hz real frames with arrival stamps */
  H();
  for (int i = 0; i < 250; i++) {
    uint8_t b[16];
    size_t n = FRAME(b, 1, 2, 3, 4);
    wifi_proto_packet(&P, b, n, IP(DEF_IP), now_us + 100000, now_us + (int64_t)i * 4000);
    send_placeholder(DEF_IP);
    SENDF("10.9.9.9", 1, 2, 3, 4);
  }
  wifi_proto_emit_stats(&P, now_us);
  CHECK_INT(st.frames, 250);
  CHECK_NEAR(st.hz, 250.0, 0.01);
  CHECK_NEAR(st.jit, 0.0, 0.001);
  CHECK_NEAR(P.last_hz, 250.0, 0.01);
  CHECK_INT(P.last_frames, 250);
}

/* ---------------------------------------------------------------- activation queue */
static void ActQueue_PushTakePutGet(void) {
  wifi_actq_t q;
  wifi_actq_init(&q);
  wifi_actreq_t rq;
  wifi_actres_t rs, out;
  CHECK(!wifi_actq_take(&q, &rq));
  CHECK(!wifi_actq_get(&q, &out));
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.1"), 80, "action=joystick_begin&interval=4000&channels=16"), WIFI_ACT_QUEUED);
  CHECK(wifi_actq_is_pending(&q, IP("10.0.0.1")));
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.1"), 80, "x"), WIFI_ACT_BUSY);      /* dedupe while queued */
  CHECK(wifi_actq_take(&q, &rq));
  CHECK(rq.ip == IP("10.0.0.1") && rq.http_port == 80);
  CHECK_STR(rq.body, "action=joystick_begin&interval=4000&channels=16");
  CHECK(!wifi_actq_stale(&q, rq.session));
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.1"), 80, "x"), WIFI_ACT_BUSY);      /* dedupe while in flight */
  memset(&rs, 0, sizeof rs);
  rs.ip = rq.ip; rs.session = rq.session; rs.ok = true; rs.status = 200;
  snprintf(rs.reason, sizeof rs.reason, "OK");
  wifi_actq_put(&q, &rs);
  CHECK(wifi_actq_is_pending(&q, IP("10.0.0.1")));                          /* until the frame loop takes it */
  CHECK(wifi_actq_get(&q, &out));
  CHECK(out.ok && out.status == 200 && out.ip == IP("10.0.0.1"));
  CHECK_STR(out.reason, "OK");
  CHECK(!wifi_actq_is_pending(&q, IP("10.0.0.1")));
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.1"), 80, "x"), WIFI_ACT_QUEUED);
  /* body too long for a request slot */
  char longb[100];
  memset(longb, 'b', sizeof longb - 1);
  longb[sizeof longb - 1] = 0;
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.2"), 80, longb), WIFI_ACT_BUSY);
  CHECK(!wifi_actq_is_pending(&q, IP("10.0.0.2")));
}

static void ActQueue_FullAndOrder(void) {
  wifi_actq_t q;
  wifi_actq_init(&q);
  char ip[32];
  for (int i = 1; i <= WIFI_ACTQ; i++) {
    snprintf(ip, sizeof ip, "10.0.0.%d", i);
    CHECK_INT(wifi_actq_push(&q, IP(ip), 80, "b"), WIFI_ACT_QUEUED);
  }
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.99"), 80, "b"), WIFI_ACT_BUSY);
  wifi_actreq_t rq;
  for (int i = 1; i <= WIFI_ACTQ; i++) {         /* FIFO */
    snprintf(ip, sizeof ip, "10.0.0.%d", i);
    CHECK(wifi_actq_take(&q, &rq));
    CHECK(rq.ip == IP(ip));
  }
  CHECK(!wifi_actq_take(&q, &rq));
  /* all taken but none answered: still pending, so still full */
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.99"), 80, "b"), WIFI_ACT_BUSY);
  wifi_actres_t rs, out;
  memset(&rs, 0, sizeof rs);
  rs.session = q.session;
  rs.ip = IP("10.0.0.3");
  wifi_actq_put(&q, &rs);
  CHECK(wifi_actq_get(&q, &out));
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.99"), 80, "b"), WIFI_ACT_QUEUED);
  /* the impossible overflow of the result ring unpends what it drops */
  wifi_actq_init(&q);
  for (int i = 1; i <= WIFI_ACTQ; i++) {
    snprintf(ip, sizeof ip, "10.0.1.%d", i);
    wifi_actq_push(&q, IP(ip), 80, "b");
    wifi_actq_take(&q, &rq);
    rs.ip = rq.ip;
    rs.session = rq.session;
    wifi_actq_put(&q, &rs);
  }
  rs.ip = IP("10.0.1.200");
  wifi_actq_put(&q, &rs);
  CHECK(!wifi_actq_is_pending(&q, IP("10.0.1.1")));
  int got = 0;
  while (wifi_actq_get(&q, &out)) got++;
  CHECK_INT(got, WIFI_ACTQ);
}

static void ActQueue_SessionDiscardsStaleResults(void) {
  wifi_actq_t q;
  wifi_actq_init(&q);
  wifi_actreq_t rq;
  wifi_actres_t rs, out;
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.1"), 80, "b"), WIFI_ACT_QUEUED);
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.2"), 80, "b"), WIFI_ACT_QUEUED);
  CHECK(wifi_actq_take(&q, &rq));                       /* .1 in flight on the worker */
  uint32_t old = rq.session;
  wifi_actq_new_session(&q);                            /* stop/start */
  CHECK(wifi_actq_stale(&q, old));                      /* the worker aborts it */
  CHECK(!wifi_actq_take(&q, &rq));                      /* .2 was dropped from the queue */
  CHECK(!wifi_actq_is_pending(&q, IP("10.0.0.1")));
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.1"), 80, "b"), WIFI_ACT_QUEUED);   /* new session may ask again */
  memset(&rs, 0, sizeof rs);
  rs.ip = IP("10.0.0.1"); rs.session = old; rs.ok = true; rs.status = 200;
  wifi_actq_put(&q, &rs);                               /* the old answer arrives late */
  CHECK(!wifi_actq_get(&q, &out));                      /* and is discarded */
  CHECK(wifi_actq_is_pending(&q, IP("10.0.0.1")));      /* the new request still counts */
  CHECK(wifi_actq_take(&q, &rq));
  CHECK(!wifi_actq_stale(&q, rq.session));
  rs.session = rq.session; rs.ok = false; rs.status = 0;
  snprintf(rs.err, sizeof rs.err, "connection refused");
  wifi_actq_put(&q, &rs);
  /* a result already in the ring when the session changes is dropped too */
  wifi_actq_new_session(&q);
  CHECK(!wifi_actq_get(&q, &out));
  /* a result that slipped into the ring with an old session is skipped by get */
  CHECK_INT(wifi_actq_push(&q, IP("10.0.0.3"), 80, "b"), WIFI_ACT_QUEUED);
  wifi_actq_take(&q, &rq);
  q.res[0] = rs;                                        /* old session, forced in */
  q.res[0].session = old;
  q.rs_head = 0; q.rs_n = 1;
  rs.ip = rq.ip; rs.session = rq.session; rs.ok = true;
  wifi_actq_put(&q, &rs);
  CHECK(wifi_actq_get(&q, &out));
  CHECK(out.ip == IP("10.0.0.3") && out.ok);
  CHECK(!wifi_actq_get(&q, &out));
}

int main(void) {
  /* BeaconTests */
  ElrsBeacon_TriggersActivation();
  CrossfireBeacon_TriggersActivation_AndIsIdentified();
  BeaconWithTrailingBytes_IsStillABeacon();
  RepeatedBeacon_IsThrottled_WithinFiveSeconds();
  Beacon_ReactivatesAfterThrottleExpiry_WhenStillNoData();
  Beacon_DoesNotReactivate_WhileDataIsFlowing();
  Beacon_Reactivates_AfterStreamWentStale();
  Beacon_FromSecondModule_IgnoredWhileLockedToFirst();
  ForcedModule_BeaconFromOtherIp_IsIgnored();
  FailedActivation_DoesNotStartFirewallHintTimer();
  /* ChannelParsingTests */
  Decodes8Channels_LittleEndian();
  Accepts16Channels_CrossfireAlwaysSends16();
  AcceptsMinimumOf4Channels();
  RejectsFewerThan4Channels();
  RejectsMoreThan16Channels();
  RejectsTruncatedFrame();
  RejectsTooShortPackets_WithoutCrashing();
  ClampsOverrangeValues_To15BitMax();
  AllPlaceholderFrame_IsNotAJoystickSource();
  AllPlaceholderFrame_WarnsOnce_NotPerFrame();
  RealStickData_StillLocksAndStreams_AfterPlaceholders();
  OvershootFrame_IsClampedAndApplied();
  CorruptFrame_MidStream_IsDroppedSilently();
  PlaceholderFlood_ReleasesLock_ThenWarns();
  FrameTypeByte_IsNotValidated();
  ChannelsUpdatedEvent_CarriesTheSameValues();
  ZeroValues_PassThrough();
  /* SourceLockTests */
  FirstChannelFrame_LocksSource_AndFiresStreaming();
  SecondSource_IsIgnoredWhileLockIsFresh();
  SecondSource_WarnsOnlyOnce();
  StaleLock_ReleasesToNewSource();
  ForcedModule_ChannelsFromOtherIp_AreIgnored();
  SessionReset_AllowsRelock_RegressionForStopStartBug();
  Tick_ReleasesStaleLock_AndReturnsToSearching();
  Tick_DoesNotReleaseFreshLock();
  FirewallHint_ShownOnce_WhenActivatedButNoData();
  FirewallHint_NotShown_WhenDataArrived();
  SetTarget_TrimsIp_AndUnlocksCurrentSource();
  SetTarget_NullOrWhitespace_ClearsTarget();
  /* StatsTests */
  SteadyStream_Reports100Hz_ZeroJitter();
  AlternatingGaps_ReportCorrectJitter();
  StatsReset_AfterEachEmit();
  TooFewSamples_ReportZeroRate_WithoutCrashing();
  RealPackets_AreCountedInStats();
  /* MiscTests (the firewall-rule tests are Windows-only) */
  StartsWith_Cases();
  OutputOverride_ReceivesEveryFrame();
  LogNeverThrows_OnGarbageInput();
  /* MainViewModelTests: the bars' percent (the rest is in test_wifi_live) */
  TickBars_Percent();
  /* new */
  RawToStick_Vectors();
  ElrsBeacon_Parse();
  Beacons_KindAndName();
  Post_Bytes();
  HttpStatus_Parse();
  ParseIp_Cases();
  TickStarvation_IgnoredTrafficCannotBlockTheTick();
  SetTarget_PostsAtOnce_AndReportsState();
  SetTarget_Invalid_KeepsTarget();
  SetTarget_SameAsBound_Relocks();
  ActivationAsync_HintTimerStartsAtDone();
  ActivationAsync_FailedDone_NoHint();
  ActivationDone_WhileStreamingFromIt_DoesNotArmHint();
  ActivationBusy_IsNotThrottled();
  FirewallHint_NotShown_ForPlaceholderModule();
  FirewallHint_AgainAfterNextLockLoss();
  ThrottleTable_EvictsLeastRecentlyUsed();
  CountChange_FrameHookGetsNewCount();
  Boundaries_AreStrict();
  ExplicitTarget_PlaceholdersFromOtherIp_AreSilent();
  Frames_LengthAndCountEdges();
  Frames_AllOutOfRange_AreDroppedAndWarn();
  Placeholder_WhileLockedToOtherFreshSource_IsSilent();
  Beacon_FromSecondModule_WhileLockIsStaleButNotReleased();
  SecondSources_WarnTableNeverSpams();
  MultipleModules_LockToFirst_ThenHandover();
  Reset_KeepsTargetAndCounters_ClearsThrottle();
  States_AndLifecycle();
  Counters_AndLastFrame();
  Stats_EdgeCases();
  ActQueue_PushTakePutGet();
  ActQueue_FullAndOrder();
  ActQueue_SessionDiscardsStaleResults();
  T_DONE();
}
