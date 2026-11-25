#include <Adafruit_BNO08x.h>
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "driver/temperature_sensor.h"
#include <stdarg.h>
#include <string.h>

#include "flight_math.h"

// commands
#include "commands.h"
#include "webapp.h"
#include "core_usage.h"
#include "notes.h"

#define BNO08X_RESET -1
Adafruit_BNO08x bno08x(BNO08X_RESET);
sh2_SensorValue_t sensorValue;

// ---------------- Hardware Pins ----------------
// --- Motor Pins
// !! VERIFY with the `motor fl|fr|rl|rr` command (PROPS OFF) before flying !!
// The old code comments and the web UI disagree with these names about which
// motor is where. Each name here must be the motor at that PHYSICAL corner:
// FL = front-left, FR = front-right, RL = rear-left, RR = rear-right.
// If `motor fl` spins some other corner, swap the pin numbers here.
#define FL_M 39
#define FR_M 38
#define RL_M 36
#define RR_M 37

// --- IMU I2C Pins
#define GYRO_SDA 9      //QWIIC - Blue
#define GYRO_SCL 10     //QWIIC - Yellow

// --- Status LED Pins
//#define R_LED = 5;
//#define G_LED = 6;
//#define B_LED = 7;
// ---------------- Hardware Pins ----------------

// ---------------- Airframe / IMU mounting ----------------
// Body convention used by the controller:
//   +roll  = RIGHT side down       +pitch = NOSE up       +yaw = nose turns right
// !! VERIFY with the `imu` command (PROPS OFF) before flying !!
//   1. Tilt the right side down -> roll must go POSITIVE.
//   2. Raise the nose           -> pitch must go POSITIVE.
//   If the wrong angle moves, set IMU_SWAP_ROLL_PITCH 1. If the right angle moves
//   the wrong way, flip its sign. The gyro rate is flipped together with the angle,
//   so the D term can never end up with the wrong sign on its own.
#define IMU_SWAP_ROLL_PITCH  0
#define IMU_ROLL_SIGN        1
#define IMU_PITCH_SIGN       1
#define IMU_YAW_SIGN         1
// Prop direction: 1 if the front-left & rear-right props spin CLOCKWISE seen from above.
#define FL_PROP_CW           1
// ---------------- Airframe / IMU mounting ----------------

// ---------------- Debugging ----------------
#define ENABLE_ROLL_CORRECTION      1
#define ENABLE_PITCH_CORRECTION     1
#define ENABLE_YAW_CORRECTION       0
#define DEBUG_PRINT                 1
#define ENABLE_SERIAL_CMDS          1
#define ENABLE_WEB                  1
#define ENABLE_MOTORS               1
// ---------------- Debugging ----------------

// ---------------- Timing ----------------
// 5 ms = 200 Hz, matching the IMU report rate. (Was 10 ms = 100 Hz despite the comment.)
const TickType_t periodTicks = pdMS_TO_TICKS(5);
#define DEBUG_PRINT_EVERY   10        // loops between debug prints (10 -> 20 Hz)
// ---------------- Timing ----------------

// ---------------- Motor output ----------------
struct MotorOutputs {
  int FL, FR, RL, RR;
};

// ---------------- PID gains ----------------
// Output units are PWM counts (1 count ~ 0.98 us at 250 Hz / 12 bit).
// kp: counts per degree of error, kd: counts per deg/s, ki: counts per deg*s.
fm::PIDGains rollGains  = {1.6f, 1.0f, 0.35f};
fm::PIDGains pitchGains = {1.6f, 1.0f, 0.35f};
fm::PIDGains yawGains   = {0.6f, 0.0f, 0.0f};
// Board-mount misalignment, degrees. Use the `level` command with the frame sitting
// level to measure these, then paste the printed values here.
#define ROLL_TRIM     0.0f
#define PITCH_TRIM    0.0f

// ---------------- PID persistent state ----------------
// IMPORTANT: yaw gets a much smaller output ceiling than roll/pitch.
// Yaw is a rotational nudge; roll/pitch are what actually keep you level
// and need the majority of the available PWM headroom.
//                       integral, |I| limit, |out| limit   (PWM counts)
fm::PIDState rollState  = {0, 60, 250};
fm::PIDState pitchState = {0, 60, 250};
fm::PIDState yawState   = {0, 50, 120};

// ---------------- Orientation state (body frame, written by flight task only) ----------------
volatile float currentRoll = 0, currentPitch = 0, currentYaw = 0;
volatile float gyroRollRate = 0, gyroPitchRate = 0, gyroYawRate = 0;
float yawHold = 0;
float rollTrim = ROLL_TRIM, pitchTrim = PITCH_TRIM;
volatile bool     imuOk = false;          // BNO085 found and reports enabled
volatile uint32_t imuLastUs = 0;          // micros() of the last attitude sample
volatile uint32_t imuResets = 0;

// ---------------- Shared control state (written by CLI / web, read by flight task) ----------------
volatile bool armed = false;              // only the flight task sets this true
volatile bool armRequest = false;
const char * volatile armRefusal = "";
volatile bool debug = false;
volatile int  throttle_pwm = 1475;        // hover-ish baseline, tune for your build
volatile int8_t   motorTestIdx = -1;      // -1 = off, otherwise fm::M_FL..M_RR
volatile uint32_t motorTestUntilMs = 0;
volatile float lastMix[4] = {0, 0, 0, 0}; // what the mixer wants, even while disarmed

// ---------------- Motor Control ----------------
#define PWM_FREQUENCY       250   // Hz
#define PWM_RESOLUTION      12    // bits  ->  0 - 4095, one count = 1e6/250/4096 = 0.977 us
#define COUNTS_TO_US(c)     ((c) * (1000000.0f / PWM_FREQUENCY / 4096.0f))

#define MIN_PWM           1138    // ~1111 us: ESC "zero throttle" (used to arm ESCs, and while disarmed)
#define MAX_PWM           2512    // ~2453 us: NOTE most ESCs saturate at 2000 us (2048 counts) unless
                                  // calibrated to this range; anything above that is dead travel.
#define OFF_PWM           256     // ~250 us, invalid pulse used only during boot
#define ARMED_IDLE_PWM    (MIN_PWM + 40)  // lowest output while armed, so motors never stall mid-air
#define MOTOR_TEST_PWM    (MIN_PWM + 80)  // gentle spin for `motor` command
#define MOTOR_TEST_MS     1500

// ---------------- Safety ----------------
#define IMU_TIMEOUT_US     100000  // no attitude for 100 ms while armed -> disarm
#define TILT_CUTOFF_DEG    60.0f   // tilted beyond this while armed -> disarm (crash/flip)
#define ARM_MAX_TILT_DEG   20.0f   // refuse to arm unless roughly level
#define THROTTLE_SLEW      400.0f  // counts/s: arming ramps from idle to throttle_pwm (~1 s)

// ---------------- Threading ----------------
TaskHandle_t FlightThread =       NULL;
TaskHandle_t CLIThread =          NULL;
TaskHandle_t WEBThread =          NULL;
TaskHandle_t MonitoringThread =   NULL;



// ---------------- loggin stuff ----------------
// --- Print
constexpr size_t LOG_MSG_LEN   = 128;  // max chars per message
constexpr size_t LOG_QUEUE_LEN = 64;   // messages buffered (~8 KB total)
constexpr size_t CMD_LINE_LEN  = 64;
static_assert(LOG_MSG_LEN == FLIGHT_MSG_LEN, "webapp.h FLIGHT_MSG_LEN must match LOG_MSG_LEN");

struct LogMsg { char text[LOG_MSG_LEN]; };

static QueueHandle_t logQueueCMD = NULL;
static QueueHandle_t logQueueWEB = NULL;
static uint32_t droppedLogsCMD = 0;
static uint32_t droppedLogsWEB = 0;
#define CMD_MAX 128

temperature_sensor_handle_t temp_handle = NULL;
temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 70);
// ---------------- ^ Variable Settings ^ ----------------

// ---------------- Logging ----------------
// always=false: telemetry, only printed on serial when `debug on`.
// always=true : events (arm/disarm/failsafe), always printed.
static void vlog(bool always, const char *fmt, va_list args) {
  LogMsg msg;
  vsnprintf(msg.text, sizeof(msg.text), fmt, args);

  // Timeout of 0 = never block the caller; drop the message if the queue is full
  if ((debug || always) && logQueueCMD) {
    if (xQueueSend(logQueueCMD, &msg, 0) != pdTRUE) {
      __atomic_add_fetch(&droppedLogsCMD, 1, __ATOMIC_RELAXED);
    }
  }
  if (logQueueWEB) {
    if (xQueueSend(logQueueWEB, &msg, 0) != pdTRUE) {
      __atomic_add_fetch(&droppedLogsWEB, 1, __ATOMIC_RELAXED);
    }
  }
}
void logf(const char *fmt, ...)      { va_list a; va_start(a, fmt); vlog(false, fmt, a); va_end(a); }
void logEvent(const char *fmt, ...)  { va_list a; va_start(a, fmt); vlog(true,  fmt, a); va_end(a); }
// ---------------- Logging ----------------


// ---------------- BNO085 setup ----------------
static bool enableIMUReports() {
  // GAME rotation vector = gyro + accel only. The plain rotation vector also fuses
  // the magnetometer, which motor currents and the battery leads disturb; on a
  // drone that shows up as attitude jumps.
  bool ok = bno08x.enableReport(SH2_GAME_ROTATION_VECTOR, 5000);   // 5 ms = 200 Hz
  ok &= bno08x.enableReport(SH2_GYROSCOPE_CALIBRATED, 5000);
  return ok;
}

void setupIMU() {
  Wire.begin(GYRO_SDA, GYRO_SCL);
  Wire.setClock(400000);   // default 100 kHz makes each BNO085 read take several ms
  if (!bno08x.begin_I2C(0x4A, &Wire)) {
    Serial.println("!! BNO085 not found on I2C - arming is disabled !!");
    imuOk = false;
    return;
  }
  imuOk = enableIMUReports();
  if (!imuOk) Serial.println("!! BNO085 report enable failed - arming is disabled !!");
  bno08x.wasReset();   // clear the power-on reset flag so readIMU() only reports real resets
}
// ---------------- BNO085 setup ----------------

// ---------------- ESC setup ----------------
static void writeMotors(int fl, int fr, int rl, int rr) {
#if ENABLE_MOTORS
  ledcWrite(FL_M, fl);
  ledcWrite(FR_M, fr);
  ledcWrite(RL_M, rl);
  ledcWrite(RR_M, rr);
#endif
}

void setupESC() {
  ledcAttach(FL_M, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttach(FR_M, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttach(RL_M, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttach(RR_M, PWM_FREQUENCY, PWM_RESOLUTION);

  writeMotors(OFF_PWM, OFF_PWM, OFF_PWM, OFF_PWM);
  vTaskDelay(pdMS_TO_TICKS(250));

  Serial.println("Arming ESCs — keep propellers clear!");
  writeMotors(MIN_PWM, MIN_PWM, MIN_PWM, MIN_PWM);
  vTaskDelay(pdMS_TO_TICKS(2500));
  Serial.println("ESCs armed!");
}
// ---------------- ESC setup ----------------


// ---------------- Pull latest sensor data ----------------
// Converts sensor-frame readings to the body convention described at the top.
static void storeAttitude(float sRoll, float sPitch, float sYaw) {
#if IMU_SWAP_ROLL_PITCH
  float r = sPitch, p = sRoll;
#else
  float r = sRoll, p = sPitch;
#endif
  currentRoll  = IMU_ROLL_SIGN  * r - rollTrim;
  currentPitch = IMU_PITCH_SIGN * p - pitchTrim;
  currentYaw   = IMU_YAW_SIGN   * sYaw;
}

static void storeRates(float gx, float gy, float gz) {   // rad/s in, deg/s out
  const float R2D = 57.2957795f;
#if IMU_SWAP_ROLL_PITCH
  float r = gy, p = gx;
#else
  float r = gx, p = gy;
#endif
  gyroRollRate  = IMU_ROLL_SIGN  * r * R2D;
  gyroPitchRate = IMU_PITCH_SIGN * p * R2D;
  gyroYawRate   = IMU_YAW_SIGN   * gz * R2D;
}

// Returns true if orientation was updated this call.
bool readIMU() {
  if (!imuOk) return false;

  // The BNO085 can reset itself (brown-out when motors spin up, I2C glitch). After a
  // reset ALL reports are off, so attitude silently freezes unless we re-enable them.
  if (bno08x.wasReset()) {
    imuResets++;
    enableIMUReports();
    logEvent("[imu] BNO085 reset detected (#%u), reports re-enabled", (unsigned)imuResets);
  }

  bool updated = false;
  while (bno08x.getSensorEvent(&sensorValue)) {
    if (sensorValue.sensorId == SH2_GAME_ROTATION_VECTOR) {
      float r, p, y;
      fm::quatToEuler(sensorValue.un.gameRotationVector.real,
                      sensorValue.un.gameRotationVector.i,
                      sensorValue.un.gameRotationVector.j,
                      sensorValue.un.gameRotationVector.k, r, p, y);
      if (isfinite(r) && isfinite(p) && isfinite(y)) {
        storeAttitude(r, p, y);
        imuLastUs = micros();
        updated = true;
      }
    } else if (sensorValue.sensorId == SH2_GYROSCOPE_CALIBRATED) {
      storeRates(sensorValue.un.gyroscope.x, sensorValue.un.gyroscope.y,
                 sensorValue.un.gyroscope.z);
    }
  }
  return updated;
}
// ---------------- Pull latest sensor data ----------------

static void resetControllers() {
  rollState.integral = pitchState.integral = yawState.integral = 0;
  yawHold = currentYaw;
}

// ---------------- Arming (shared with web) ----------------
// Called from CLI/web. The flight task does the actual checks and state reset, so
// nothing else ever touches the PID state while it is running.
bool requestArm() {
  if (armed) return true;
  armRefusal = "";
  armRequest = true;
  for (int i = 0; i < 40 && armRequest; i++) vTaskDelay(pdMS_TO_TICKS(5));
  if (armRequest) { armRequest = false; armRefusal = "flight task did not respond"; }
  return armed;
}
const char *lastArmRefusal() { return armRefusal; }

void requestDisarm(const char *why) {
  bool was = armed;
  armed = false;
  if (was) logEvent("[safety] DISARMED: %s", why);
}

// ---------------- External Control ----------------
static void cmdThrottle(int argc, char **argv) {
  if (argc < 2) { Serial.printf("throttle is %d (range %d..%d)\n", throttle_pwm, MIN_PWM, MAX_PWM); return; }
  int t = throttle_pwm;
  if (strcasecmp(argv[1], "up") == 0)        t += 5;
  else if (strcasecmp(argv[1], "down") == 0) t -= 5;
  else if (strcasecmp(argv[1], "set") == 0 && argc >= 3) {
    char *end;
    long v = strtol(argv[2], &end, 10);
    if (*end != '\0') { Serial.printf("Invalid number: %s\n", argv[2]); return; }
    t = (int)v;
  }
  else { Serial.println("usage: throttle [up|down|set <counts>]"); return; }
  throttle_pwm = constrain(t, MIN_PWM, MAX_PWM);
  Serial.printf("Throttle changed to %d (%.0f us)\n", throttle_pwm, COUNTS_TO_US(throttle_pwm));
}
static void cmdESC(int argc, char **argv) {
  if (argc < 2) { Serial.printf("ESCs are %s\n", armed ? "ARMED" : "disarmed"); return; }
  if (strcasecmp(argv[1], "arm") == 0) {
    if (requestArm()) Serial.println("ESCs ARMED");
    else Serial.printf("Arm refused: %s\n", lastArmRefusal());
  } else if (strcasecmp(argv[1], "disarm") == 0) {
    requestDisarm("command");
    Serial.println("ESCs disarmed");
  } else Serial.println("usage: esc [arm|disarm]");
}
static void cmdDebug(int argc, char **argv) {
  if (argc < 2) { Serial.println("usage: debug [on|off]"); return; }
  if (strcasecmp(argv[1], "on") == 0) debug = true;
  else if (strcasecmp(argv[1], "off") == 0) debug = false;
  else Serial.printf("Invalid input: %s\n", argv[1]);
}
static void cmdImu(int, char **) {
  uint32_t age = micros() - imuLastUs;
  Serial.printf("IMU %s, last sample %lu ms ago, resets %u\n",
                imuOk ? "OK" : "NOT AVAILABLE", (unsigned long)(age / 1000), (unsigned)imuResets);
  Serial.printf("roll %+7.2f  pitch %+7.2f  yaw %+7.2f  (deg; +roll = right side down, +pitch = nose up)\n",
                currentRoll, currentPitch, currentYaw);
  Serial.printf("rate roll %+7.1f  pitch %+7.1f  yaw %+7.1f  (deg/s)\n",
                gyroRollRate, gyroPitchRate, gyroYawRate);
  Serial.printf("mixer FL %4.0f  FR %4.0f  RL %4.0f  RR %4.0f  (the LOW side should be higher)\n",
                lastMix[0], lastMix[1], lastMix[2], lastMix[3]);
}
static void cmdMotor(int argc, char **argv) {
  static const char *names[4] = {"fl", "fr", "rl", "rr"};
  if (argc < 2) { Serial.println("usage: motor [fl|fr|rl|rr]   (PROPS OFF! spins one motor briefly)"); return; }
  if (armed) { Serial.println("Disarm first"); return; }
  for (int i = 0; i < 4; i++) {
    if (strcasecmp(argv[1], names[i]) == 0) {
      motorTestUntilMs = millis() + MOTOR_TEST_MS;
      motorTestIdx = i;
      Serial.printf("Spinning %s for %d ms - it must be the %s motor\n", names[i], MOTOR_TEST_MS,
                    i == 0 ? "FRONT-LEFT" : i == 1 ? "FRONT-RIGHT" : i == 2 ? "REAR-LEFT" : "REAR-RIGHT");
      return;
    }
  }
  Serial.printf("Unknown motor '%s'\n", argv[1]);
}
static void cmdLevel(int, char **) {
  if (armed) { Serial.println("Disarm first"); return; }
  // currentRoll/Pitch already have the old trim subtracted.
  rollTrim  += currentRoll;
  pitchTrim += currentPitch;
  Serial.printf("Level captured. To make it permanent set:\n#define ROLL_TRIM  %.2ff\n#define PITCH_TRIM %.2ff\n",
                rollTrim, pitchTrim);
}
// The sensor is enabled once in setup(). Enabling/disabling it on every read raced
// between the web task and the CLI task (one could disable it mid-read of the other).
static int getTemp() {
  float tsens_out = 0;
  if (!temp_handle || temperature_sensor_get_celsius(temp_handle, &tsens_out) != ESP_OK) return 0;
  return (int)lroundf(tsens_out);
}
// ---------------- External Control ----------------

// ---------------- Main entry point ----------------
// desiredX/Y/Z in range roughly [-1, 1] (e.g. from joystick/RC, normalized)
// maxTiltDeg caps how aggressively x/y get turned into lean angle.
// baseThrottle is the collective throttle (already ramped by the caller).
fm::Mix computeMotorOutputs(float desiredX, float desiredY, float baseThrottle,
                            float dt, bool integrate, float maxTiltDeg = 25.0f) {
  // Map desired movement -> target angles
  float targetRoll  = fm::clampf(desiredX, -1.0f, 1.0f) * maxTiltDeg;
  float targetPitch = fm::clampf(desiredY, -1.0f, 1.0f) * maxTiltDeg;

  float errorRoll  = targetRoll  - currentRoll;
  float errorPitch = targetPitch - currentPitch;
  float errorYaw   = fm::wrap180(yawHold - currentYaw);

  float rollOut  = ENABLE_ROLL_CORRECTION  ? fm::pidUpdate(errorRoll,  gyroRollRate,  dt, integrate, rollGains,  rollState)  : 0;
  float pitchOut = ENABLE_PITCH_CORRECTION ? fm::pidUpdate(errorPitch, gyroPitchRate, dt, integrate, pitchGains, pitchState) : 0;
  float yawOut   = ENABLE_YAW_CORRECTION   ? fm::pidUpdate(errorYaw,   gyroYawRate,   dt, integrate, yawGains,   yawState)   : 0;

  return fm::mixQuadX(baseThrottle, rollOut, pitchOut, yawOut, FL_PROP_CW,
                      ARMED_IDLE_PWM, MAX_PWM);
}
// ---------------- Main entry point ----------------



// ---------------- Thread Health Check ----------------
void checkTaskAlive(TaskHandle_t handle, const char* name) {
  if (handle == NULL) {
    Serial.printf("[%s] Handle is NULL — task was never created or was deleted\n", name);
    return;
  }

  eTaskState state = eTaskGetState(handle);
  const char* stateStr;
  switch (state) {
    case eRunning:   stateStr = "Running"; break;
    case eReady:     stateStr = "Ready"; break;
    case eBlocked:   stateStr = "Blocked"; break;
    case eSuspended: stateStr = "Suspended"; break;
    case eDeleted:   stateStr = "Deleted"; break;
    default:         stateStr = "Unknown"; break;
  }
  Serial.printf("[%s] State: %s\n", name, stateStr);
}
// ---------------- Thread Health Check ----------------



// ---------------- CommandLineInterface Thread ----------------
// Copies up to `max` queued flight messages into the caller's buffers.
// (The old version returned pointers into a local variable: every entry pointed at
// the same dead stack buffer.)
static int getFlightData(char out[][FLIGHT_MSG_LEN], int max){
  LogMsg msg;
  int index = 0;

  if (logQueueWEB && xQueueReceive(logQueueWEB, &msg, pdMS_TO_TICKS(10)) == pdTRUE){
    do {
      memcpy(out[index], msg.text, LOG_MSG_LEN);
      index++;
    } while (index < max && xQueueReceive(logQueueWEB, &msg, 0) == pdTRUE);
  }

  uint32_t dropped = __atomic_exchange_n(&droppedLogsWEB, 0, __ATOMIC_RELAXED);
  if (dropped && debug) Serial.printf("[web-logger] dropped %u messages\n", dropped);
  return index;
}

#if ENABLE_SERIAL_CMDS
void commandLineInterface(void *pvParameters) {
  LogMsg msg;
  char   line[CMD_LINE_LEN];
  size_t len = 0;
  bool   overflow = false;

  for (;;) {
    // --- Print to serial ---
    if (xQueueReceive(logQueueCMD, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
      Serial.println(msg.text);
      while (xQueueReceive(logQueueCMD, &msg, 0) == pdTRUE) {
        Serial.println(msg.text);
      }
    }

    // --- Count messages not sent ---
    uint32_t dropped = __atomic_exchange_n(&droppedLogsCMD, 0, __ATOMIC_RELAXED);
    if (dropped) Serial.printf("[cmd-logger] dropped %u messages\n", dropped);

    // --- Handle incoming serial data ---
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\r' || c == '\n') {
        if (overflow) {
          Serial.print("[error] line too long, discarded\n");
        } else if (len > 0) {
          line[len] = '\0';
          dispatchCommand(line);
        }
        len = 0;
        overflow = false;
      } else if (c == 0x08 || c == 0x7F) {          // backspace / delete
        if (len > 0) len--;
      } else if (isprint((unsigned char)c)) {
        if (len < CMD_LINE_LEN - 1) line[len++] = c;
        else overflow = true;
      }
    }
  }
}
#endif
// ---------------- CommandLineInterface Thread ----------------





// ---------------- FlightController Thread ----------------
void flightController(void *pvParameters) {
  float desiredX = 0.0f; // lateral
  float desiredY = 0.0f; // forward/back
  float desiredZ = 0.0f; // vertical
  const float maxClimbPWM = 300.0f;

  float baseThrottle = ARMED_IDLE_PWM;
  int counter = 0;
  uint32_t lastUs = micros();

  // Initialise ONCE. Resetting it every iteration (as before) turns
  // vTaskDelayUntil into a plain delay, so the period became 10 ms + work time.
  TickType_t lastWakeTime = xTaskGetTickCount();

  for(;;){
    vTaskDelayUntil(&lastWakeTime, periodTicks);

    uint32_t now = micros();
    float dt = fm::clampf((now - lastUs) * 1e-6f, 0.0005f, 0.02f);
    lastUs = now;

    readIMU();
    bool imuFresh = imuOk && (micros() - imuLastUs) < IMU_TIMEOUT_US;

    // --- Arm request from CLI / web ---
    if (armRequest) {
      if (!imuFresh)                                     armRefusal = "IMU not reporting";
      else if (fabsf(currentRoll)  > ARM_MAX_TILT_DEG ||
               fabsf(currentPitch) > ARM_MAX_TILT_DEG)   armRefusal = "not level";
      else {
        motorTestIdx = -1;
        resetControllers();
        baseThrottle = ARMED_IDLE_PWM;
        armed = true;
        logEvent("[safety] ARMED");
      }
      armRequest = false;
    }

    // --- Failsafes ---
    if (armed && !imuFresh) requestDisarm("IMU data stale");
    if (armed && (fabsf(currentRoll) > TILT_CUTOFF_DEG || fabsf(currentPitch) > TILT_CUTOFF_DEG))
      requestDisarm("tilt limit exceeded");

    // --- Throttle: ramp up after arming instead of jumping straight to hover ---
    float targetThrottle = throttle_pwm + fm::clampf(desiredZ, -1.0f, 1.0f) * maxClimbPWM;
    bool isArmed = armed;
    if (isArmed) baseThrottle = fm::approach(baseThrottle, targetThrottle, THROTTLE_SLEW * dt);
    else {
      baseThrottle = ARMED_IDLE_PWM;
      resetControllers();
    }
    // Only integrate once the throttle ramp is finished, so the I term does not wind
    // up while the drone is still sitting on the ground.
    bool integrate = isArmed && baseThrottle >= targetThrottle - 1.0f;

    // Mixer runs even while disarmed (at the target throttle) so `imu` can show
    // what it would do when you tilt the frame by hand.
    fm::Mix mix = computeMotorOutputs(desiredX, desiredY,
                                      isArmed ? baseThrottle : targetThrottle, dt, integrate);
    for (int i = 0; i < 4; i++) lastMix[i] = mix.m[i];

    MotorOutputs m;
    if (isArmed) {
      m = { (int)mix.m[fm::M_FL], (int)mix.m[fm::M_FR], (int)mix.m[fm::M_RL], (int)mix.m[fm::M_RR] };
    } else {
      // Disarmed: keep sending a valid zero-throttle pulse. The old code sent duty 0
      // (no pulse at all). Most ESCs treat that as signal loss and then refuse to
      // re-arm when the signal comes back at hover throttle.
      m = { MIN_PWM, MIN_PWM, MIN_PWM, MIN_PWM };
      int8_t t = motorTestIdx;
      if (t >= 0) {
        if ((int32_t)(millis() - motorTestUntilMs) < 0) {
          int *slot[4] = { &m.FL, &m.FR, &m.RL, &m.RR };
          *slot[t] = MOTOR_TEST_PWM;
        } else motorTestIdx = -1;
      }
    }
    writeMotors(m.FL, m.FR, m.RL, m.RR);

  #if DEBUG_PRINT
    if (++counter >= DEBUG_PRINT_EVERY) {
      counter = 0;
      // Format expected by the web UI: "Orientation: heading,roll,pitch" and
      // "Motors: FL,FR,RL,RR" in microseconds. (Previously pitch and roll were
      // sent swapped and motors were sent as raw duty counts.)
      logf("Orientation: %.2f, %.2f, %.2f", currentYaw, currentRoll, currentPitch);
      logf("Motors: %.0f, %.0f, %.0f, %.0f", COUNTS_TO_US(m.FL), COUNTS_TO_US(m.FR),
           COUNTS_TO_US(m.RL), COUNTS_TO_US(m.RR));
    }
  #endif
  }
}
// ---------------- FlightController Thread ----------------





// ---------------- System Management ----------------
void setup() {
  Serial.begin(115200);
  setupESC();     // ESC signal first, so the ESCs see a valid pulse as early as possible
  setupIMU();
  if (temperature_sensor_install(&temp_sensor_config, &temp_handle) == ESP_OK)
    temperature_sensor_enable(temp_handle);
  else temp_handle = NULL;

  // Notify user drone is ready:
  setupTune();

  // Create the queues before any task that uses them.
  logQueueCMD = xQueueCreate(LOG_QUEUE_LEN, sizeof(LogMsg));
  #if ENABLE_WEB
    logQueueWEB = xQueueCreate(LOG_QUEUE_LEN, sizeof(LogMsg));
    xTaskCreate(
      webApplication,               // task function
      "WebApplication",             // name
      6144,                         // stack size (bytes) — increase if stack overflow occurs
      NULL,                         // parameter
      5,                            // priority (higher = more urgent; was 1)
      &WEBThread                    // handle
    );
  #endif

  // Serial Thread
  #if ENABLE_SERIAL_CMDS
    xTaskCreate(
      commandLineInterface,         // task function
      "CommandLineInterface",       // name
      4096,                         // stack size (bytes) — increase if stack overflow occurs
      NULL,                         // parameter
      5,                            // priority (higher = more urgent; was 1)
      &CLIThread                    // handle
    );
  #endif

  // Thread Monitoring Thread
  xTaskCreate(
    monitor_task,                 // task function
    "MonitoringTask",             // name
    4096,                         // stack size (bytes) — increase if stack overflow occurs
    NULL,                         // parameter
    1,                            // priority (higher = more urgent; was 1)
    &MonitoringThread             // handle
  );

  // Flight Thread
  xTaskCreatePinnedToCore(
    flightController,               // task function
    "FlightCtrl",                   // name
    8192,                           // stack size (bytes) — increase if stack overflow occurs
    NULL,                           // parameter
    24,                             // priority (higher = more urgent; was 1)
    &FlightThread,                  // handle
    1                               // core
  );


}

void loop() {
  vTaskDelay(2500); // 2.5 Seconds
  //checkTaskAlive(CLIThread, " [CLI] ");
  //checkTaskAlive(FlightThread, " [Flight] ");
  // --- Maybe add LED stuff here? ---
}
// ---------------- System Management ----------------
