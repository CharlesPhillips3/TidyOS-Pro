// TidyOS Pro - Clone: copy this board's firmware onto another Heltec V4, by wire.
//
// This board talks to the other board's ROM bootloader (the same protocol esptool uses) over
// three wires, reading its own flash and writing it across:
//
//    this board            other V4
//    GPIO 43 (TX)  ----->  RX (GPIO 44)
//    GPIO 44 (RX)  <-----  TX (GPIO 43)
//    GND           ------  GND
//    GPIO 3        ----->  RST        (optional: resets it into download mode by itself)
//    GPIO 33       ----->  GPIO 0/PRG (optional, with GPIO 3)
//
// Without the two optional wires: on the other board hold PRG, tap RST, let go of PRG.
// Power the other board from its own USB-C (or 5V to 5V).
//
// What's copied: the bootloader, the partition table, the boot menu, TidyOS Pro and (if asked
// and installed) The Shelf with its web app. NOT copied: this board's settings and Wi-Fi
// password (NVS) and anyone's files (internal storage), so the copy starts fresh.
#include "config.h"
#include "sdkconfig.h"
#include <Arduino.h>
#include <esp_partition.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include "mbedtls/md5.h"
#include <string>
#include <vector>
#include <mutex>

#define CLONE_TX   43
#define CLONE_RX   44
#define CLONE_RST  3
#define CLONE_IO0  33

namespace clone {

volatile int  gPct = -1;                 // -1 idle, 0..100 working
volatile bool gBusy = false, gDone = false, gOk = false;
static std::mutex s_mx;
static std::vector<std::string> s_log;
static std::string s_status = "Ready.";

static void say(const char * fmt, ...)
{
  char b[160]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  Serial.printf("[clone] %s\n", b);
  std::lock_guard<std::mutex> lk(s_mx);
  s_status = b; s_log.push_back(b);
  if (s_log.size() > 40) s_log.erase(s_log.begin());
}
std::string status() { std::lock_guard<std::mutex> lk(s_mx); return s_status; }
std::vector<std::string> logLines() { std::lock_guard<std::mutex> lk(s_mx); return s_log; }

static HardwareSerial & U = Serial2;

// ---- SLIP framing + ROM commands
static void slipByte(uint8_t c) { if (c == 0xC0) { U.write(0xDB); U.write(0xDC); } else if (c == 0xDB) { U.write(0xDB); U.write(0xDD); } else U.write(c); }
static void sendCmd(uint8_t op, const uint8_t * data, uint16_t len, uint32_t chk = 0)
{
  U.write(0xC0);
  uint8_t h[8] = { 0x00, op, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)chk, (uint8_t)(chk >> 8), (uint8_t)(chk >> 16), (uint8_t)(chk >> 24) };
  for (uint8_t c : h) slipByte(c);
  for (uint16_t i = 0; i < len; ++i) slipByte(data[i]);
  U.write(0xC0);
  U.flush();
}

// reads one SLIP frame; false on timeout
static bool readFrame(std::vector<uint8_t> & f, uint32_t timeoutMs)
{
  f.clear();
  uint32_t t0 = millis(); bool in = false, esc = false;
  while (millis() - t0 < timeoutMs) {
    if (!U.available()) { delay(1); continue; }
    uint8_t c = U.read();
    if (!in) { if (c == 0xC0) in = true; continue; }
    if (c == 0xC0) { if (f.empty()) continue; return true; }
    if (esc) { f.push_back(c == 0xDC ? 0xC0 : c == 0xDD ? 0xDB : c); esc = false; }
    else if (c == 0xDB) esc = true;
    else f.push_back(c);
  }
  return false;
}

// waits for the response to `op`; returns its data (status bytes included) or false
static bool response(uint8_t op, std::vector<uint8_t> & data, uint32_t timeoutMs, uint32_t * value = nullptr)
{
  uint32_t t0 = millis();
  std::vector<uint8_t> f;
  while (millis() - t0 < timeoutMs) {
    if (!readFrame(f, timeoutMs - (millis() - t0))) return false;
    if (f.size() < 8 || f[0] != 0x01 || f[1] != op) continue;          // something else (a late SYNC reply)
    uint16_t len = f[2] | (f[3] << 8);
    if (value) *value = f[4] | (f[5] << 8) | (f[6] << 16) | ((uint32_t)f[7] << 24);
    data.assign(f.begin() + 8, f.begin() + 8 + std::min<size_t>(len, f.size() - 8));
    // the S3 ROM ends every reply with 4 status bytes: status, error, 0, 0
    if (data.size() >= 4 && data[data.size() - 4] != 0) { say("The other board said error 0x%02X to command 0x%02X", data[data.size() - 3], op); return false; }
    return true;
  }
  return false;
}

static bool command(uint8_t op, const uint8_t * d, uint16_t n, uint32_t timeoutMs, uint32_t chk = 0, std::vector<uint8_t> * out = nullptr)
{
  std::vector<uint8_t> r;
  sendCmd(op, d, n, chk);
  bool ok = response(op, r, timeoutMs);
  if (out) *out = r;
  return ok;
}

static void putU32(uint8_t * p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void resetIntoDownload(bool autoPins)
{
#if CONFIG_SPIRAM_MODE_OCT
  autoPins = false;            // V4-R8: GPIO 33 belongs to its PSRAM
#endif
  if (!autoPins) return;
  pinMode(CLONE_RST, OUTPUT); pinMode(CLONE_IO0, OUTPUT);
  digitalWrite(CLONE_IO0, LOW);
  digitalWrite(CLONE_RST, LOW); delay(120);
  digitalWrite(CLONE_RST, HIGH); delay(60);        // comes out of reset with GPIO 0 low: download mode
  delay(250);
  digitalWrite(CLONE_IO0, HIGH);
}

static bool sync()
{
  uint8_t d[36] = { 0x07, 0x07, 0x12, 0x20 };
  memset(d + 4, 0x55, 32);
  for (int tries = 0; tries < 12; ++tries) {
    while (U.available()) U.read();
    sendCmd(0x08, d, sizeof(d));
    std::vector<uint8_t> r;
    if (response(0x08, r, 300)) {
      delay(100); while (U.available()) U.read();      // the ROM answers SYNC several times
      return true;
    }
  }
  return false;
}

struct Region { uint32_t addr, len; const char * what; bool erasedFill; };

// how much of a partition is in use: up to the last 4 KB sector that isn't all 0xFF
static uint32_t usedLength(uint32_t addr, uint32_t size)
{
  static uint8_t buf[4096];
  uint32_t used = 0;
  for (uint32_t off = 0; off < size; off += 4096) {
    if (esp_flash_read(nullptr, buf, addr + off, 4096) != ESP_OK) break;
    for (int i = 0; i < 4096; i += 4) if (*(uint32_t *)(buf + i) != 0xFFFFFFFF) { used = off + 4096; break; }
    if ((off & 0x3FFFF) == 0) vTaskDelay(1);
  }
  return used;
}

static bool writeRegion(const Region & r, uint32_t & doneAll, uint32_t totalAll)
{
  const uint32_t BLK = 0x400;                 // the ROM loader's write size
  uint32_t len = (r.len + BLK - 1) / BLK * BLK;
  uint32_t blocks = len / BLK;
  uint8_t p[20];
  putU32(p, len); putU32(p + 4, blocks); putU32(p + 8, BLK); putU32(p + 12, r.addr); putU32(p + 16, 0);
  say("Erasing %s (%u KB at 0x%06X)...", r.what, (unsigned)(len / 1024), (unsigned)r.addr);
  if (!command(0x02, p, 20, 5000 + len / 1024 * 40)) { say("The other board didn't start the write of %s.", r.what); return false; }
  static uint8_t pkt[16 + 0x400];
  for (uint32_t b = 0; b < blocks; ++b) {
    uint8_t * d = pkt + 16;
    if (r.erasedFill) memset(d, 0xFF, BLK);
    else {
      uint32_t off = b * BLK, n = std::min<uint32_t>(BLK, r.len > off ? r.len - off : 0);
      memset(d, 0xFF, BLK);
      if (n && esp_flash_read(nullptr, d, r.addr + off, n) != ESP_OK) { say("Couldn't read this board's flash."); return false; }
    }
    uint32_t chk = 0xEF; for (uint32_t i = 0; i < BLK; ++i) chk ^= d[i];
    putU32(pkt, BLK); putU32(pkt + 4, b); putU32(pkt + 8, 0); putU32(pkt + 12, 0);
    bool ok = false;
    for (int t = 0; t < 3 && !ok; ++t) ok = command(0x03, pkt, sizeof(pkt), 3000, chk);
    if (!ok) { say("Writing %s stopped at %u KB.", r.what, (unsigned)(b * BLK / 1024)); return false; }
    doneAll += BLK;
    gPct = (int)((uint64_t)doneAll * 100 / totalAll);
  }
  // check it: the other board works out the MD5 of what it now holds
  uint8_t m[16]; putU32(m, r.addr); putU32(m + 4, r.len); putU32(m + 8, 0); putU32(m + 12, 0);
  std::vector<uint8_t> out;
  if (!command(0x13, m, 16, 8000 + r.len / 1024 * 20, 0, &out) || out.size() < 32) { say("Couldn't check %s on the other board.", r.what); return false; }
  mbedtls_md5_context ctx; mbedtls_md5_init(&ctx); mbedtls_md5_starts_ret(&ctx);
  static uint8_t buf[4096];
  for (uint32_t off = 0; off < r.len; off += sizeof(buf)) {
    uint32_t n = std::min<uint32_t>(sizeof(buf), r.len - off);
    if (r.erasedFill) memset(buf, 0xFF, n); else esp_flash_read(nullptr, buf, r.addr + off, n);
    mbedtls_md5_update_ret(&ctx, buf, n);
  }
  uint8_t h[16]; mbedtls_md5_finish_ret(&ctx, h); mbedtls_md5_free(&ctx);
  char hex[33]; for (int i = 0; i < 16; ++i) sprintf(hex + i * 2, "%02x", h[i]);
  std::string theirs;
  if (out.size() >= 36) theirs.assign((const char *)out.data(), 32);           // ROM: 32 hex characters
  else { char t[33]; for (int i = 0; i < 16; ++i) sprintf(t + i * 2, "%02x", out[i]); theirs = t; }
  if (theirs != hex) { say("%s didn't check out on the other board (MD5 differs).", r.what); return false; }
  say("%s written and checked.", r.what);
  return true;
}

struct Job { bool withShelf, autoPins; };

static void cloneTask(void * a)
{
  Job job = *(Job *)a; delete (Job *)a;
#if CONFIG_SPIRAM_MODE_OCT
  job.autoPins = false;
#endif
  gBusy = true; gDone = false; gOk = false; gPct = 0;
  { std::lock_guard<std::mutex> lk(s_mx); s_log.clear(); }
  std::vector<Region> regs;
  // bootloader (0x0) and partition table (0x8000) as they are on this board
  regs.push_back({ 0x0, usedLength(0x0, 0x8000), "bootloader", false });
  regs.push_back({ 0x8000, 0x1000, "partition table", false });
  regs.push_back({ 0x9000, 0x5000, "settings (blank)", true });          // a fresh start: none of this board's settings
  regs.push_back({ 0xE000, 0x2000, "boot selection (blank)", true });    // starts in the boot menu
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t * p = esp_partition_get(it);
    bool shelf = !strcmp(p->label, "shelf");
    if (shelf && !job.withShelf) continue;
    uint32_t u = usedLength(p->address, p->size);
    if (!u) continue;
    static char names[4][24]; static int ni = 0; char * nm = names[ni++ & 3];
    snprintf(nm, 24, "%s", !strcmp(p->label, "menu") ? "boot menu" : !strcmp(p->label, "tidyos") ? "TidyOS Pro" : shelf ? "The Shelf" : p->label);
    regs.push_back({ p->address, u, nm, false });
  }
  esp_partition_iterator_release(it);
  if (job.withShelf) {
    const esp_partition_t * w = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
    if (w) { uint32_t u = usedLength(w->address, w->size); if (u) regs.push_back({ w->address, u, "The Shelf web app", false }); }
  }
  uint32_t total = 0; for (auto & r : regs) total += (r.len + 0x3FF) / 0x400 * 0x400;
  say("Copying %u KB in %d pieces.", (unsigned)(total / 1024), (int)regs.size());

  U.setRxBufferSize(1024);                    // (must come before begin)
  U.begin(115200, SERIAL_8N1, CLONE_RX, CLONE_TX);
  resetIntoDownload(job.autoPins);
  say("Looking for the other board's bootloader...");
  bool ok = sync();
  if (!ok) say("No answer. Check TX->RX, RX<-TX and GND, and that the other board is in download mode (hold PRG, tap RST).");
  if (ok) {
    say("Found it. Speeding up to 921600 baud.");
    uint8_t b[8]; putU32(b, 921600); putU32(b + 4, 0);
    if (command(0x0F, b, 8, 1000)) { delay(20); U.updateBaudRate(921600); delay(50); while (U.available()) U.read(); }
    else { say("Staying at 115200 baud (slower)."); }
    uint8_t att[8] = { 0 };
    command(0x0D, att, 8, 1000);                                       // SPI attach (ignore older ROMs' complaints)
    uint8_t prm[24]; putU32(prm, 0); putU32(prm + 4, 16 * 1024 * 1024); putU32(prm + 8, 64 * 1024); putU32(prm + 12, 4096); putU32(prm + 16, 256); putU32(prm + 20, 0xFFFF);
    command(0x0B, prm, 24, 1000);                                      // 16 MB flash
    uint32_t done = 0;
    for (auto & r : regs) { if (!(ok = writeRegion(r, done, total))) break; }
  }
  if (ok) {
    uint8_t e[4]; putU32(e, 0);                                       // 0 = leave the loader and run
    command(0x04, e, 4, 1000);
    if (job.autoPins) { digitalWrite(CLONE_RST, LOW); delay(100); digitalWrite(CLONE_RST, HIGH); }
    say(job.autoPins ? "Done. The other board is restarting into the boot menu." : "Done. Press RST on the other board to start it.");
  }
  U.end();
  if (job.autoPins) { pinMode(CLONE_RST, INPUT); pinMode(CLONE_IO0, INPUT); }
  gOk = ok; gDone = true; gBusy = false; gPct = ok ? 100 : -1;
  vTaskDelete(nullptr);
}

bool start(bool withShelf, bool autoPins)
{
  if (gBusy) return false;
  Job * j = new Job{ withShelf, autoPins };
  return xTaskCreatePinnedToCore(cloneTask, "clone", 8192, j, 1, nullptr, 0) == pdPASS;
}

bool shelfInstalled()
{
  const esp_partition_t * p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "shelf");
  if (!p) return false;
  uint8_t m = 0; esp_flash_read(nullptr, &m, p->address, 1);
  return m == 0xE9;
}

}  // namespace clone
