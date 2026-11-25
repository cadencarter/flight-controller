// Host-side tests for src/flight_controller_threaded/flight_math.h
//   g++ -std=c++17 -O2 -I src/flight_controller_threaded test/flight_math_test.cpp -o /tmp/fmt && /tmp/fmt
//
// 1. Unit checks: quaternion->Euler, mixer torque directions, desaturation, PID.
// 2. Closed-loop sim of a generic ~600 g quad (rigid body + motor lag + sensor delay),
//    run with the ORIGINAL mixer and the fixed one. Airframe numbers are guesses, so
//    this only shows qualitative behaviour (does each axis get corrected at all).
#include <cstdio>
#include <cmath>
#include <deque>
#include "flight_math.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// Body torques from 4 motor commands (FL, FR, RL, RR), FL prop clockwise.
// roll + = right side down, pitch + = nose up, yaw + = nose right.
static void torques(const float m[4], float &r, float &p, float &y) {
  r = (m[fm::M_FL] + m[fm::M_RL]) - (m[fm::M_FR] + m[fm::M_RR]);
  p = (m[fm::M_FL] + m[fm::M_FR]) - (m[fm::M_RL] + m[fm::M_RR]);
  y = (m[fm::M_FR] + m[fm::M_RL]) - (m[fm::M_FL] + m[fm::M_RR]);   // CCW props push nose right
}

static void unitTests() {
  // --- quatToEuler: 10 deg about X, 10 deg about Y, identity, and an out-of-range quaternion
  float r, p, y;
  float h = 10.0f * (float)M_PI / 360.0f;
  fm::quatToEuler(cosf(h), sinf(h), 0, 0, r, p, y);
  CHECK(fabsf(r - 10) < 0.01f && fabsf(p) < 0.01f, "quat X: roll %.3f pitch %.3f", r, p);
  fm::quatToEuler(cosf(h), 0, sinf(h), 0, r, p, y);
  CHECK(fabsf(p - 10) < 0.01f && fabsf(r) < 0.01f, "quat Y: roll %.3f pitch %.3f", r, p);
  fm::quatToEuler(0.7072f, 0, 0.7072f, 0, r, p, y);   // |q| slightly > 1, gimbal edge
  CHECK(std::isfinite(p), "quat clamp: pitch is NaN");

  // --- mixer: each axis command must produce torque on THAT axis only
  const char *axis[3] = {"roll", "pitch", "yaw"};
  for (int a = 0; a < 3; a++) {
    float in[3] = {0, 0, 0}; in[a] = 50;
    fm::Mix mx = fm::mixQuadX(1500, in[0], in[1], in[2], true, 1178, 2512);
    float t[3]; torques(mx.m, t[0], t[1], t[2]);
    for (int b = 0; b < 3; b++) {
      if (a == b) CHECK(t[b] > 0, "mixer %s cmd gives %s torque %.1f (want >0)", axis[a], axis[b], t[b]);
      else        CHECK(fabsf(t[b]) < 1e-3f, "mixer %s cmd leaks into %s (%.1f)", axis[a], axis[b], t[b]);
    }
  }

  // --- desaturation: near idle, the roll differential must survive (old code clipped it away)
  fm::Mix lo = fm::mixQuadX(1190, 100, 0, 0, true, 1178, 2512);
  float tr, tp, ty; torques(lo.m, tr, tp, ty);
  CHECK(fabsf(tr - 400) < 1, "desat low: roll torque %.1f, want 400", tr);
  for (int i = 0; i < 4; i++) CHECK(lo.m[i] >= 1178 && lo.m[i] <= 2512, "desat low: motor %d out of range", i);
  fm::Mix nan = fm::mixQuadX(1500, NAN, 0, 0, true, 1178, 2512);
  for (int i = 0; i < 4; i++) CHECK(nan.m[i] == 1178, "NaN input must give idle");

  // --- PID: I term is bounded by iLimit; no integration when told not to
  fm::PIDGains g = {1, 0.5f, 0};
  fm::PIDState s = {0, 60, 250};
  for (int i = 0; i < 100000; i++) fm::pidUpdate(10, 0, 0.005f, true, g, s);
  CHECK(fabsf(g.ki * s.integral - 60) < 0.01f, "I term %.2f, want 60", g.ki * s.integral);
  fm::PIDState s2 = {0, 60, 250};
  fm::pidUpdate(10, 0, 0.005f, false, g, s2);
  CHECK(s2.integral == 0, "integrated while integrate=false");
}

// ---------------- closed-loop sim ----------------
struct Sim {
  // Generic ~600 g X quad. Thrust linear in counts above 1138; hover ~1475.
  const float kT = 1.47f / (1475 - 1138);  // N per count per motor
  const float l = 0.08f;                    // m, motor to roll/pitch axis
  const float kQ = 0.012f;                  // N*m per N of thrust (prop drag)
  const float I = 0.004f, Iz = 0.007f;      // kg m^2
  const float tau = 0.03f;                  // s, motor time constant
  const float distRoll = 0.01f, distPitch = 0.01f;   // N*m: CG offset
  float ang[3] = {0, 0, 0}, rate[3] = {0, 0, 0};      // rad, rad/s (roll, pitch, yaw)
  float thrust[4] = {0, 0, 0, 0};

  void step(const int cmd[4], float dt) {
    for (int i = 0; i < 4; i++) {
      float tgt = (cmd[i] - 1138) * kT;
      thrust[i] += (tgt - thrust[i]) * (dt / tau);
    }
    float r, p, y; torques(thrust, r, p, y);
    float acc[3] = {(r * l + distRoll) / I, (p * l + distPitch) / I, y * kQ / Iz};
    for (int a = 0; a < 3; a++) { rate[a] += acc[a] * dt; ang[a] += rate[a] * dt; }
  }
};

static void runSim(bool fixedMixer) {
  const float R2D = 57.2957795f, dt = 0.005f;
  Sim sim;
  sim.ang[0] = 10 / R2D; sim.ang[1] = -10 / R2D;     // start 10 deg off on both axes
  fm::PIDGains gR = {1.6f, fixedMixer ? 1.0f : 0.02f, 0.35f}, gP = gR;
  fm::PIDState sR = {0, 60, 250}, sP = {0, 60, 250};
  float oldIntR = 0, oldIntP = 0;
  std::deque<float> delay;                            // ~10 ms sensor latency
  float maxAbs[2] = {0, 0};
  bool tipped = false;
  for (int k = 0; k < 600; k++) {                     // 3 s
    float meas[4] = {sim.ang[0] * R2D, sim.ang[1] * R2D, sim.rate[0] * R2D, sim.rate[1] * R2D};
    for (float v : meas) delay.push_back(v);
    if (delay.size() > 8) for (int i = 0; i < 4; i++) delay.pop_front();
    float roll = delay[0], pitch = delay[1], rr = delay[2], pr = delay[3];
    int cmd[4];
    if (fixedMixer) {
      float ro = fm::pidUpdate(-roll, rr, dt, true, gR, sR);
      float po = fm::pidUpdate(-pitch, pr, dt, true, gP, sP);
      fm::Mix m = fm::mixQuadX(1475, ro, po, 0, true, 1178, 2512);
      for (int i = 0; i < 4; i++) cmd[i] = (int)m.m[i];
    } else {
      // The original code, verbatim in behaviour (incl. the clamp-then-add integral).
      auto oldPid = [&](float e, float rate, float &integ, fm::PIDGains &g) {
        integ = e * dt + fm::clampf(integ, -25, 25);
        integ = fm::clampf(integ, -75, 75);
        return fm::clampf(g.kp * e + g.ki * integ - g.kd * rate, -250, 250);
      };
      float ro = oldPid(-roll, rr, oldIntR, gR), po = oldPid(-pitch, pr, oldIntP, gP);
      float T = 1475;
      float fl = T - po + ro, fr = T + po + ro, rl = T + po - ro, rrm = T - po - ro;
      // Pin names taken at face value (fl variable = front-left motor).
      cmd[fm::M_FL] = (int)fm::clampf(fl, 1138, 2512); cmd[fm::M_FR] = (int)fm::clampf(fr, 1138, 2512);
      cmd[fm::M_RL] = (int)fm::clampf(rl, 1138, 2512); cmd[fm::M_RR] = (int)fm::clampf(rrm, 1138, 2512);
    }
    sim.step(cmd, dt);
    if (k > 300) for (int a = 0; a < 2; a++) maxAbs[a] = fmaxf(maxAbs[a], fabsf(sim.ang[a] * R2D));
    if (fabsf(sim.ang[0]) > 1.2f || fabsf(sim.ang[1]) > 1.2f) { tipped = true; break; }
  }
  printf("%-14s roll %+8.2f deg  pitch %+8.2f deg  yaw rate %+8.1f deg/s  %s\n",
         fixedMixer ? "fixed mixer:" : "original code:", sim.ang[0] * R2D, sim.ang[1] * R2D,
         sim.rate[2] * R2D, tipped ? "(FLIPPED)" : "");
  if (fixedMixer) {
    CHECK(!tipped && fabsf(sim.ang[0] * R2D) < 1.0f && fabsf(sim.ang[1] * R2D) < 1.0f,
          "fixed mixer did not converge to within 1 deg (peak in last 1.5 s: roll %.2f pitch %.2f)", maxAbs[0], maxAbs[1]);
  }
}

int main() {
  unitTests();
  printf("Closed-loop sim, 3 s, start at roll +10 / pitch -10 deg, CG offset disturbance:\n");
  runSim(false);
  runSim(true);
  printf(failures ? "\n%d FAILURE(S)\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
