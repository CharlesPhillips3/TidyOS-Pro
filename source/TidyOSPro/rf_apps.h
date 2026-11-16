#pragma once
// ====================================================================== TidyOS Pro field tools
// (included from apps.cpp, after the Ping app)
//
//   RF Sweep       live 902-928 MHz spectrum, peak hold and a waterfall
//   Meter Traffic  how busy the band and each channel are; one-channel burst counter
//   Site Survey    GPS-tagged readings logged to the SD card, drawn as a map; KML export
//   RF Exposure    power density worked out from the readings, against the FCC limits,
//                  plus a calculator for "this transmitter at this distance"
//   GPS            the GNSS module's fix, satellites and position
//   Wi-Fi Scan     2.4 GHz networks nearby, their channels and signal
//
// How the exposure figure is worked out (RF Exposure):
//   The radio reports power at its input in dBm. Corrected for the front-end gain and any
//   attenuator, that's the power P the antenna delivered. An antenna of gain G has an effective
//   area Ae = G * lambda^2 / (4 pi) (about 135 cm^2 for 2 dBi at 915 MHz), so the power density
//   that delivered it is S = P / Ae. Summing every channel's time-averaged power gives the
//   band's average density, compared with the FCC's 0.61 mW/cm^2 (general public, 915 MHz).
//   It's an estimate: the antenna's real gain, the board's own front-end gain and how the
//   signal arrives (direction, polarisation, reflections) all move it by several dB, and the
//   receiver tops out a little below 0 dBm, so right next to a transmitter it reads low
//   (it says so). For a measurement you can sign, use a calibrated RF exposure meter.

#include "rf_engine.h"
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

static const RGB888 RF_BLUE(0, 85, 170), RF_CYAN(0, 170, 170), RF_GREEN(0, 170, 0), RF_RED(170, 0, 0), RF_YEL(255, 255, 85);

// value 0..1 -> blue, cyan, green, yellow, red (the 8-colour screen shows blue/cyan/green/red)
static RGB888 rfHeat(float v)
{
  if (v < 0.2f) return RF_BLUE;
  if (v < 0.4f) return RF_CYAN;
  if (v < 0.6f) return RF_GREEN;
  if (v < 0.8f) return RF_YEL;
  return RF_RED;
}

static std::string rfStamp()
{
  time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
  char b[32]; strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tmv);
  return b;
}

static std::string rfDir()
{
  std::string d = gSDMounted ? std::string(SD_MOUNT "/rf") : dataPath("rf");
  mkdir(d.c_str(), 0777);
  return d;
}

static bool rfNeedRadio(const char * app)
{
  if (rf::radioOk()) return true;
  std::string m = std::string("The 900 MHz radio isn't working: ") + rf::lastError();
  gApp->messageBox(app, m.c_str(), "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
  return false;
}

static void rfFmtFreq(char * b, size_t n, int ch) { snprintf(b, n, "%.1f MHz", rf::chFreq(ch)); }

// ---------------------------------------------------------------- spectrum + waterfall view
class SpectrumView : public uiControl {
public:
  float lo = -130, hi = -30;        // dBm range shown
  int   sel = -1;                   // clicked channel
  bool  showWf = true;
  std::function<void(int)> onPick;
  SpectrumView(uiWindow * p, Point pos, Size sz) : uiControl(p, pos, sz, true, 0) {
    windowStyle().borderSize = 1; windowStyle().borderColor = T.shadow; windowStyle().activeBorderColor = T.shadow;
    setFocusIndex(-1);
  }
  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    if (e->id == UIEVT_MOUSEBUTTONDOWN) {
      Rect r = clientRect(uiOrigin::Window);
      int x = e->params.mouse.status.X - r.X1 - 30;
      int w = r.width() - 34;
      if (x >= 0 && x < w) { sel = x * rf::NCH / w; repaint(); if (onPick) onPick(sel); }
      return;
    }
    if (e->id != UIEVT_PAINT) return;
    Rect r = clientRect(uiOrigin::Window);
    beginPaint(e, r);
    auto cv = canvas();
    cv->setBrushColor(T.win);
    cv->fillRectangle(r.X1, r.Y1, r.X2, r.Y2);
    if (!rf::gSpec) return;
    int gx = r.X1 + 30, gw = r.width() - 34;
    int specH = showWf ? r.height() * 55 / 100 : r.height() - 16;
    int gy = r.Y1 + 4, gh = specH - 4;
    auto Y = [&](float dbm) { float v = (dbm - lo) / (hi - lo); v = v < 0 ? 0 : v > 1 ? 1 : v; return gy + gh - 1 - (int)(v * (gh - 1)); };
    auto X = [&](int ch) { return gx + ch * (gw - 1) / (rf::NCH - 1); };
    // grid, dBm labels
    cv->setPenColor(T.face);
    for (float d = -120; d <= hi; d += 20) { int y = Y(d); cv->drawLine(gx, y, gx + gw - 1, y); }
    for (int f = 905; f <= 925; f += 5) { int x = gx + (int)((f - rf::F0) / (rf::STEP * (rf::NCH - 1)) * (gw - 1)); cv->drawLine(x, gy, x, gy + gh - 1); }
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    char b[16];
    for (float d = -120; d <= hi; d += 20) { snprintf(b, sizeof(b), "%d", (int)d); cv->drawText(&FONT_std_12, r.X1 + 2, Y(d) - 6, b); }
    float avg[rf::NCH], pk[rf::NCH], hold[rf::NCH], nf;
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      memcpy(avg, rf::gSpec->avg, sizeof(avg)); memcpy(pk, rf::gSpec->peak, sizeof(pk)); memcpy(hold, rf::gSpec->hold, sizeof(hold));
      nf = rf::gSpec->noiseFloor;
    }
    // noise floor and busy threshold
    cv->setPenColor(RF_GREEN);
    int ny = Y(nf); for (int x = gx; x < gx + gw; x += 4) cv->drawLine(x, ny, x + 1, ny);
    cv->setPenColor(RF_RED);
    int ty = Y(nf + rf::gSettings.threshDb); for (int x = gx; x < gx + gw; x += 6) cv->drawLine(x, ty, x + 2, ty);
    // peak hold (grey), peak (blue), average (black, 2 px)
    auto line = [&](float * v, RGB888 c, bool thick) {
      cv->setPenColor(c);
      for (int i = 1; i < rf::NCH; ++i) {
        cv->drawLine(X(i - 1), Y(v[i - 1]), X(i), Y(v[i]));
        if (thick) cv->drawLine(X(i - 1), Y(v[i - 1]) - 1, X(i), Y(v[i]) - 1);
      }
    };
    line(hold, T.face == T.win ? T.shadow : RGB888(170, 170, 170), false);
    line(pk, RF_BLUE, false);
    line(avg, T.text, true);
    if (sel >= 0) { cv->setPenColor(RF_RED); int x = X(sel); cv->drawLine(x, gy, x, gy + gh - 1); }
    // frequency labels
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    int ly = gy + gh + 1;
    for (int f = 905; f <= 925; f += 5) {
      int x = gx + (int)((f - rf::F0) / (rf::STEP * (rf::NCH - 1)) * (gw - 1));
      snprintf(b, sizeof(b), "%d", f); cv->drawText(&FONT_std_12, x - 9, ly, b);
    }
    cv->drawText(&FONT_std_12, gx + gw - 26, ly, "MHz");
    if (!showWf) return;
    // waterfall: newest at the top
    int wy = ly + 14, wh = r.Y2 - wy - 1;
    if (wh < 8) return;
    int rows = rf::WF_ROWS, rowH = imax(1, wh / rows);
    std::lock_guard<std::mutex> lk(rf::gMx);
    for (int k = 0; k < rows; ++k) {
      int row = (rf::gSpec->wfHead - 1 - k + rows * 2) % rows;
      int y = wy + k * rowH;
      if (y + rowH > r.Y2) break;
      for (int c = 0; c < rf::NCH; ++c) {
        int8_t v = rf::gSpec->wf[row][c];
        if (v == -128) continue;
        float t = (v - (nf - 3)) / 30.0f;          // 0 = at the noise floor, 1 = 30 dB above it
        if (t < 0.12f) continue;                   // quiet: leave white
        cv->setBrushColor(rfHeat(t));
        int x1 = X(c), x2 = c + 1 < rf::NCH ? X(c + 1) - 1 : gx + gw - 1;
        cv->fillRectangle(x1, y, imax(x1, x2), y + rowH - 1);
      }
    }
  }
};

// ====================================================================== RF Sweep
class RfSweepApp : public AppFrame {
public:
  static AppFrame * inst;
  SpectrumView * view;
  uiButton * runBtn;
  uiLabel * status, * status2;
  uiTextEdit * antEd, * feEd, * attEd, * thrEd;
  uint32_t lastSweeps = 0;

  RfSweepApp() : AppFrame("RF Sweep  902-928 MHz", Point(4, 4), Size(632, 452), &inst) {
    runBtn = button("Start", cx, cy, 70);
    runBtn->onClick = [this]() { toggle(); };
    button("Reset", cx + 76, cy, 60)->onClick = [this]() { rf::resetStats(); view->repaint(); };
    button("Save CSV", cx + 142, cy, 80)->onClick = [this]() { saveCsv(); };
    button("Listen", cx + 228, cy, 64)->onClick = [this]() { listenHere(); };
    auto wfb = button("Waterfall", cx + 298, cy, 84);
    wfb->onClick = [this]() { view->showWf = !view->showWf; view->repaint(); };
    label("Ant dBi", cx + 392, cy + 3, 52, 18);
    antEd = edit("", cx + 444, cy, 40);
    label("Atten dB", cx + 490, cy + 3, 58, 18);
    attEd = edit("", cx + 548, cy, 40);
    view = new SpectrumView(this, Point(cx, cy + 28), Size(618, 330));
    view->onPick = [this](int) { refresh(true); };
    status = label("", cx, cy + 362, 618, 18);
    status2 = label("", cx, cy + 380, 618, 18);
    status2->labelStyle().textFont = &FONT_std_12;
    label("Front-end dB", cx, cy + 403, 84, 18);
    feEd = edit("", cx + 86, cy + 400, 44);
    label("Busy = noise +", cx + 140, cy + 403, 96, 18);
    thrEd = edit("", cx + 236, cy + 400, 40);
    button("Apply", cx + 282, cy + 400, 60)->onClick = [this]() { applySettings(); };
    auto hint = label("Click the graph to pick a channel. Black = average, blue = peak, grey = peak hold.", cx + 350, cy + 404, 268, 18);
    hint->labelStyle().textFont = &FONT_std_12;
    showSettings();
    if (rf::mode() == rf::Mode::Off && rf::radioOk()) rf::setMode(rf::Mode::Sweep);
    refresh(true);
    startTimer(400);
    onTimer = [this](uiTimerHandle) { refresh(false); };
  }

  void showSettings() {
    char b[16];
    snprintf(b, sizeof(b), "%.1f", rf::gSettings.antGainDbi); antEd->setText(b);
    snprintf(b, sizeof(b), "%.1f", rf::gSettings.attenDb);    attEd->setText(b);
    snprintf(b, sizeof(b), "%.1f", rf::gSettings.frontEndDb); feEd->setText(b);
    snprintf(b, sizeof(b), "%.0f", rf::gSettings.threshDb);   thrEd->setText(b);
  }
  void applySettings() {
    rf::gSettings.antGainDbi = atof(antEd->text());
    rf::gSettings.attenDb    = atof(attEd->text());
    rf::gSettings.frontEndDb = atof(feEd->text());
    float t = atof(thrEd->text()); rf::gSettings.threshDb = t < 3 ? 3 : t > 40 ? 40 : t;
    rf::saveSettings();
    showSettings();
    antEd->repaint(); attEd->repaint(); feEd->repaint(); thrEd->repaint();
    rf::resetStats();
  }
  void toggle() {
    if (!rfNeedRadio("RF Sweep")) return;
    rf::setMode(rf::mode() == rf::Mode::Sweep ? rf::Mode::Off : rf::Mode::Sweep);
    refresh(true);
  }
  void listenHere() {
    int ch = view->sel >= 0 ? view->sel : rf::gSpec ? rf::gSpec->bandPeakCh : 64;
    if (!rfNeedRadio("RF Sweep")) return;
    rf::setMode(rf::Mode::Listen, ch);
    openMeterTraffic();
  }
  void saveCsv() {
    if (!rf::gSpec) return;
    std::string path = rfDir() + "/sweep-" + rfStamp() + ".csv";
    std::string s = "channel,freq_mhz,avg_dbm,peak_dbm,hold_dbm,busy_pct,avg_mw_per_cm2\n";
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      for (int c = 0; c < rf::NCH; ++c) {
        char b[160];
        double busy = rf::gSpec->samples[c] ? 100.0 * rf::gSpec->busy[c] / rf::gSpec->samples[c] : 0;
        double mw = rf::gSpec->samples[c] ? rf::gSpec->linSum[c] / rf::gSpec->samples[c] : 0;
        double s_ = mw > 0 ? rf::powerDensity(10 * log10(mw), rf::gSettings.antGainDbi, rf::chFreq(c)) : 0;
        snprintf(b, sizeof(b), "%d,%.1f,%.1f,%.1f,%.1f,%.2f,%.3e\n", c, rf::chFreq(c), rf::gSpec->avg[c], rf::gSpec->peak[c], rf::gSpec->hold[c], busy, s_);
        s += b;
      }
      char b[200];
      snprintf(b, sizeof(b), "# noise floor %.1f dBm, %u sweeps, antenna %.1f dBi, atten %.1f dB, front-end %.1f dB",
               rf::gSpec->noiseFloor, (unsigned)rf::gSpec->sweeps, rf::gSettings.antGainDbi, rf::gSettings.attenDb, rf::gSettings.frontEndDb);
      s += b;
      if (rf::gGnss.fix) { snprintf(b, sizeof(b), ", at %.6f,%.6f", rf::gGnss.lat, rf::gGnss.lon); s += b; }
      s += "\n";
    }
    bool ok = writeFileStr(path.c_str(), s);
    gApp->messageBox("RF Sweep", ok ? ("Saved " + path + " (opens in Tidy)").c_str() : "Couldn't save the file.", "OK", nullptr, nullptr, ok ? uiMessageBoxIcon::Info : uiMessageBoxIcon::Warning);
  }
  void refresh(bool force) {
    bool on = rf::mode() == rf::Mode::Sweep;
    runBtn->setText(on ? "Stop" : "Start");
    runBtn->buttonStyle().backgroundColor = on ? T.accent : T.face;
    runBtn->buttonStyle().textColor = on ? T.accentText : T.text;
    runBtn->repaint();
    if (!rf::gSpec) { status->setText(rf::lastError()); status->repaint(); return; }
    uint32_t sw = rf::gSpec->sweeps;
    if (sw == lastSweeps && !force) return;
    lastSweeps = sw;
    view->repaint();
    char b[200], f[20];
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      rfFmtFreq(f, sizeof(f), rf::gSpec->bandPeakCh);
      snprintf(b, sizeof(b), "Noise floor %.1f dBm   Peak %.1f dBm at %s   Busy %.1f%%   %s",
               rf::gSpec->noiseFloor, rf::gSpec->bandPeak, f, rf::gSpec->occupancy * 100,
               rf::gSpec->overload ? "OVERLOAD: too strong to read - move back or add an attenuator" : "");
      status->setText(b);
      if (view->sel >= 0) {
        int c = view->sel; rfFmtFreq(f, sizeof(f), c);
        double busy = rf::gSpec->samples[c] ? 100.0 * rf::gSpec->busy[c] / rf::gSpec->samples[c] : 0;
        snprintf(b, sizeof(b), "%s (ch %d): avg %.1f, peak %.1f, hold %.1f dBm, busy %.1f%%.   %u sweeps, %u ms each%s",
                 f, c, rf::gSpec->avg[c], rf::gSpec->peak[c], rf::gSpec->hold[c], busy, (unsigned)sw, (unsigned)rf::gSpec->sweepMs,
                 rf::radioOk() ? "" : "   (no radio)");
      } else
        snprintf(b, sizeof(b), "%u sweeps, %u ms each, 129 channels of 200 kHz.  Readings at the antenna, corrected by the settings below.", (unsigned)sw, (unsigned)rf::gSpec->sweepMs);
      status2->setText(b);
    }
    status->repaint(); status2->repaint();
  }
};
AppFrame * RfSweepApp::inst = nullptr;
void openRfSweep() { if (!RfSweepApp::inst) new RfSweepApp(); RfSweepApp::inst->open(); }

// ====================================================================== Meter Traffic
class BusyBars : public uiControl {
public:
  BusyBars(uiWindow * p, Point pos, Size sz) : uiControl(p, pos, sz, true, 0) {
    windowStyle().borderSize = 1; windowStyle().borderColor = T.shadow; windowStyle().activeBorderColor = T.shadow; setFocusIndex(-1);
  }
  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    if (e->id != UIEVT_PAINT) return;
    Rect r = clientRect(uiOrigin::Window);
    beginPaint(e, r);
    auto cv = canvas();
    cv->setBrushColor(T.win); cv->fillRectangle(r.X1, r.Y1, r.X2, r.Y2);
    if (!rf::gSpec) return;
    int gx = r.X1 + 30, gw = r.width() - 34, gy = r.Y1 + 4, gh = r.height() - 22;
    float pct[rf::NCH], mx = 1;
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      for (int c = 0; c < rf::NCH; ++c) { pct[c] = rf::gSpec->samples[c] ? 100.0f * rf::gSpec->busy[c] / rf::gSpec->samples[c] : 0; if (pct[c] > mx) mx = pct[c]; }
    }
    float top = mx <= 5 ? 5 : mx <= 10 ? 10 : mx <= 25 ? 25 : mx <= 50 ? 50 : 100;
    cv->setPenColor(T.face);
    for (int k = 1; k <= 4; ++k) { int y = gy + gh - gh * k / 4; cv->drawLine(gx, y, gx + gw - 1, y); }
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    char b[16];
    for (int k = 0; k <= 4; ++k) { snprintf(b, sizeof(b), "%g%%", top * k / 4); cv->drawText(&FONT_std_12, r.X1 + 1, gy + gh - gh * k / 4 - 6, b); }
    for (int c = 0; c < rf::NCH; ++c) {
      int x1 = gx + c * gw / rf::NCH, x2 = gx + (c + 1) * gw / rf::NCH - 1;
      int h = (int)(gh * fminf(1, pct[c] / top));
      if (h <= 0) continue;
      cv->setBrushColor(rfHeat(pct[c] / top));
      cv->fillRectangle(x1, gy + gh - h, imax(x1, x2), gy + gh - 1);
    }
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    for (int f = 905; f <= 925; f += 5) {
      int x = gx + (int)((f - rf::F0) / (rf::STEP * rf::NCH) * gw);
      snprintf(b, sizeof(b), "%d", f); cv->drawText(&FONT_std_12, x - 9, gy + gh + 3, b);
    }
  }
};

class TraceView : public uiControl {
public:
  TraceView(uiWindow * p, Point pos, Size sz) : uiControl(p, pos, sz, true, 0) {
    windowStyle().borderSize = 1; windowStyle().borderColor = T.shadow; windowStyle().activeBorderColor = T.shadow; setFocusIndex(-1);
  }
  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    if (e->id != UIEVT_PAINT) return;
    Rect r = clientRect(uiOrigin::Window);
    beginPaint(e, r);
    auto cv = canvas();
    cv->setBrushColor(T.win); cv->fillRectangle(r.X1, r.Y1, r.X2, r.Y2);
    if (!rf::gListen) return;
    float tr[200]; int head; float nz;
    { std::lock_guard<std::mutex> lk(rf::gMx); memcpy(tr, rf::gListen->trace, sizeof(tr)); head = rf::gListen->traceHead; nz = rf::gListen->noise; }
    float lo = -130, hi = -20;
    int gx = r.X1 + 30, gw = r.width() - 34, gy = r.Y1 + 3, gh = r.height() - 6;
    auto Y = [&](float d) { float v = (d - lo) / (hi - lo); v = v < 0 ? 0 : v > 1 ? 1 : v; return gy + gh - 1 - (int)(v * (gh - 1)); };
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    char b[12];
    for (float d = -120; d <= -20; d += 25) { snprintf(b, sizeof(b), "%d", (int)d); cv->drawText(&FONT_std_12, r.X1 + 1, Y(d) - 6, b); }
    cv->setPenColor(RF_RED);
    int ty = Y(nz + rf::gSettings.threshDb); for (int x = gx; x < gx + gw; x += 6) cv->drawLine(x, ty, x + 2, ty);
    cv->setPenColor(RF_BLUE);
    int px = -1, py = 0;
    for (int i = 0; i < 200; ++i) {
      float v = tr[(head + i) % 200];
      int x = gx + i * (gw - 1) / 199, y = Y(v);
      if (px >= 0) cv->drawLine(px, py, x, y);
      px = x; py = y;
    }
  }
};

class MeterTrafficApp : public AppFrame {
public:
  static AppFrame * inst;
  BusyBars * bars;
  TraceView * trace;
  uiLabel * head, * l1, * l2, * l3;
  uiTextEdit * freqEd;
  uiButton * bandBtn, * oneBtn;
  uiListBox * list;

  MeterTrafficApp() : AppFrame("Meter Traffic", Point(10, 8), Size(620, 446), &inst) {
    bandBtn = button("Whole band", cx, cy, 100);
    bandBtn->onClick = [this]() { if (rfNeedRadio("Meter Traffic")) { rf::setMode(rf::Mode::Sweep); refresh(); } };
    oneBtn = button("One channel", cx + 106, cy, 100);
    oneBtn->onClick = [this]() { listen(); };
    label("MHz", cx + 214, cy + 3, 30, 18);
    freqEd = edit("915.0", cx + 244, cy, 60);
    freqEd->onKeyType = [this](uiKeyEventInfo const & k) { if (k.VK == VK_RETURN || k.VK == VK_KP_ENTER) listen(); };
    button("Stop", cx + 310, cy, 54)->onClick = [this]() { rf::setMode(rf::Mode::Off); refresh(); };
    button("Reset", cx + 370, cy, 56)->onClick = [this]() { rf::resetStats(); refresh(); };
    button("Save report", cx + 432, cy, 96)->onClick = [this]() { saveReport(); };
    head = label("", cx, cy + 28, 606, 18);
    bars = new BusyBars(this, Point(cx, cy + 48), Size(606, 170));
    trace = new TraceView(this, Point(cx, cy + 48), Size(606, 120));
    l1 = label("", cx, cy + 224, 606, 18);
    l2 = label("", cx, cy + 242, 606, 18);
    l3 = label("", cx, cy + 260, 606, 18);
    l1->labelStyle().textFont = l2->labelStyle().textFont = l3->labelStyle().textFont = &FONT_std_12;
    list = S(new uiListBox(this, Point(cx, cy + 282), Size(606, 124)));
    list->items().takeStrings();
    list->listBoxStyle().textFont = &FONT_8x14;
    list->listBoxStyle().itemHeight = 16;
    auto note = label("Counts energy bursts above the noise, so it works for OpenWay (hw 2/3/3.1) and Riva alike. It can't read or decode them.", cx, cy + 410, 606, 18);
    note->labelStyle().textFont = &FONT_std_12;
    if (rf::gListen && rf::mode() == rf::Mode::Listen) { char b[16]; snprintf(b, sizeof(b), "%.1f", rf::chFreq(rf::gListen->ch)); freqEd->setText(b); }
    else if (rf::mode() == rf::Mode::Off && rf::radioOk()) rf::setMode(rf::Mode::Sweep);
    refresh();
    startTimer(500);
    onTimer = [this](uiTimerHandle) { refresh(); };
  }

  void listen() {
    if (!rfNeedRadio("Meter Traffic")) return;
    float f = atof(freqEd->text());
    int ch = (int)lroundf((f - rf::F0) / rf::STEP);
    ch = ch < 0 ? 0 : ch >= rf::NCH ? rf::NCH - 1 : ch;
    char b[16]; snprintf(b, sizeof(b), "%.1f", rf::chFreq(ch)); freqEd->setText(b); freqEd->repaint();
    rf::setMode(rf::Mode::Listen, ch);
    refresh();
  }

  void busiest(std::vector<std::pair<float, int>> & v) {
    v.clear();
    for (int c = 0; c < rf::NCH; ++c)
      if (rf::gSpec->samples[c]) v.push_back({ 100.0f * rf::gSpec->busy[c] / rf::gSpec->samples[c], c });
    std::sort(v.begin(), v.end(), [](const std::pair<float, int> & a, const std::pair<float, int> & b) { return a.first > b.first; });
  }

  void refresh() {
    bool one = rf::mode() == rf::Mode::Listen;
    if (bars->state().visible == one) gApp->showWindow(bars, !one);
    if (trace->state().visible != one) gApp->showWindow(trace, one);
    bandBtn->buttonStyle().backgroundColor = rf::mode() == rf::Mode::Sweep ? T.accent : T.face;
    bandBtn->buttonStyle().textColor = rf::mode() == rf::Mode::Sweep ? T.accentText : T.text;
    oneBtn->buttonStyle().backgroundColor = one ? T.accent : T.face;
    oneBtn->buttonStyle().textColor = one ? T.accentText : T.text;
    bandBtn->repaint(); oneBtn->repaint();
    if (!rf::gSpec) { head->setText(rf::lastError()); head->repaint(); return; }
    char b[220];
    std::vector<std::string> rows;
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      uint32_t secs = (millis() - (one ? rf::gListen->startedAt : rf::gSpec->resetAt)) / 1000;
      if (secs == 0) secs = 1;
      if (!one) {
        uint64_t bz = 0, n = 0;
        for (int c = 0; c < rf::NCH; ++c) { bz += rf::gSpec->busy[c]; n += rf::gSpec->samples[c]; }
        snprintf(b, sizeof(b), "Whole band, %u s: busy %.2f%% of the time on average.  Noise floor %.1f dBm.  %s",
                 (unsigned)secs, n ? 100.0 * bz / n : 0.0, rf::gSpec->noiseFloor, rf::mode() == rf::Mode::Off ? "(stopped)" : "");
        head->setText(b);
        std::vector<std::pair<float, int>> v; busiest(v);
        int active = 0; for (auto & p : v) if (p.first >= 1) ++active;
        snprintf(b, sizeof(b), "Channels with traffic (busy 1%% or more): %d of %d.", active, rf::NCH); l1->setText(b);
        snprintf(b, sizeof(b), "Strongest last sweep: %.1f dBm at %.1f MHz.  Peak hold anywhere: %.1f dBm.",
                 rf::gSpec->bandPeak, rf::chFreq(rf::gSpec->bandPeakCh), *std::max_element(rf::gSpec->hold, rf::gSpec->hold + rf::NCH)); l2->setText(b);
        l3->setText("Busiest channels:");
        for (int i = 0; i < (int)v.size() && i < 12; ++i) {
          int c = v[i].second;
          snprintf(b, sizeof(b), "%6.1f MHz  ch %3d   busy %5.1f%%   avg %6.1f  peak hold %6.1f dBm", rf::chFreq(c), c, v[i].first, rf::gSpec->avg[c], rf::gSpec->hold[c]);
          rows.push_back(b);
        }
      } else {
        auto & L = *rf::gListen;
        snprintf(b, sizeof(b), "%.1f MHz (ch %d), %u s: %u bursts = %.1f a minute, on the air %.2f%% of the time.",
                 rf::chFreq(L.ch), L.ch, (unsigned)secs, (unsigned)L.bursts, L.bursts * 60.0 / secs, L.busyMs * 100.0 / (secs * 1000.0));
        head->setText(b);
        snprintf(b, sizeof(b), "Noise here %.1f dBm, now %.1f dBm, busy above %.1f dBm.", L.noise, L.now, L.noise + rf::gSettings.threshDb); l1->setText(b);
        static const char * nm[8] = { "<2", "2-5", "5-10", "10-20", "20-50", "50-100", "100-250", "250+" };
        std::string h = "Burst lengths (ms):";
        for (int i = 0; i < 8; ++i) { snprintf(b, sizeof(b), "  %s: %u", nm[i], (unsigned)L.hist[i]); h += b; }
        l2->setText(h.c_str());
        l3->setText("Latest bursts:");
        for (int i = 0; i < L.recentN; ++i) {
          auto & bu = L.recent[(L.recentHead - 1 - i + 32) % 32];
          snprintf(b, sizeof(b), "%7.1f s ago   %5u ms   peak %4d dBm", (millis() - bu.atMs) / 1000.0, (unsigned)bu.lenMs, (int)bu.peakDbm);
          rows.push_back(b);
        }
      }
    }
    head->repaint(); l1->repaint(); l2->repaint(); l3->repaint();
    list->items().clear();
    for (auto & r : rows) list->items().append(r.c_str());
    list->repaint();
    (one ? (uiControl *)trace : (uiControl *)bars)->repaint();
  }

  void saveReport() {
    if (!rf::gSpec) return;
    std::string path = rfDir() + "/traffic-" + rfStamp() + ".csv";
    std::string s;
    char b[200];
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      if (rf::mode() == rf::Mode::Listen) {
        auto & L = *rf::gListen;
        uint32_t secs = imax(1, (int)((millis() - L.startedAt) / 1000));
        snprintf(b, sizeof(b), "# one channel %.1f MHz, %u s, %u bursts, %.2f%% on air, noise %.1f dBm\nago_s,len_ms,peak_dbm\n",
                 rf::chFreq(L.ch), (unsigned)secs, (unsigned)L.bursts, L.busyMs * 100.0 / (secs * 1000.0), L.noise);
        s += b;
        for (int i = 0; i < L.recentN; ++i) {
          auto & bu = L.recent[(L.recentHead - 1 - i + 32) % 32];
          snprintf(b, sizeof(b), "%.1f,%u,%d\n", (millis() - bu.atMs) / 1000.0, (unsigned)bu.lenMs, (int)bu.peakDbm); s += b;
        }
      } else {
        s = "freq_mhz,busy_pct,avg_dbm,hold_dbm\n";
        for (int c = 0; c < rf::NCH; ++c) {
          snprintf(b, sizeof(b), "%.1f,%.2f,%.1f,%.1f\n", rf::chFreq(c), rf::gSpec->samples[c] ? 100.0 * rf::gSpec->busy[c] / rf::gSpec->samples[c] : 0.0, rf::gSpec->avg[c], rf::gSpec->hold[c]);
          s += b;
        }
      }
    }
    bool ok = writeFileStr(path.c_str(), s);
    gApp->messageBox("Meter Traffic", ok ? ("Saved " + path).c_str() : "Couldn't save the file.", "OK", nullptr, nullptr, ok ? uiMessageBoxIcon::Info : uiMessageBoxIcon::Warning);
  }
};
AppFrame * MeterTrafficApp::inst = nullptr;
void openMeterTraffic() { if (!MeterTrafficApp::inst) new MeterTrafficApp(); MeterTrafficApp::inst->open(); }

// ====================================================================== Site Survey
struct SurveyPt { double lat, lon; float nf, peak, occ, sdens; uint32_t t; bool fix; int mark; };
static std::vector<SurveyPt> * s_svy = nullptr;    // in PSRAM via the default allocator
static std::string s_svyFile;
static volatile bool s_svyOn = false;
static int s_svyEvery = 5, s_svyMarks = 0;
static uint32_t s_svyLast = 0;

static float bandDensityNow()      // mW/cm^2 from the last sweep's averages (call with gMx held)
{
  double s = 0;
  for (int c = 0; c < rf::NCH; ++c) s += rf::powerDensity(rf::gSpec->avg[c], rf::gSettings.antGainDbi, rf::chFreq(c));
  return (float)s;
}

static void surveyAddPoint(int mark)
{
  if (!s_svy || !rf::gSpec) return;
  SurveyPt p;
  {
    std::lock_guard<std::mutex> lk(rf::gMx);
    p.lat = rf::gGnss.lat; p.lon = rf::gGnss.lon; p.fix = rf::gGnss.fix;
    p.nf = rf::gSpec->noiseFloor; p.peak = rf::gSpec->bandPeak; p.occ = rf::gSpec->occupancy;
    p.sdens = bandDensityNow();
  }
  p.t = time(nullptr); p.mark = mark;
  s_svy->push_back(p);
  if (!s_svyFile.empty()) {
    FILE * f = fopen(s_svyFile.c_str(), "a");
    if (f) {
      struct tm tmv; time_t tt = p.t; localtime_r(&tt, &tmv);
      char ts[24]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
      fprintf(f, "%s,%s,%.7f,%.7f,%d,%.1f,%.1f,%.2f,%.3e,%s\n", ts, p.fix ? "yes" : "no", p.lat, p.lon, rf::gGnss.sats,
              p.nf, p.peak, p.occ * 100, p.sdens, mark ? ("mark " + std::to_string(mark)).c_str() : "");
      fclose(f);
    }
  }
}

class SurveyMap : public uiControl {
public:
  int colorBy = 0;     // 0 noise floor, 1 busy %, 2 peak
  SurveyMap(uiWindow * p, Point pos, Size sz) : uiControl(p, pos, sz, true, 0) {
    windowStyle().borderSize = 1; windowStyle().borderColor = T.shadow; windowStyle().activeBorderColor = T.shadow; setFocusIndex(-1);
  }
  float val(const SurveyPt & p) { return colorBy == 0 ? p.nf : colorBy == 1 ? p.occ * 100 : p.peak; }
  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    if (e->id != UIEVT_PAINT) return;
    Rect r = clientRect(uiOrigin::Window);
    beginPaint(e, r);
    auto cv = canvas();
    cv->setBrushColor(T.win); cv->fillRectangle(r.X1, r.Y1, r.X2, r.Y2);
    cv->setPenColor(T.text);
    if (!s_svy || s_svy->empty()) {
      cv->drawText(&FONT_std_14, r.X1 + 12, r.Y1 + 12, "No points yet. Start logging, then walk or drive the area.");
      cv->drawText(&FONT_std_12, r.X1 + 12, r.Y1 + 32, "RF Sweep keeps running while you log; each point is GPS + that moment's readings.");
      return;
    }
    double la0 = 1e9, la1 = -1e9, lo0 = 1e9, lo1 = -1e9; float v0 = 1e9, v1 = -1e9;
    for (auto & p : *s_svy) {
      if (!p.fix) continue;
      la0 = fmin(la0, p.lat); la1 = fmax(la1, p.lat); lo0 = fmin(lo0, p.lon); lo1 = fmax(lo1, p.lon);
      v0 = fminf(v0, val(p)); v1 = fmaxf(v1, val(p));
    }
    char b[96];
    if (la0 > la1) { cv->drawText(&FONT_std_14, r.X1 + 12, r.Y1 + 12, "Points logged, but no GPS fix yet - take the board outside."); return; }
    if (v1 - v0 < 1) v1 = v0 + 1;
    // keep the map square in metres
    double midLat = (la0 + la1) / 2, kx = cos(midLat * M_PI / 180);
    double spanX = fmax((lo1 - lo0) * kx, 1e-5), spanY = fmax(la1 - la0, 1e-5);
    int pad = 14, w = r.width() - pad * 2, h = r.height() - pad * 2 - 16;
    double sc = fmin(w / spanX, h / spanY);
    int ox = r.X1 + pad + (int)((w - spanX * sc) / 2), oy = r.Y1 + pad + (int)((h - spanY * sc) / 2);
    auto PX = [&](const SurveyPt & p) { return ox + (int)((p.lon - lo0) * kx * sc); };
    auto PY = [&](const SurveyPt & p) { return oy + (int)((la1 - p.lat) * sc); };
    cv->setPenColor(T.face);
    int lx = -1, ly = 0;
    for (auto & p : *s_svy) { if (!p.fix) continue; int x = PX(p), y = PY(p); if (lx >= 0) cv->drawLine(lx, ly, x, y); lx = x; ly = y; }
    for (auto & p : *s_svy) {
      if (!p.fix) continue;
      int x = PX(p), y = PY(p);
      cv->setBrushColor(rfHeat((val(p) - v0) / (v1 - v0)));
      cv->fillRectangle(x - 3, y - 3, x + 3, y + 3);
      if (p.mark) { cv->setPenColor(T.text); cv->setBrushColor(T.win); snprintf(b, sizeof(b), "%d", p.mark); cv->drawText(&FONT_std_12, x + 5, y - 6, b); }
    }
    // where we are now
    if (rf::gnssFixed()) {
      SurveyPt n; { std::lock_guard<std::mutex> lk(rf::gMx); n.lat = rf::gGnss.lat; n.lon = rf::gGnss.lon; }
      int x = PX(n), y = PY(n);
      cv->setPenColor(T.text); cv->drawRectangle(x - 6, y - 6, x + 6, y + 6); cv->drawLine(x - 9, y, x + 9, y); cv->drawLine(x, y - 9, x, y + 9);
    }
    // legend + scale
    int ly2 = r.Y2 - 16;
    const char * what = colorBy == 0 ? "noise floor dBm" : colorBy == 1 ? "busy %" : "peak dBm";
    for (int i = 0; i < 5; ++i) { cv->setBrushColor(rfHeat(i / 4.0f + 0.01f)); cv->fillRectangle(r.X1 + 8 + i * 16, ly2 + 2, r.X1 + 22 + i * 16, ly2 + 12); }
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    snprintf(b, sizeof(b), "%.1f ... %.1f %s", v0, v1, what);
    cv->drawText(&FONT_std_12, r.X1 + 92, ly2 + 1, b);
    double metres = spanY * 111320.0;
    snprintf(b, sizeof(b), "map height %.0f m, %d points", metres, (int)s_svy->size());
    cv->drawText(&FONT_std_12, r.X2 - 200, ly2 + 1, b);
  }
};

class SiteSurveyApp : public AppFrame {
public:
  static AppFrame * inst;
  SurveyMap * map;
  uiButton * logBtn, * colBtn;
  uiTextEdit * everyEd;
  uiLabel * stat, * gps;
  SiteSurveyApp() : AppFrame("Site Survey", Point(8, 6), Size(624, 448), &inst) {
    logBtn = button("Start logging", cx, cy, 110);
    logBtn->onClick = [this]() { toggle(); };
    button("Mark here", cx + 116, cy, 84)->onClick = [this]() { if (s_svyOn) { surveyAddPoint(++s_svyMarks); map->repaint(); } };
    label("Every", cx + 208, cy + 3, 40, 18);
    everyEd = edit(std::to_string(s_svyEvery).c_str(), cx + 248, cy, 36);
    label("s", cx + 288, cy + 3, 12, 18);
    colBtn = button("Colour: noise", cx + 304, cy, 112);
    colBtn->onClick = [this]() { map->colorBy = (map->colorBy + 1) % 3; colBtn->setText(map->colorBy == 0 ? "Colour: noise" : map->colorBy == 1 ? "Colour: busy" : "Colour: peak"); colBtn->repaint(); map->repaint(); };
    button("Export KML", cx + 422, cy, 92)->onClick = [this]() { exportKml(); };
    button("Clear", cx + 520, cy, 90)->onClick = [this]() { if (s_svy && !s_svyOn) { s_svy->clear(); map->repaint(); } };
    gps = label("", cx, cy + 28, 610, 18);
    map = new SurveyMap(this, Point(cx, cy + 48), Size(610, 340));
    stat = label("", cx, cy + 392, 610, 18);
    stat->labelStyle().textFont = &FONT_std_12;
    auto n = label("Logs to SD:/rf/survey-*.csv (opens in Tidy). KML opens in Google Earth. Points without a GPS fix are kept but not mapped.", cx, cy + 410, 610, 18);
    n->labelStyle().textFont = &FONT_std_12;
    if (!s_svy) s_svy = new std::vector<SurveyPt>();
    refresh();
    startTimer(1000);
    onTimer = [this](uiTimerHandle) { tick(); };
  }
  void toggle() {
    if (s_svyOn) { s_svyOn = false; refresh(); return; }
    if (!rfNeedRadio("Site Survey")) return;
    if (rf::mode() != rf::Mode::Sweep) rf::setMode(rf::Mode::Sweep);
    int ev = atoi(everyEd->text()); s_svyEvery = ev < 1 ? 1 : ev > 600 ? 600 : ev;
    s_svyFile = rfDir() + "/survey-" + rfStamp() + ".csv";
    writeFileStr(s_svyFile.c_str(), "time,gps_fix,lat,lon,sats,noise_floor_dbm,peak_dbm,busy_pct,band_mw_per_cm2,note\n");
    s_svy->clear(); s_svyMarks = 0;
    s_svyOn = true; s_svyLast = 0;
    if (!rf::gGnss.present) gApp->messageBox("Site Survey", "No GPS module was found, so points are logged without a position.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
    refresh();
  }
  void tick() {
    if (s_svyOn && millis() - s_svyLast >= (uint32_t)s_svyEvery * 1000) { s_svyLast = millis(); surveyAddPoint(0); map->repaint(); }
    else if (rf::gnssFixed()) map->repaint();
    refresh();
  }
  void refresh() {
    logBtn->setText(s_svyOn ? "Stop logging" : "Start logging");
    logBtn->buttonStyle().backgroundColor = s_svyOn ? T.accent : T.face;
    logBtn->buttonStyle().textColor = s_svyOn ? T.accentText : T.text;
    logBtn->repaint();
    char b[200];
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      auto & g = rf::gGnss;
      if (!g.present) snprintf(b, sizeof(b), "GPS: no module found on the GNSS header");
      else if (!g.fix) snprintf(b, sizeof(b), "GPS: searching... %d satellites in view", g.satsInView);
      else snprintf(b, sizeof(b), "GPS: %.6f, %.6f   %d sats, HDOP %.1f, %.0f m, %.0f km/h", g.lat, g.lon, g.sats, g.hdop, g.altM, g.speedKmh);
      gps->setText(b);
      if (rf::gSpec)
        snprintf(b, sizeof(b), "%s   Now: noise %.1f dBm, peak %.1f dBm, busy %.1f%%   %d points",
                 s_svyOn ? ("Logging to " + s_svyFile).c_str() : "Not logging.", rf::gSpec->noiseFloor, rf::gSpec->bandPeak, rf::gSpec->occupancy * 100, s_svy ? (int)s_svy->size() : 0);
      else snprintf(b, sizeof(b), "%s", rf::lastError());
      stat->setText(b);
    }
    gps->repaint(); stat->repaint();
  }
  void exportKml() {
    if (!s_svy || s_svy->empty()) return;
    std::string path = (s_svyFile.empty() ? rfDir() + "/survey-" + rfStamp() + ".csv" : s_svyFile);
    path = path.substr(0, path.size() - 4) + ".kml";
    float v0 = 1e9, v1 = -1e9;
    for (auto & p : *s_svy) if (p.fix) { v0 = fminf(v0, p.nf); v1 = fmaxf(v1, p.nf); }
    if (v1 - v0 < 1) v1 = v0 + 1;
    bool ok = writeFileWith(path.c_str(), [&](RSink & o) {
      o.put("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><name>TidyOS Pro site survey</name>\n");
      static const char * col[5] = { "ffaa5500", "ffaaaa00", "ff00aa00", "ff55ffff", "ff0000aa" };   // aabbggrr
      for (int i = 0; i < 5; ++i) {
        char b[200]; snprintf(b, sizeof(b), "<Style id=\"s%d\"><IconStyle><color>%s</color><scale>0.7</scale><Icon><href>http://maps.google.com/mapfiles/kml/shapes/shaded_dot.png</href></Icon></IconStyle></Style>\n", i, col[i]);
        o.put(b);
      }
      for (auto & p : *s_svy) {
        if (!p.fix) continue;
        int k = (int)fminf(4, fmaxf(0, (p.nf - v0) / (v1 - v0) * 5));
        char b[400];
        struct tm tmv; time_t tt = p.t; localtime_r(&tt, &tmv); char ts[24]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        snprintf(b, sizeof(b), "<Placemark><name>%s</name><description>%s&lt;br&gt;noise floor %.1f dBm&lt;br&gt;peak %.1f dBm&lt;br&gt;busy %.1f%%</description><styleUrl>#s%d</styleUrl><Point><coordinates>%.7f,%.7f,0</coordinates></Point></Placemark>\n",
                 p.mark ? ("Mark " + std::to_string(p.mark)).c_str() : "", ts, p.nf, p.peak, p.occ * 100, k, p.lon, p.lat);
        o.put(b);
      }
      o.put("</Document></kml>\n");
    });
    gApp->messageBox("Site Survey", ok ? ("Saved " + path).c_str() : "Couldn't save the KML file.", "OK", nullptr, nullptr, ok ? uiMessageBoxIcon::Info : uiMessageBoxIcon::Warning);
  }
  bool canClose() override {
    if (s_svyOn) return gApp->messageBox("Site Survey", "Stop logging and close?", "Stop", "Keep logging", nullptr, uiMessageBoxIcon::Question) == uiMessageBoxResult::Button1 ? (s_svyOn = false, true) : false;
    return true;
  }
};
AppFrame * SiteSurveyApp::inst = nullptr;
void openSiteSurvey() { if (!SiteSurveyApp::inst) new SiteSurveyApp(); SiteSurveyApp::inst->open(); }

// ====================================================================== RF Exposure
class RfExposureApp : public AppFrame {
public:
  static AppFrame * inst;
  uiLabel * big, * verdict, * l[7];
  uiTextEdit * pwEd, * dutyEd, * gEd, * distEd, * nEd;
  uiLabel * calc, * calc2;
  RfExposureApp() : AppFrame("RF Exposure (estimate)", Point(40, 10), Size(560, 440), &inst) {
    auto h = label("Measured here (since the last Reset in RF Sweep)", cx, cy, 540, 18);
    h->labelStyle().textFont = &FONT_std_14;
    big = label("", cx, cy + 22, 540, 26);
    big->labelStyle().textFont = &FONT_std_22;
    verdict = label("", cx, cy + 52, 540, 18);
    for (int i = 0; i < 7; ++i) { l[i] = label("", cx, cy + 74 + i * 17, 540, 17); l[i]->labelStyle().textFont = &FONT_std_12; }
    auto h2 = label("Calculator: one transmitter at a distance", cx, cy + 202, 540, 18);
    h2->labelStyle().textFont = &FONT_std_14;
    label("Power W", cx, cy + 227, 60, 18);     pwEd = edit("1", cx + 60, cy + 224, 44);
    label("Duty %", cx + 112, cy + 227, 48, 18); dutyEd = edit("2", cx + 160, cy + 224, 40);
    label("Gain dBi", cx + 208, cy + 227, 56, 18); gEd = edit("2", cx + 264, cy + 224, 36);
    label("Distance m", cx + 306, cy + 227, 72, 18); distEd = edit("1", cx + 378, cy + 224, 40);
    label("How many", cx + 424, cy + 227, 60, 18); nEd = edit("1", cx + 486, cy + 224, 40);
    button("Work it out", cx, cy + 252, 100)->onClick = [this]() { work(); };
    calc = label("", cx + 108, cy + 255, 432, 18);
    calc2 = label("", cx, cy + 278, 540, 36);
    calc2->labelStyle().textFont = &FONT_std_12;
    static const char * notes[] = {
      "How it's worked out: power at the antenna / the antenna's effective area (G x wavelength^2 / 4 pi) = power density.",
      "Limits (FCC 47 CFR 1.1310, at 915 MHz): 0.61 mW/cm^2 general public, 3.05 mW/cm^2 occupational, averaged 30 min.",
      "It's an estimate, good to a few dB: antenna gain, the board's front-end and reflections all move it. The receiver",
      "tops out near 0 dBm, so right beside a meter it reads LOW (you'll see OVERLOAD). For a signed-off number use a",
      "calibrated broadband RF exposure meter. Meters and their ICMs are FCC-certified to meet these limits at 20 cm.",
    };
    for (int i = 0; i < 5; ++i) { auto n = label(notes[i], cx, cy + 320 + i * 16, 540, 16); n->labelStyle().textFont = &FONT_std_12; }
    if (rf::mode() == rf::Mode::Off && rf::radioOk()) rf::setMode(rf::Mode::Sweep);
    work();
    refresh();
    startTimer(1000);
    onTimer = [this](uiTimerHandle) { refresh(); };
  }
  void work() {
    float pw = atof(pwEd->text()), duty = atof(dutyEd->text()), g = atof(gEd->text()), d = atof(distEd->text());
    int n = atoi(nEd->text()); if (n < 1) n = 1;
    if (d < 0.05f) d = 0.05f;
    // far-field estimate: S = P * G / (4 pi r^2), in mW/cm^2
    double peak = pw * 1000.0 * pow(10.0, g / 10.0) / (4 * M_PI * pow(d * 100.0, 2));
    double avg = peak * duty / 100.0 * n;
    double lim = rf::fccGeneral(915);
    char b[200];
    snprintf(b, sizeof(b), "%.4f mW/cm^2 average = %.2f%% of the public limit", avg, avg / lim * 100);
    calc->setText(b);
    snprintf(b, sizeof(b), "While one is sending: %.4f mW/cm^2 (%.1f%% of the limit). %s",
             peak, peak / lim * 100, d < 0.2f ? "Closer than 20 cm the simple formula overstates it (near field)." : "");
    calc2->setText(b);
    calc->repaint(); calc2->repaint();
  }
  void refresh() {
    if (!rf::gSpec) { big->setText(rf::lastError()); big->repaint(); return; }
    double avgS = 0, peakS = 0; float peakDbm = -200; int peakCh = 0; bool over; uint32_t secs, sw;
    {
      std::lock_guard<std::mutex> lk(rf::gMx);
      for (int c = 0; c < rf::NCH; ++c) {
        if (rf::gSpec->samples[c]) {
          double mw = rf::gSpec->linSum[c] / rf::gSpec->samples[c];
          avgS += rf::powerDensity(10 * log10(mw), rf::gSettings.antGainDbi, rf::chFreq(c));
        }
        if (rf::gSpec->hold[c] > peakDbm) { peakDbm = rf::gSpec->hold[c]; peakCh = c; }
      }
      peakS = rf::powerDensity(peakDbm, rf::gSettings.antGainDbi, rf::chFreq(peakCh));
      over = rf::gSpec->overload; secs = (millis() - rf::gSpec->resetAt) / 1000; sw = rf::gSpec->sweeps;
    }
    double gen = rf::fccGeneral(915), occ = rf::fccOccupational(915);
    char b[200];
    snprintf(b, sizeof(b), "%.2e mW/cm^2  =  %.4f%% of the limit", avgS, avgS / gen * 100);
    big->setText(b);
    const char * v = avgS / gen < 0.01 ? "Far below the FCC general-public limit." :
                     avgS / gen < 0.1  ? "Well below the FCC general-public limit." :
                     avgS / gen < 0.5  ? "Below the FCC limit." : "Getting close to the FCC general-public limit - check with a calibrated meter.";
    snprintf(b, sizeof(b), "%s%s", v, over ? "  OVERLOAD seen: real values are higher than shown." : "");
    verdict->setText(b);
    verdict->labelStyle().textColor = over ? RF_RED : T.text;
    snprintf(b, sizeof(b), "Band average over %u s (%u sweeps): %.3e mW/cm^2 = %.4f%% public, %.5f%% occupational.", (unsigned)secs, (unsigned)sw, avgS, avgS / gen * 100, avgS / occ * 100);
    l[0]->setText(b);
    snprintf(b, sizeof(b), "Strongest moment: %.1f dBm at %.1f MHz = %.3e mW/cm^2 (%.3f%% of the limit, while it lasted).", peakDbm, rf::chFreq(peakCh), peakS, peakS / gen * 100);
    l[1]->setText(b);
    snprintf(b, sizeof(b), "Same in other units: %.3f uW/cm^2 average, %.4f W/m^2, about %.2f V/m.", avgS * 1000, avgS * 10, sqrt(avgS * 10 * 377));
    l[2]->setText(b);
    snprintf(b, sizeof(b), "Settings: antenna %.1f dBi, attenuator %.1f dB, front-end correction %.1f dB (change them in RF Sweep).",
             rf::gSettings.antGainDbi, rf::gSettings.attenDb, rf::gSettings.frontEndDb);
    l[3]->setText(b);
    l[4]->setText(rf::mode() == rf::Mode::Sweep ? "Sweeping 902-928 MHz now. Leave it a few minutes in one spot for a steady average." : "The sweep is stopped: start it in RF Sweep to update this.");
    l[5]->setText("Hold the board where a person would be (head / chest height), away from your body and metal.");
    l[6]->setText("");
    big->repaint(); verdict->repaint(); for (auto x : l) x->repaint();
  }
};
AppFrame * RfExposureApp::inst = nullptr;
void openRfExposure() { if (!RfExposureApp::inst) new RfExposureApp(); RfExposureApp::inst->open(); }

// ====================================================================== GPS
class GpsApp : public AppFrame {
public:
  static AppFrame * inst;
  uiLabel * l[11];
  GpsApp() : AppFrame("GPS", Point(150, 60), Size(380, 300), &inst) {
    for (int i = 0; i < 11; ++i) { l[i] = label("", cx, cy + i * 20, 366, 18); }
    l[0]->labelStyle().textFont = &FONT_std_18;
    button("Save waypoint", cx, cy + 230, 120)->onClick = [this]() { waypoint(); };
    refresh();
    startTimer(1000);
    onTimer = [this](uiTimerHandle) { refresh(); };
  }
  static std::string dms(double v, bool lat) {
    char h = lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W');
    v = fabs(v); int d = (int)v; double m = (v - d) * 60; int mi = (int)m; double s = (m - mi) * 60;
    char b[40]; snprintf(b, sizeof(b), "%d\xB0 %02d' %04.1f\" %c", d, mi, s, h); return b;
  }
  void refresh() {
    rf::Gnss g; { std::lock_guard<std::mutex> lk(rf::gMx); g = rf::gGnss; }
    char b[120];
    if (!g.present) {
      l[0]->setText("No GPS module found");
      l[1]->setText("Plug the GNSS module into the V4's GNSS header");
      l[2]->setText("and restart. (The SD card can't share GPIO 39-42.)");
      for (int i = 3; i < 11; ++i) l[i]->setText("");
    } else {
      l[0]->setText(g.fix ? "3D fix" : "Searching for satellites...");
      snprintf(b, sizeof(b), "Latitude   %.7f   %s", g.lat, dms(g.lat, true).c_str()); l[1]->setText(b);
      snprintf(b, sizeof(b), "Longitude  %.7f   %s", g.lon, dms(g.lon, false).c_str()); l[2]->setText(b);
      snprintf(b, sizeof(b), "Altitude   %.1f m  (%.0f ft)", g.altM, g.altM * 3.28084); l[3]->setText(b);
      snprintf(b, sizeof(b), "Satellites %d used, %d in view   HDOP %.1f", g.sats, g.satsInView, g.hdop); l[4]->setText(b);
      snprintf(b, sizeof(b), "Speed      %.1f km/h (%.1f mph), heading %.0f\xB0", g.speedKmh, g.speedKmh * 0.621371, g.courseDeg); l[5]->setText(b);
      if (g.year) snprintf(b, sizeof(b), "UTC        %04d-%02d-%02d %02d:%02d:%02d", g.year, g.month, g.day, g.hour, g.minute, g.second);
      else snprintf(b, sizeof(b), "UTC        (waiting)");
      l[6]->setText(b);
      snprintf(b, sizeof(b), "Module     %d baud, RX on GPIO %d, %u sentences", g.baud, g.rxPin, (unsigned)g.sentences); l[7]->setText(b);
      snprintf(b, sizeof(b), "Last data  %.1f s ago", (millis() - g.lastSentenceMs) / 1000.0); l[8]->setText(b);
      l[9]->setText(g.hdop < 2 ? "Accuracy: good (a few metres)" : g.hdop < 5 ? "Accuracy: fair" : "Accuracy: poor - give it a clear view of the sky");
      l[10]->setText("");
    }
    for (auto x : l) x->repaint();
  }
  void waypoint() {
    rf::Gnss g; { std::lock_guard<std::mutex> lk(rf::gMx); g = rf::gGnss; }
    if (!g.fix) { gApp->messageBox("GPS", "No fix yet.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info); return; }
    std::string path = rfDir() + "/waypoints.csv";
    bool fresh = fileSize(path.c_str()) <= 0;
    FILE * f = fopen(path.c_str(), "a");
    if (f) {
      if (fresh) fputs("time,lat,lon,alt_m,sats,hdop\n", f);
      struct tm tmv; time_t tt = time(nullptr); localtime_r(&tt, &tmv); char ts[24]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
      fprintf(f, "%s,%.7f,%.7f,%.1f,%d,%.1f\n", ts, g.lat, g.lon, g.altM, g.sats, g.hdop);
      fclose(f);
    }
    gApp->messageBox("GPS", f ? ("Added to " + path).c_str() : "Couldn't save.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
  }
};
AppFrame * GpsApp::inst = nullptr;
void openGps() { if (!GpsApp::inst) new GpsApp(); GpsApp::inst->open(); }

// ====================================================================== Wi-Fi Scan
struct WifiRow { std::string ssid; int rssi, ch; uint8_t bssid[6]; wifi_auth_mode_t auth; };
static std::vector<WifiRow> s_wifi;
static volatile int s_wifiState = 0;    // 0 idle, 1 scanning, 2 done
static void wifiScanTask(void *)
{
  int n = WiFi.scanNetworks(false, true);
  std::vector<WifiRow> v;
  for (int i = 0; i < n; ++i) {
    WifiRow r; r.ssid = WiFi.SSID(i).c_str(); r.rssi = WiFi.RSSI(i); r.ch = WiFi.channel(i); r.auth = WiFi.encryptionType(i);
    memcpy(r.bssid, WiFi.BSSID(i), 6);
    v.push_back(r);
  }
  WiFi.scanDelete();
  std::sort(v.begin(), v.end(), [](const WifiRow & a, const WifiRow & b) { return a.rssi > b.rssi; });
  s_wifi.swap(v);
  s_wifiState = 2;
  vTaskDelete(nullptr);
}
class WifiScanApp : public AppFrame {
public:
  static AppFrame * inst;
  uiListBox * list; uiLabel * sum; uiButton * btn;
  int shown = -1;
  WifiScanApp() : AppFrame("Wi-Fi Scan", Point(60, 30), Size(540, 400), &inst) {
    btn = button("Scan", cx, cy, 80); btn->onClick = [this]() { scan(); };
    sum = label("", cx + 90, cy + 3, 436, 18);
    list = S(new uiListBox(this, Point(cx, cy + 28), Size(526, 330)));
    list->items().takeStrings();
    list->listBoxStyle().textFont = &FONT_8x14;
    list->listBoxStyle().itemHeight = 16;
    scan();
    startTimer(500);
    onTimer = [this](uiTimerHandle) { if (s_wifiState == 2 && shown != 2) show(); };
  }
  void scan() {
    if (s_wifiState == 1) return;
    s_wifiState = 1; shown = 1;
    sum->setText("Scanning 2.4 GHz channels 1-13..."); sum->repaint();
    xTaskCreatePinnedToCore(wifiScanTask, "wscan", 4096, nullptr, 1, nullptr, 0);
  }
  void show() {
    shown = 2;
    list->items().clear();
    int perCh[15] = {0};
    for (auto & r : s_wifi) {
      char b[120];
      const char * a = r.auth == WIFI_AUTH_OPEN ? "open" : r.auth == WIFI_AUTH_WEP ? "WEP" : r.auth == WIFI_AUTH_WPA3_PSK ? "WPA3" : r.auth == WIFI_AUTH_WPA2_WPA3_PSK ? "WPA2/3" : "WPA2";
      snprintf(b, sizeof(b), "%4d dBm  ch %2d  %-6s %02X:%02X:%02X:%02X:%02X:%02X  %s", r.rssi, r.ch, a,
               r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5], r.ssid.empty() ? "(hidden)" : r.ssid.c_str());
      list->items().append(b);
      if (r.ch > 0 && r.ch < 15) perCh[r.ch]++;
    }
    list->repaint();
    char b[160];
    int best = 1; for (int c : { 1, 6, 11 }) if (perCh[c] < perCh[best]) best = c;
    snprintf(b, sizeof(b), "%d networks. Busiest: ch1 %d, ch6 %d, ch11 %d. Quietest of 1/6/11: %d", (int)s_wifi.size(), perCh[1], perCh[6], perCh[11], best);
    sum->setText(b); sum->repaint();
  }
};
AppFrame * WifiScanApp::inst = nullptr;
void openWifiScan() { if (!WifiScanApp::inst) new WifiScanApp(); WifiScanApp::inst->open(); }

// ====================================================================== Clone (clone.cpp)
namespace clone {
extern volatile int gPct; extern volatile bool gBusy, gDone, gOk;
bool start(bool withShelf, bool autoPins);
bool shelfInstalled();
std::string status();
std::vector<std::string> logLines();
}
class CloneApp : public AppFrame {
public:
  static AppFrame * inst;
  uiButton * goBtn, * shelfBtn, * autoBtn;
  uiLabel * st, * pct;
  uiListBox * log;
  bool withShelf = false, autoPins = false;
  CloneApp() : AppFrame("Clone to another V4", Point(40, 12), Size(560, 436), &inst) {
    static const char * how[] = {
      "Copies this board's firmware onto another Heltec V4 by wire. Your settings, Wi-Fi password",
      "and files are NOT copied: the other board starts fresh, at the boot menu.",
      "",
      "  This board  GPIO 43 (TX)  ->  RX  (GPIO 44)  other V4",
      "  This board  GPIO 44 (RX)  <-  TX  (GPIO 43)  other V4",
      "  This board  GND           --  GND             other V4",
      "  Optional:   GPIO 3 -> RST,  GPIO 33 -> GPIO 0 (PRG)   (then it resets by itself)",
      "",
      "Power the other board from its own USB-C. Without the optional wires: on the other board",
      "hold PRG, tap RST, let go of PRG, then press Start.",
    };
    for (int i = 0; i < 10; ++i) { auto l = label(how[i], cx, cy + i * 16, 546, 16); l->labelStyle().textFont = i >= 3 && i <= 6 ? &FONT_8x14 : &FONT_std_12; }
    int y = cy + 168;
    shelfBtn = button("", cx, y, 200); shelfBtn->onClick = [this]() { if (clone::shelfInstalled()) withShelf = !withShelf; show(); };
    autoBtn = button("", cx + 206, y, 220); autoBtn->onClick = [this]() { autoPins = !autoPins; show(); };
    goBtn = button("Start", cx + 432, y, 114); goBtn->onClick = [this]() { go(); };
    st = label("", cx, y + 28, 546, 18);
    pct = label("", cx, y + 46, 546, 18);
    pct->labelStyle().textFont = &FONT_8x14;
    log = S(new uiListBox(this, Point(cx, y + 68), Size(546, 170)));
    log->items().takeStrings();
    log->listBoxStyle().textFont = &FONT_std_12;
    log->listBoxStyle().itemHeight = 15;
    show();
    startTimer(400);
    onTimer = [this](uiTimerHandle) { show(); };
  }
  void go() {
    if (clone::gBusy) return;
    if (gApp->messageBox("Clone", "This erases everything on the OTHER board and puts this firmware on it. Go ahead?", "Erase and copy", "Cancel", nullptr, uiMessageBoxIcon::Question) != uiMessageBoxResult::Button1) return;
    clone::start(withShelf, autoPins);
  }
  int lastN = -1;
  void show() {
    bool sh = clone::shelfInstalled();
    shelfBtn->setText(!sh ? "The Shelf: not installed here" : withShelf ? "[x] Include The Shelf" : "[ ] Include The Shelf");
    autoBtn->setText(autoPins ? "[x] Auto reset (GPIO 3 / 33)" : "[ ] Auto reset (GPIO 3 / 33)");
    goBtn->setText(clone::gBusy ? "Copying..." : "Start");
    shelfBtn->repaint(); autoBtn->repaint(); goBtn->repaint();
    st->setText(clone::status().c_str()); st->repaint();
    int p = clone::gPct;
    pct->setText(p >= 0 ? (bar(p / 100.0, 40) + " " + std::to_string(p) + "%").c_str() : ""); pct->repaint();
    auto L = clone::logLines();
    if ((int)L.size() != lastN) {
      lastN = L.size();
      log->items().clear(); for (auto & l : L) log->items().append(l.c_str()); log->repaint();
    }
  }
  bool canClose() override { return !clone::gBusy; }
};
AppFrame * CloneApp::inst = nullptr;
void openClone() { if (!CloneApp::inst) new CloneApp(); CloneApp::inst->open(); }

// ====================================================================== The Shelf (dual boot)
void openShelfSwitch()
{
  if (!clone::shelfInstalled()) {
    gApp->messageBox("The Shelf", "The Shelf isn't installed on this board. The TidyOS Pro web flasher can add it (dual boot).", "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
    return;
  }
  if (gApp->messageBox("The Shelf", "Restart into The Shelf? (Restarting from The Shelf brings back the boot menu.)", "Restart", "Cancel", nullptr, uiMessageBoxIcon::Question) != uiMessageBoxResult::Button1) return;
  Preferences p;
  if (p.begin("tidyboot", false)) { p.putString("once", "shelf"); p.end(); }
  if (const esp_partition_t * m = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "menu")) esp_ota_set_boot_partition(m);
  rf::powerDown();
  delay(200);
  ESP.restart();
}
