# Pre-flight bench check (PROPS OFF)

The mixer and IMU mapping depend on how the board, IMU and motors are physically
wired. Three sources in the old code disagreed about which pin drives which motor,
so verify all of this before flying. It takes about 5 minutes over serial (115200).

1. **Motor positions:** run `motor fl`, `motor fr`, `motor rl`, `motor rr`.
   Each must spin the front-left, front-right, rear-left and rear-right motor
   respectively. If not, swap the `FL_M / FR_M / RL_M / RR_M` pin numbers.
2. **Prop direction:** while a motor spins, note its direction (seen from above).
   Front-left and rear-right should match each other, and the other two the
   opposite. Set `FL_PROP_CW` (only matters once yaw correction is enabled).
3. **IMU axes:** run `imu` repeatedly while tilting the frame by hand.
   - Right side down → `roll` goes **positive**.
   - Nose up → `pitch` goes **positive**.
   - Wrong angle moves → `IMU_SWAP_ROLL_PITCH 1`. Right angle, wrong sign → flip
     `IMU_ROLL_SIGN` / `IMU_PITCH_SIGN`.
4. **Mixer direction:** still with `imu`, the `mixer` line must show the motors on
   the **low** side with the **higher** numbers (it's pushing that side back up).
   If the high side gets more, stop: something in steps 1–3 is wrong.
5. **Level trim:** set the frame on a level surface and run `level`. Paste the
   printed `ROLL_TRIM` / `PITCH_TRIM` into the sketch.
6. **Arm test:** `esc arm` should refuse if the IMU isn't reporting or the frame is
   tilted more than 20°. Once armed, tilting past 60° must disarm.

First flights: tether the drone, keep throttle low, and tune `kp` / `kd` first
(raise `kp` until it oscillates, back off ~30 %; raise `kd` to damp). Then use `ki`
to remove slow drift.
