#pragma once
// Pure control math: no Arduino / FreeRTOS dependencies, so it can be unit-tested
// on a PC (see test/flight_math_test.cpp).
#include <math.h>

namespace fm {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float wrap180(float a) {
  while (a > 180.0f)  a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

// Move `cur` toward `target` by at most `step`.
inline float approach(float cur, float target, float step) {
  if (cur < target) return (cur + step > target) ? target : cur + step;
  return (cur - step < target) ? target : cur - step;
}

// ---------------- Quaternion -> Euler (degrees, ZYX / aerospace order) ----------------
// roll  = rotation about sensor X, pitch = about sensor Y, yaw = about sensor Z.
inline void quatToEuler(float qr, float qi, float qj, float qk,
                        float &roll, float &pitch, float &yaw) {
  const float R2D = 57.2957795f;
  float sqr = qr * qr, sqi = qi * qi, sqj = qj * qj, sqk = qk * qk;
  float n = sqr + sqi + sqj + sqk;
  if (n < 1e-6f) n = 1.0f;
  roll  = atan2f(2.0f * (qj * qk + qi * qr), sqr - sqi - sqj + sqk) * R2D;
  // asin() of a value even slightly outside [-1, 1] is NaN, and NaN in the PID
  // turns into garbage motor commands. Clamp the argument.
  pitch = asinf(clampf(2.0f * (qr * qj - qi * qk) / n, -1.0f, 1.0f)) * R2D;
  yaw   = atan2f(2.0f * (qi * qj + qk * qr), sqr + sqi - sqj - sqk) * R2D;
}

// ---------------- PID ----------------
struct PIDGains { float kp, ki, kd; };

struct PIDState {
  float integral;   // accumulated error, deg*s
  float iLimit;     // max |I term|, in output units (PWM counts)
  float outLimit;   // max |output|, in output units (PWM counts)
};

// Angle-mode PID with derivative on the measured rate (no derivative kick).
// `integrate` should be false on the ground / while disarmed (anti-windup).
inline float pidUpdate(float error, float measuredRate, float dt, bool integrate,
                       const PIDGains &g, PIDState &s) {
  if (integrate && g.ki > 0.0f) {
    s.integral += error * dt;
    float iMax = s.iLimit / g.ki;
    s.integral = clampf(s.integral, -iMax, iMax);
  }
  float out = g.kp * error + g.ki * s.integral - g.kd * measuredRate;
  return clampf(out, -s.outLimit, s.outLimit);
}

// ---------------- Quad-X mixer ----------------
// Motor order everywhere: 0=FL, 1=FR, 2=RL, 3=RR  (PHYSICAL positions on the frame).
// Sign convention for the inputs (body frame):
//   roll  > 0 : torque that lowers the RIGHT side
//   pitch > 0 : torque that raises the NOSE
//   yaw   > 0 : torque that turns the nose RIGHT (clockwise seen from above)
// flPropCw: true if the front-left and rear-right props spin clockwise seen from above.
enum { M_FL = 0, M_FR = 1, M_RL = 2, M_RR = 3 };

struct Mix { float m[4]; };

inline Mix mixQuadX(float throttle, float roll, float pitch, float yaw,
                    bool flPropCw, float lo, float hi) {
  // A prop's reaction torque opposes its spin, so to yaw clockwise you speed up
  // the counter-clockwise props.
  float y = flPropCw ? yaw : -yaw;
  float c[4];
  c[M_FL] =  roll + pitch - y;
  c[M_FR] = -roll + pitch + y;
  c[M_RL] =  roll - pitch + y;
  c[M_RR] = -roll - pitch - y;

  Mix o;
  if (!isfinite(throttle) || !isfinite(roll) || !isfinite(pitch) || !isfinite(yaw)) {
    for (int i = 0; i < 4; i++) o.m[i] = lo;
    return o;
  }

  // Desaturation: keep the motor-to-motor DIFFERENCES (that is what holds attitude)
  // and give up collective throttle instead. Plain per-motor clamping silently
  // throws attitude authority away when a motor hits a limit.
  float cmax = c[0], cmin = c[0];
  for (int i = 1; i < 4; i++) { cmax = fmaxf(cmax, c[i]); cmin = fminf(cmin, c[i]); }
  float span = cmax - cmin, range = hi - lo;
  if (span > range && span > 0.0f) {
    float k = range / span;
    for (int i = 0; i < 4; i++) c[i] *= k;
    cmax *= k; cmin *= k;
  }
  float t = throttle;
  if (t + cmax > hi) t = hi - cmax;
  if (t + cmin < lo) t = lo - cmin;
  for (int i = 0; i < 4; i++) o.m[i] = clampf(t + c[i], lo, hi);
  return o;
}

}  // namespace fm
