/* The web server's pure helpers: path cleaning, percent-decoding, JSON escaping and scanning,
 * Host / Origin checks, ETags, MIME types, device and temp names, the SD dirty flag. */
#include "t.h"
#include "../src/web_internal.h"

#include <math.h>

static int clean(const char *in, char *out) { return web_clean_path(in, out, 1100); }

static void test_url_decode(void) {
  char s[64];
  strcpy(s, "plain"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "plain");
  strcpy(s, "a%20b"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "a b");
  strcpy(s, "%2F%2f"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "//");
  strcpy(s, "a+b"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "a+b");          /* '+' stays '+' */
  strcpy(s, "%C3%A9"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "\xC3\xA9");
  strcpy(s, "%41%42%43"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "ABC");
  strcpy(s, ""); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "");
  strcpy(s, "%"); CHECK_INT(web_url_decode(s), -1);
  strcpy(s, "%2"); CHECK_INT(web_url_decode(s), -1);
  strcpy(s, "a%zz"); CHECK_INT(web_url_decode(s), -1);
  strcpy(s, "%g0"); CHECK_INT(web_url_decode(s), -1);
  strcpy(s, "%0g"); CHECK_INT(web_url_decode(s), -1);
  strcpy(s, "%00"); CHECK_INT(web_url_decode(s), -1);                               /* NUL */
  strcpy(s, "ok%00ok"); CHECK_INT(web_url_decode(s), -1);
  strcpy(s, "%%41"); CHECK_INT(web_url_decode(s), -1);
  strcpy(s, "%2541"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "%41");        /* decoded once only */
  strcpy(s, "%2B%2b"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "++");
  strcpy(s, "%FF%fe"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "\xFF\xFE");   /* bytes; UTF-8 is checked later */
  strcpy(s, "end%41"); CHECK_INT(web_url_decode(s), 0); CHECK_STR(s, "endA");
  CHECK_INT(web_url_decode(NULL), -1);
}

static void test_clean_path(void) {
  char o[1100], big[4096];
  CHECK_INT(clean("", o), 0); CHECK_STR(o, "");
  CHECK_INT(clean("/", o), 0); CHECK_STR(o, "");
  CHECK_INT(clean("a", o), 0); CHECK_STR(o, "a");
  CHECK_INT(clean("/SCRIPTS/TOOLS/c1.txt", o), 0); CHECK_STR(o, "SCRIPTS/TOOLS/c1.txt");
  CHECK_INT(clean("a/b/", o), 0); CHECK_STR(o, "a/b");
  CHECK_INT(clean("My%20Track.txt", o), 0); CHECK_STR(o, "My Track.txt");
  CHECK_INT(clean("%20lead", o), 0); CHECK_STR(o, " lead");                         /* leading space is fine */
  CHECK_INT(clean(".hidden", o), 0); CHECK_STR(o, ".hidden");
  CHECK_INT(clean("a+b", o), 0); CHECK_STR(o, "a+b");
  CHECK_INT(clean("caf%C3%A9/%E2%82%AC/%F0%9F%9A%81", o), 0); CHECK_STR(o, "caf\xC3\xA9/\xE2\x82\xAC/\xF0\x9F\x9A\x81");
  /* empty components, dots, traversal */
  CHECK_INT(clean("a//b", o), -1);
  CHECK_INT(clean("a/b//", o), -1);                                                  /* only one trailing slash */
  CHECK_INT(clean("//", o), -1);
  CHECK_INT(clean("a/.", o), -1);
  CHECK_INT(clean("./a", o), -1);
  CHECK_INT(clean("a/..", o), -1);
  CHECK_INT(clean("%2e%2e%2f", o), -1);
  CHECK_INT(clean("x/%2E", o), -1);
  CHECK_INT(clean("..a", o), 0); CHECK_STR(o, "..a");                               /* dots inside a name are fine */
  CHECK_INT(clean("a..b/c.d", o), 0); CHECK_STR(o, "a..b/c.d");
  CHECK_INT(clean("St%C3%ADckTime/C1.TXT", o), 0); CHECK_STR(o, "St\xC3\xAD" "ckTime/C1.TXT");  /* case kept */
  CHECK_INT(clean("//etc/passwd", o), -1);                                           /* absolute after /api/sd/ */
  CHECK_INT(clean(".", o), -1);
  CHECK_INT(clean("..", o), -1);
  CHECK_INT(clean("a/../b", o), -1);
  CHECK_INT(clean("a/./b", o), -1);
  CHECK_INT(clean("%2e%2e", o), -1);
  CHECK_INT(clean("%2E%2E/x", o), -1);
  CHECK_INT(clean("%2e", o), -1);
  CHECK_INT(clean(".%2e", o), -1);
  CHECK_INT(clean("...", o), -1);                                                    /* ends in a dot */
  CHECK_INT(clean("a.", o), -1);
  CHECK_INT(clean("a%20", o), -1);                                                   /* ends in a space */
  CHECK_INT(clean("c1.txt.", o), -1);
  /* separators and FAT-invalid characters */
  CHECK_INT(clean("a%2fb", o), -1);
  CHECK_INT(clean("a%2F", o), -1);
  CHECK_INT(clean("a\\b", o), -1);
  CHECK_INT(clean("a%5cb", o), -1);
  CHECK_INT(clean("..%5c..%5cx", o), -1);
  CHECK_INT(clean("C:", o), -1);
  CHECK_INT(clean("C%3A/x", o), -1);
  CHECK_INT(clean("c:%5cwindows", o), -1);
  const char *bad[] = { "%22", "%2A", "*", "%3A", ":", "%3C", "<", "%3E", ">", "%3F", "?", "%7C", "|" };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char in[32];
    snprintf(in, sizeof in, "x%sy", bad[i]);
    CHECK_INT(clean(in, o), -1);
  }
  /* control characters, NUL, bad escapes */
  CHECK_INT(clean("a%00b", o), -1);
  CHECK_INT(clean("a%01b", o), -1);
  CHECK_INT(clean("a%1Fb", o), -1);
  CHECK_INT(clean("a%7Fb", o), -1);
  CHECK_INT(clean("a%0Ab", o), -1);
  CHECK_INT(clean("a%09b", o), -1);
  CHECK_INT(clean("a%", o), -1);
  CHECK_INT(clean("a%4", o), -1);
  CHECK_INT(clean("a%xyb", o), -1);
  /* invalid UTF-8 */
  CHECK_INT(clean("%FF", o), -1);
  CHECK_INT(clean("%C0%AF", o), -1);                                                 /* overlong '/' */
  CHECK_INT(clean("%E0%80%AF", o), -1);
  CHECK_INT(clean("%ED%A0%80", o), -1);                                              /* surrogate */
  CHECK_INT(clean("%F4%90%80%80", o), -1);                                           /* > U+10FFFF */
  CHECK_INT(clean("%E2%82", o), -1);                                                 /* truncated */
  CHECK_INT(clean("%80", o), -1);
  /* lengths: 255 bytes per component, 1024 in all */
  memset(big, 'a', 255); big[255] = 0;
  CHECK_INT(clean(big, o), 0); CHECK_INT(strlen(o), 255);
  memset(big, 'a', 256); big[256] = 0;
  CHECK_INT(clean(big, o), -1);
  /* 255 bytes as 85 three-byte characters is fine, 86 is not */
  big[0] = 0;
  for (int i = 0; i < 85; i++) strcat(big, "%E2%82%AC");
  CHECK_INT(clean(big, o), 0);
  strcat(big, "%E2%82%AC");
  CHECK_INT(clean(big, o), -1);
  /* 4 x 255 + 3 slashes = 1023; one more byte is fine, two are not */
  {
    char comp[256];
    memset(comp, 'b', 255); comp[255] = 0;
    snprintf(big, sizeof big, "%s/%s/%s/%s", comp, comp, comp, comp);
    CHECK_INT(clean(big, o), 0); CHECK_INT(strlen(o), 1023);
    strcat(big, "/");
    CHECK_INT(clean(big, o), 0);                                                     /* trailing slash only */
    big[strlen(big) - 1] = 0;
    strcat(big, "/c");
    CHECK_INT(clean(big, o), -1);                                                    /* 1025 */
  }
  /* output buffer too small, NULLs */
  CHECK_INT(web_clean_path("abcdef", o, 4), -1);
  CHECK_INT(web_clean_path("abc", o, 4), 0); CHECK_STR(o, "abc");
  CHECK_INT(web_clean_path(NULL, o, 10), -1);
  CHECK_INT(web_clean_path("a", NULL, 10), -1);
  CHECK_INT(web_clean_path("a", o, 0), -1);
  /* overlong raw input is refused before decoding */
  memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
  CHECK_INT(clean(big, o), -1);
}

static void test_json_escape(void) {
  char o[128];
  CHECK_INT(web_json_escape("abc", o, sizeof o), 3); CHECK_STR(o, "abc");
  CHECK_INT(web_json_escape("", o, sizeof o), 0); CHECK_STR(o, "");
  CHECK_INT(web_json_escape(NULL, o, sizeof o), 0); CHECK_STR(o, "");
  web_json_escape("a\"b\\c", o, sizeof o); CHECK_STR(o, "a\\\"b\\\\c");
  web_json_escape("\n\r\t\b\f", o, sizeof o); CHECK_STR(o, "\\n\\r\\t\\b\\f");
  web_json_escape("\x01\x1f\x7f", o, sizeof o); CHECK_STR(o, "\\u0001\\u001f\\u007f");
  web_json_escape("<b>&", o, sizeof o); CHECK_STR(o, "\\u003cb\\u003e\\u0026");
  web_json_escape("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x9A\x81", o, sizeof o); CHECK_STR(o, "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x9A\x81");
  web_json_escape("a\xFF" "b", o, sizeof o); CHECK_STR(o, "a\\ufffdb");             /* invalid UTF-8 */
  web_json_escape("\xC3", o, sizeof o); CHECK_STR(o, "\\ufffd");                    /* truncated sequence */
  web_json_escape("\xED\xA0\x80", o, sizeof o); CHECK_STR(o, "\\ufffd\\ufffd\\ufffd");
  web_json_escape("x\xE2\x80\xA8y\xE2\x80\xA9", o, sizeof o); CHECK_STR(o, "x\\u2028y\\u2029");
  /* the length is always the full one; the output is cut between escapes and NUL-terminated */
  CHECK_INT(web_json_escape("ab\"cd", o, 4), 6); CHECK_STR(o, "ab");
  CHECK_INT(web_json_escape("ab\"cd", o, 5), 6); CHECK_STR(o, "ab\\\"");
  CHECK_INT(web_json_escape("\x01", o, 6), 6); CHECK_STR(o, "");
  CHECK_INT(web_json_escape("\x01", o, 7), 6); CHECK_STR(o, "\\u0001");
  CHECK_INT(web_json_escape("\xE2\x82\xAC", o, 3), 3); CHECK_STR(o, "");           /* never half a character */
  CHECK_INT(web_json_escape("abc", NULL, 0), 3);
  o[0] = 'x';
  CHECK_INT(web_json_escape("abc", o, 1), 3); CHECK_STR(o, "");
}

static void test_host_origin(void) {
  web_add_hostname("sticktime");
  CHECK(web_host_ok("192.168.1.23"));
  CHECK(web_host_ok("192.168.1.23:80"));
  CHECK(web_host_ok("10.0.0.1:65535"));
  CHECK(web_host_ok("127.0.0.1."));
  CHECK(web_host_ok("localhost"));
  CHECK(web_host_ok("LOCALHOST:8080"));
  CHECK(web_host_ok("sticktime"));
  CHECK(web_host_ok("sticktime.local"));
  CHECK(web_host_ok("StickTime.LOCAL:80"));
  CHECK(web_host_ok("sticktime.local."));
  CHECK(web_host_ok("[::1]"));
  CHECK(web_host_ok("[fe80::1]:8080"));
  CHECK(web_host_ok("[::ffff:192.168.1.2]"));
  CHECK(!web_host_ok(""));
  CHECK(!web_host_ok(NULL));
  CHECK(!web_host_ok("evil.com"));
  CHECK(!web_host_ok("sticktime.local.evil.com"));
  CHECK(!web_host_ok("evil.sticktime.local"));
  CHECK(!web_host_ok("sticktime.locals"));
  CHECK(!web_host_ok("sticktimex"));
  CHECK(!web_host_ok("localhost.evil.com"));
  CHECK(!web_host_ok("user@sticktime.local"));
  CHECK(!web_host_ok("sticktime.local:"));
  CHECK(!web_host_ok("sticktime.local:99999"));
  CHECK(!web_host_ok("sticktime.local:80x"));
  CHECK(!web_host_ok("1.2.3"));
  CHECK(!web_host_ok("1.2.3.4.5"));
  CHECK(!web_host_ok("256.1.1.1"));
  CHECK(!web_host_ok("1.2.3.4x"));
  CHECK(!web_host_ok("1..2.3"));
  CHECK(!web_host_ok("[::1"));
  CHECK(!web_host_ok("[evil.com]"));
  CHECK(!web_host_ok("[]"));
  CHECK(!web_host_ok("[::1]x"));
  CHECK(!web_host_ok(":80"));
  CHECK(!web_host_ok("."));
  CHECK(web_host_ok("[FE80::ABCD]"));
  CHECK(!web_host_ok("[::1]:"));
  CHECK(!web_host_ok("[::1]:65536"));
  CHECK(!web_host_ok("[1.2.3.4]"));                                                  /* v4 in brackets is no v6 */
  CHECK(!web_host_ok("sticktime local"));
  CHECK(!web_host_ok("sticktime.local:80:80"));
  CHECK(web_host_ok("0.0.0.0"));
  CHECK(web_host_ok("255.255.255.255:1"));
  CHECK(!web_host_ok("01234.1.1.1"));
  /* a second name (after a hostname change) is accepted too */
  CHECK(!web_host_ok("racer.local"));
  web_add_hostname("racer");
  CHECK(web_host_ok("racer.local"));
  CHECK(web_host_ok("sticktime.local"));
  char hn[64];
  web_hostname(hn, sizeof hn);
  CHECK_STR(hn, "sticktime");

  CHECK(web_origin_ok("http://sticktime.local", "sticktime.local"));
  CHECK(web_origin_ok("http://sticktime.local", "sticktime.local:80"));
  CHECK(web_origin_ok("http://sticktime.local:80", "sticktime.local"));
  CHECK(web_origin_ok("http://STICKTIME.local", "sticktime.LOCAL"));
  CHECK(web_origin_ok("http://127.0.0.1:8080", "127.0.0.1:8080"));
  CHECK(web_origin_ok("http://[::1]:8080", "[::1]:8080"));
  CHECK(web_origin_ok("https://sticktime.local", "sticktime.local"));
  CHECK(!web_origin_ok("null", "sticktime.local"));
  CHECK(!web_origin_ok("http://evil.com", "sticktime.local"));
  CHECK(!web_origin_ok("http://sticktime.local:8080", "sticktime.local"));
  CHECK(!web_origin_ok("http://127.0.0.1:8081", "127.0.0.1:8080"));
  CHECK(!web_origin_ok("ftp://sticktime.local", "sticktime.local"));
  CHECK(!web_origin_ok("http://", "sticktime.local"));
  CHECK(!web_origin_ok("http://sticktime.local/path", "sticktime.local"));
  CHECK(!web_origin_ok("http://a@sticktime.local", "sticktime.local"));
  CHECK(!web_origin_ok("sticktime.local", "sticktime.local"));
  CHECK(!web_origin_ok("", "sticktime.local"));
  CHECK(!web_origin_ok(NULL, "x"));
  CHECK(!web_origin_ok("http://x", NULL));
  CHECK(web_origin_ok("http://sticktime.local/", "sticktime.local"));                /* a trailing slash */
  CHECK(web_origin_ok("HTTP://sticktime.local", "sticktime.local"));                 /* the scheme in any case */
  CHECK(web_origin_ok("https://sticktime.local:443", "sticktime.local"));
  CHECK(!web_origin_ok("https://sticktime.local:80", "sticktime.local:443"));
  CHECK(!web_origin_ok("http://sticktime.local:", "sticktime.local"));
  CHECK(!web_origin_ok("http://sticktime.local.evil.com", "sticktime.local"));
}

static void test_etag_mime(void) {
  CHECK(web_etag_match("\"1a-2b\"", "\"1a-2b\""));
  CHECK(web_etag_match("W/\"1a-2b\"", "\"1a-2b\""));
  CHECK(web_etag_match("\"x\", \"1a-2b\"", "\"1a-2b\""));
  CHECK(web_etag_match("\"x\",W/\"1a-2b\" ", "\"1a-2b\""));
  CHECK(web_etag_match("*", "\"1a-2b\""));
  CHECK(web_etag_match(" * ", "\"1a-2b\""));
  CHECK(!web_etag_match("\"1a-2c\"", "\"1a-2b\""));
  CHECK(!web_etag_match("1a-2b", "\"1a-2b\""));
  CHECK(!web_etag_match("\"1a-2b", "\"1a-2b\""));
  CHECK(!web_etag_match("", "\"1a-2b\""));
  CHECK(!web_etag_match("**", "\"1a-2b\""));
  CHECK(!web_etag_match(NULL, "\"x\""));
  CHECK(web_etag_match("\"x\"", "W/\"x\""));                                       /* weak comparison both ways */
  CHECK(web_etag_match("\"a\",*", "\"x\""));
  CHECK(!web_etag_match("*x", "\"x\""));
  CHECK(!web_etag_match("\"x\"y", "\"xy\""));
  CHECK(!web_etag_match("W/", "\"x\""));
  CHECK(!web_etag_match("\"1a-2b\"", NULL));

  CHECK_STR(web_mime("web/index.html"), "text/html; charset=utf-8");
  CHECK_STR(web_mime("a/app.JS"), "text/javascript; charset=utf-8");
  CHECK_STR(web_mime("s.css"), "text/css; charset=utf-8");
  CHECK_STR(web_mime("x.json"), "application/json; charset=utf-8");
  CHECK_STR(web_mime("i.png"), "image/png");
  CHECK_STR(web_mime("i.svg"), "image/svg+xml");
  CHECK_STR(web_mime("favicon.ico"), "image/x-icon");
  CHECK_STR(web_mime("c1.txt"), "text/plain; charset=utf-8");
  CHECK_STR(web_mime("f.woff2"), "font/woff2");
  CHECK_STR(web_mime("s.wav"), "audio/wav");
  CHECK_STR(web_mime("s.mp3"), "audio/mpeg");
  CHECK_STR(web_mime("s.ogg"), "audio/ogg");
  CHECK_STR(web_mime("noext"), "application/octet-stream");
  CHECK_STR(web_mime("dir.d/noext"), "application/octet-stream");
  CHECK_STR(web_mime("x.unknown"), "application/octet-stream");
}

static void test_json_scan(void) {
  const char *ok[] = { "{}", "[]", "0", "-1.5e3", "\"a\\u00e9\\n\"", "true", "null", " { \"a\" : [1, 2, {\"b\": false}] } ",
                       "{\"networks\":[{\"ssid\":\"Home\",\"signal\":72}],\"at\":1760000000000}" };
  for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) CHECK(web_json_valid(ok[i], strlen(ok[i])));
  const char *bad[] = { "", " ", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "01", "1.", ".5", "-", "\"abc", "\"a\nb\"", "\"\\x\"",
                        "\"\\u12\"", "tru", "nul", "{} {}", "[1 2]", "{a:1}", "+1", "1e", "{\"a\":1,}" };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(!web_json_valid(bad[i], strlen(bad[i])));
  CHECK(!web_json_valid(NULL, 0));
  /* deep nesting is refused, not a stack overflow */
  char deep[300];
  memset(deep, '[', 150); memset(deep + 150, ']', 150);
  CHECK(!web_json_valid(deep, 300));
  memset(deep, '[', 20); memset(deep + 20, ']', 20);
  CHECK(web_json_valid(deep, 40));

  const char *doc = "{\"state\":\"idle\",\"n\":[1,{\"x\":2}],\"ssid\":\"caf\\u00e9 \\\"\\ud83d\\ude81\\\"\",\"mode\":\"client\",\"at\":123}";
  const char *v, *ve;
  CHECK(web_json_find(doc, strlen(doc), "at", &v, &ve));
  CHECK_INT(strtol(v, NULL, 10), 123);
  CHECK(web_json_find(doc, strlen(doc), "n", &v, &ve));
  CHECK_INT(ve - v, 11);
  CHECK(!web_json_find(doc, strlen(doc), "x", &v, &ve));                           /* only top-level keys */
  CHECK(!web_json_find("[1]", 3, "a", &v, &ve));
  CHECK(!web_json_find("{}", 2, "a", &v, &ve));
  char s[64];
  CHECK(web_json_get_str(doc, strlen(doc), "ssid", s, sizeof s));
  CHECK_STR(s, "caf\xC3\xA9 \"\xF0\x9F\x9A\x81\"");
  CHECK(web_json_get_str(doc, strlen(doc), "mode", s, sizeof s));
  CHECK_STR(s, "client");
  CHECK(!web_json_get_str(doc, strlen(doc), "at", s, sizeof s));                    /* not a string */
  CHECK(web_json_get_str(doc, strlen(doc), "state", s, 3));                         /* truncated */
  CHECK_STR(s, "id");
  /* the first of two equal keys; spaces around everything; the end of one value */
  const char *dup = " { \"k\" : 1 , \"k\" : 2 } ";
  CHECK(web_json_find(dup, strlen(dup), "k", &v, &ve));
  CHECK_INT(*v, '1');
  CHECK(web_json_find("{\"a\":{\"k\":1},\"k\":\"x\"}", 21, "k", &v, &ve));
  CHECK_INT(*v, '"');
  CHECK(!web_json_find("{\"a\":1,}", 8, "b", &v, &ve));
  CHECK(!web_json_find("{\"a\" 1}", 7, "a", &v, &ve));
  const char *two = "[1, 2]  x";
  CHECK(web_json_skip(two, two + strlen(two)) == two + 8);                        /* past the value and its spaces */
  CHECK(web_json_skip("[1,", (const char *)"[1," + 3) == NULL);
  const char *nul = "{\"a\":\"\\u0000x\\udc00\"}";
  CHECK(web_json_get_str(nul, strlen(nul), "a", s, sizeof s));                     /* NUL and a lone surrogate */
  CHECK_STR(s, "\xEF\xBF\xBDx\xEF\xBF\xBD");
}

static void test_sb(void) {
  sb_t b;
  sb_init(&b, 4);
  for (int i = 0; i < 1000; i++) sb_printf(&b, "%d,", i);
  CHECK(!b.oom);
  CHECK(!strncmp(b.p, "0,1,2,", 6));
  CHECK_INT(b.p[b.len], 0);
  b.len = 0;
  sb_json_str(&b, "a\"b");
  sb_puts(&b, " ");
  sb_json_str(&b, NULL);
  sb_puts(&b, " ");
  sb_json_num(&b, NAN, 1);
  sb_puts(&b, " ");
  sb_json_num(&b, INFINITY, 1);
  sb_puts(&b, " ");
  sb_json_num(&b, 2.345, 2);
  CHECK_STR(b.p, "\"a\\\"b\" \"\" null null 2.35");
  sb_free(&b);
  CHECK(b.p == NULL);
}

static void test_names_flags(void) {
  CHECK(web_is_device_name("con"));
  CHECK(web_is_device_name("CON"));
  CHECK(web_is_device_name("con.txt"));
  CHECK(web_is_device_name("Nul.tar.gz"));
  CHECK(web_is_device_name("prn"));
  CHECK(web_is_device_name("aux"));
  CHECK(web_is_device_name("com1"));
  CHECK(web_is_device_name("COM9.lua"));
  CHECK(web_is_device_name("lpt0"));
  CHECK(!web_is_device_name("console"));
  CHECK(!web_is_device_name("com10"));
  CHECK(!web_is_device_name("com"));
  CHECK(!web_is_device_name("lptx"));
  CHECK(!web_is_device_name("c1.txt"));
  CHECK(!web_is_device_name("acon"));
  CHECK(!web_is_device_name(""));

  CHECK(web_is_temp_name(".st-tmp-12-3"));
  CHECK(web_is_temp_name("c1.txt.tmp~"));
  CHECK(!web_is_temp_name("st-tmp-1"));
  CHECK(!web_is_temp_name("c1.txt"));
  CHECK(!web_is_temp_name(".tmp"));
  char a[64], b2[64];
  web_temp_name(a, sizeof a);
  web_temp_name(b2, sizeof b2);
  CHECK(web_is_temp_name(a));
  CHECK(strcmp(a, b2) != 0);

  CHECK(!web_take_sd_dirty());
  web_note_sd_dirty();
  web_note_sd_dirty();
  CHECK(web_take_sd_dirty());
  CHECK(!web_take_sd_dirty());
}

int main(void) {
  test_url_decode();
  test_clean_path();
  test_json_escape();
  test_host_origin();
  test_etag_mime();
  test_json_scan();
  test_sb();
  test_names_flags();
  T_DONE();
}
