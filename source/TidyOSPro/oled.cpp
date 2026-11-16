// TidyOS Pro - the Heltec V4's little OLED: where to find the desktop, and the radio / GPS at a glance.
// Sleeps after 5 minutes (it's an OLED); a press of PRG wakes it for another 5.
#include "config.h"
#ifdef TIDYOS_V4
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <U8g2lib.h>
#include <mutex>
#include "rf_engine.h"
#include "net.h"
#include "screen.h"
#include "core.h"

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_oled(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);
static const uint32_t AWAKE_MS = 5UL * 60 * 1000;

static void oledTask(void *)
{
  uint32_t wokeAt = millis();
  bool asleep = false, prgWas = false;
  for (;;) {
    bool prg = digitalRead(PIN_PRG) == LOW;
    if (prg && !prgWas) { wokeAt = millis(); if (asleep) { s_oled.setPowerSave(0); asleep = false; } }
    prgWas = prg;
    if (!asleep && millis() - wokeAt > AWAKE_MS) { s_oled.setPowerSave(1); asleep = true; }
    if (!asleep) {
      char l[4][32];
      snprintf(l[0], 32, "TidyOS Pro %s", TIDYOS_VERSION);
      if (netConnected()) snprintf(l[1], 32, "%s", WiFi.localIP().toString().c_str());
      else snprintf(l[1], 32, "Wi-Fi %s", boardId().c_str());
      if (netConnected()) snprintf(l[2], 32, "%.21s.local", boardHost().c_str());
      else snprintf(l[2], 32, "pw %s  192.168.4.1", SCREEN_AP_PASS);
      {
        std::lock_guard<std::mutex> lk(rf::gMx);
        if (rf::gSpec && rf::mode() != rf::Mode::Off) snprintf(l[3], 32, "%.0fdBm %.1f%% %s", rf::gSpec->noiseFloor, rf::gSpec->occupancy * 100,
                                                            rf::gGnss.present ? (rf::gGnss.fix ? "GPS ok" : "GPS ..") : "");
        else snprintf(l[3], 32, "900MHz idle %s", rf::gGnss.present ? (rf::gGnss.fix ? "GPS ok" : "GPS ..") : "");
      }
      s_oled.clearBuffer();
      s_oled.setFont(u8g2_font_7x14B_tr); s_oled.drawStr(0, 12, l[0]);
      s_oled.setFont(u8g2_font_6x12_tr);
      s_oled.drawStr(0, 28, l[1]); s_oled.drawStr(0, 42, l[2]); s_oled.drawStr(0, 58, l[3]);
      s_oled.sendBuffer();
    }
    vTaskDelay(pdMS_TO_TICKS(asleep ? 100 : 1000));
  }
}

void oledBegin()
{
#if CONFIG_SPIRAM_MODE_OCT
  pinMode(PIN_VEXT_R8, OUTPUT); digitalWrite(PIN_VEXT_R8, LOW);
#else
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);       // screen power on
#endif
  delay(50);
  pinMode(PIN_PRG, INPUT_PULLUP);
  s_oled.begin();
  s_oled.setContrast(160);
  xTaskCreatePinnedToCore(oledTask, "oled", 3072, nullptr, 1, nullptr, 0);
}
#else
void oledBegin() {}
#endif
