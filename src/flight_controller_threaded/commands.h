#pragma once
#include <Arduino.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "mon_struts.h"
#include "esp_heap_caps.h"

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

static void cmdStatus(int, char **) {
  Serial.println("-----------------------");
  Serial.printf("uptime: %lu s\n", (unsigned long)(millis() / 1000));
  // free: now. min: lowest ever since boot. largest: biggest single block that can
  // still be allocated (shrinks with fragmentation even when free stays flat).
  Serial.printf("heap free: %u  min: %u  largest block: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  Serial.printf("temp: %d C\n", getTemp());
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
