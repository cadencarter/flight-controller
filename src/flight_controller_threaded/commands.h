#pragma once
#include <Arduino.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "mon_struts.h"

constexpr int MAX_ARGS = 8;

// --- Functions in main.ino --- //
static void cmdThrottle(int argc, char **argv);
static void cmdESC(int argc, char **argv);
static void cmdDebug(int argc, char **argv);
static void cmdImu(int argc, char **argv);
static void cmdMotor(int argc, char **argv);
static void cmdLevel(int argc, char **argv);
static int getTemp();
// ----------------------------- //

static void cmdHelp(int argc, char **argv);

static void cmdStatus(int, char ** argv) {
  String uptime = "uptime: " + String(millis()/1000);
  String mem = "free heap: " + String(ESP.getFreeHeap());
  String temp = "temp: " + String(getTemp());

  Serial.println("-----------------------");
  Serial.println(uptime + " s");
  Serial.println(mem + " bytes");
  Serial.println(temp + " C");
}

extern volatile bool armed;
static void cmdReboot(int, char **) {
  if (armed) { Serial.print("disarm before rebooting\n"); return; }
  Serial.print("rebooting...\n");
  Serial.flush();               // wait for the UART to finish sending
  ESP.restart();
}

static void cmdEcho(int argc, char **argv) {
  for (int i = 1; i < argc; i++) Serial.printf("%s ", argv[i]);
  Serial.print("\n");
}

// ---------- Command table ----------
struct Command {
  const char *name;
  const char *options;
  const char *help;
  void (*fn)(int argc, char **argv);
};

static const Command commands[] = {
  {"help",      "",               "list commands",             cmdHelp},
  {"status",    "",               "show system status",        cmdStatus},
  {"reboot",    "",               "restart the chip",          cmdReboot},
  {"echo",      "[text]",         "echo text back",            cmdEcho},
  {"throttle",  "[up/down/set]",  "change throttle",           cmdThrottle},
  {"esc",       "[arm/disarm]",   "arm/disarm ESCs",           cmdESC},
  {"debug",     "[on/off]",       "enable/disable debugging",  cmdDebug},
  {"imu",       "",               "show attitude + mixer (bench check)", cmdImu},
  {"motor",     "[fl/fr/rl/rr]",  "spin one motor (PROPS OFF)", cmdMotor},
  {"level",     "",               "capture level trim",        cmdLevel},
};

static void cmdHelp(int, char **) {
  Serial.printf("\nusage: [command] [option] ...\n");
  for (const auto &c : commands) Serial.printf("  %-10s %-20s %s\n", c.name, c.options, c.help);
}

// ---------- Tokenize + dispatch (modifies `line` in place) ----------
static void dispatchCommand(char *line) {
  char *argv[MAX_ARGS];
  int argc = 0;
  char *save = nullptr;

  for (char *tok = strtok_r(line, " \t", &save);
       tok && argc < MAX_ARGS;
       tok = strtok_r(nullptr, " \t", &save)) {
    argv[argc++] = tok;
  }
  if (argc == 0) return;

  for (const auto &c : commands) {
    if (!strcasecmp(argv[0], c.name)) { c.fn(argc, argv); return; }
  }
  Serial.printf("unknown command '%s' (try 'help')\n", argv[0]);
}
