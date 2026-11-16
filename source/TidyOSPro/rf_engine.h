// TidyOS Pro - the 900 MHz engine: the Heltec V4's SX1262 used as a receiver, plus its GNSS.
//
// The radio only ever listens here. It steps across 902-928 MHz in 200 kHz channels (the
// Wi-SUN / FAN channel plan Itron Riva and most 900 MHz AMI mesh networks use, 902.2-927.8 MHz),
// reading the SX1262's instantaneous RSSI. From that it builds:
//   - a live spectrum (average, peak and peak-hold per channel) and a waterfall
//   - the noise floor (the quietest quarter of the band) and band occupancy
//   - "busy" counts per channel: how often a channel was above noise + threshold (meter bursts)
//   - one-channel listening: burst counting and burst lengths at full speed
//   - an exposure estimate (power density) from the measured power and the antenna gain
//
// It measures signal strength at the radio, not a calibrated field strength. See rf_apps.h for
// how the exposure estimate is worked out and why it is only an estimate.
#pragma once
#include <Arduino.h>
#include <mutex>

namespace rf {

constexpr int   NCH     = 129;        // 902.2 ... 927.8 MHz
constexpr float F0      = 902.2f;
constexpr float STEP    = 0.2f;
constexpr int   WF_ROWS = 64;         // waterfall history (sweeps)
inline float chFreq(int ch) { return F0 + STEP * ch; }

enum class Mode : uint8_t { Off, Sweep, Listen };

struct Settings {
  float antGainDbi   = 2.0f;    // stock Heltec V4 antenna is about 2 dBi; a Laird 902-928 whip is 3 dBi
  float frontEndDb   = 0.0f;    // correction for the V4's front-end (LNA) gain, from a calibration
  float attenDb      = 0.0f;    // an inline attenuator, if one is fitted
  float threshDb     = 10.0f;   // "busy" = this far above the noise floor
  int   dwellUs      = 1500;    // time on each channel per sweep (sweep mode)
};

struct Spectrum {
  float avg[NCH];               // dBm, average over the last sweep's samples
  float peak[NCH];              // dBm, highest sample in the last sweep
  float hold[NCH];              // dBm, peak hold since reset
  uint32_t busy[NCH];           // samples above noise + threshold, since reset
  uint32_t samples[NCH];        // samples taken, since reset
  double   linSum[NCH];         // sum of linear power (mW) of all samples, since reset (for the time average)
  float noiseFloor = -120;      // dBm: average of the quietest quarter of channels (last sweep)
  float bandPeak = -130;        // dBm: strongest sample of the last sweep
  int   bandPeakCh = 0;
  float occupancy = 0;          // 0..1: share of samples above threshold, last sweep
  bool  overload = false;       // last sweep had samples near the receiver's top end
  uint32_t sweeps = 0;          // since reset
  uint32_t sweepMs = 0;         // how long one sweep took
  uint32_t resetAt = 0;         // millis() at the last reset
  int8_t wf[WF_ROWS][NCH];      // waterfall: avg dBm per sweep, newest at wfHead-1
  int wfHead = 0;
};

struct Burst { uint32_t atMs; uint16_t lenMs; int8_t peakDbm; };
struct Listen {
  int ch = 64;
  float noise = -120;           // running noise estimate on this channel
  float now = -130;             // latest sample
  uint32_t bursts = 0, busyMs = 0, startedAt = 0;
  uint16_t hist[8] = {0};       // burst lengths: <2, <5, <10, <20, <50, <100, <250, >=250 ms
  Burst recent[32]; int recentN = 0, recentHead = 0;
  float trace[200]; int traceHead = 0;     // last 200 samples (~ every 5 ms), dBm
};

struct Gnss {
  bool present = false, fix = false;
  double lat = 0, lon = 0;
  float altM = 0, hdop = 99, speedKmh = 0, courseDeg = 0;
  int sats = 0, satsInView = 0;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  uint32_t lastSentenceMs = 0, sentences = 0, fixAtMs = 0;
  int baud = 0, rxPin = -1;
};

extern Settings   gSettings;
extern Spectrum * gSpec;        // in PSRAM
extern Listen   * gListen;
extern Gnss       gGnss;
extern std::mutex gMx;          // guards gSpec, gListen, gGnss

void  powerDown();              // radio, front-end and GNSS off (before deep sleep)
bool  begin();                  // powers the radio; false = no radio found (its error is in lastError())
const char * lastError();
bool  radioOk();
void  setMode(Mode m, int ch = -1);
Mode  mode();
void  resetStats();
void  loadSettings();
void  saveSettings();

// corrected input power at the antenna connector, dBm (radio reading - front-end gain + attenuator)
inline float corrected(float rawDbm) { return rawDbm - gSettings.frontEndDb + gSettings.attenDb; }
// power density (mW/cm^2) of a plane wave that would put `dbm` into an antenna of gain `gDbi` at f MHz
float powerDensity(float dbm, float gDbi, float fMHz);
// FCC 47 CFR 1.1310 limits, mW/cm^2, 300-1500 MHz: general public f/1500, occupational f/300
inline float fccGeneral(float fMHz)      { return fMHz / 1500.0f; }
inline float fccOccupational(float fMHz) { return fMHz / 300.0f; }

// GNSS
bool gnssStart();               // looks for the module (once); true if NMEA is coming in
bool gnssFixed();

}  // namespace rf

bool gnssPresent();             // for core.cpp: GPIO 39-42 are taken by the GNSS module
