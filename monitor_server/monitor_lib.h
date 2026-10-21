/*
 * MONITOR_LIB - объявления для мониторинга сервера (NodeMCU v3 / ESP8266)
 */

#ifndef MONITOR_LIB_H
#define MONITOR_LIB_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ===================== НАСТРОЙКИ =====================
extern const char* WIFI_SSID;             // имя Wi-Fi (задаётся в .ino)
extern const char* WIFI_PASS;             // пароль Wi-Fi (задаётся в .ino)
extern const char* TARGET;                // адрес сервера (задаётся в .ino)

const unsigned long PING_INTERVAL = 60UL * 1000UL; // раз в минуту
const unsigned long WIFI_RETRY_MS = 10UL * 1000UL; // повтор подключения к Wi-Fi
// ======================================================

enum State { ST_OK, ST_DOWN, ST_NOWIFI };
extern State state;
extern int   lastRtt;   // ms, -1 = нет ответа

void ledWrite(bool on);
void alertReset();
void alertTick();
void showState();
void setStatus(State s);
void doPing();
void monitorSetup();
void monitorLoop();

#endif
