# Roadmap

Goal: the Steam Frame's SteamOS on the Quest 2, good enough to play VR games.
Engineering notes for finished items are in [docs/QUEST2_PORT.md](docs/QUEST2_PORT.md).

## Done

- [x] kexec from rooted Android into a patched Meta 4.19 kernel
- [x] Alpine Linux root on a pinned image, SSH over USB Ethernet, internet via the PC
- [x] Display on both eyes with an upright image (patched gamescope)
- [x] GPU: Vulkan (Turnip on KGSL), OpenGL (Zink), for Alpine and SteamOS
- [x] SteamOS (Steam Frame) userspace as a chroot; Steam client UI runs
- [x] Touch controllers as a gamepad (SyncBoss protocol, `qkx-controllers`)
- [x] Headset IMU stream decoded (accelerometer, gyroscope, temperature)
- [x] Steam starts automatically when the payload boots (`qkx-steam` service)
- [x] README with build and run instructions; repository backup

## Next: VR

1. **3DoF tracking**: orientation for the head and both controllers from their
   IMUs (sensor fusion; data already decoded). Gyro scale and axes to calibrate.
2. **VR runtime**: Monado (open-source OpenXR runtime) with a Quest 2 driver
   built on the SyncBoss streams; Monado's SteamVR plugin for SteamVR games.
3. **Lens distortion and chromatic aberration** correction (Monado compositor
   or the gamescope shader), with per-eye IPD.
4. **Tracking cameras**: get the four cameras streaming on Linux. They are
   configured through SyncBoss (the type-3 traffic in the Android trace) and
   the Qualcomm camera stack (`/dev/video*`, `/dev/media*`).
5. **Head 6DoF**: visual-inertial tracking with Basalt (as Monado uses it).
6. **Controller 6DoF**: infrared LED constellation tracking (Monado has work in
   progress for Rift S). LED models and calibration from the controllers
   (per-controller 0x8f queries seen in the trace). Research-level.

## Next: games and system

- [ ] FEX (x86 emulation, `fex-wtf` is in the image) for x86 Linux/Proton games
- [ ] Graphics drivers inside Steam's runtime containers (pressure-vessel)
- [ ] Map the controllers' capacitive touch (record `0x45`) and haptics
- [ ] Audio
- [ ] Wi-Fi (no USB tether needed)
- [ ] Battery, charging and thermal management (CPU clocks are capped for now)
- [ ] Proximity sensor calibration (Android supplies it from firmware files)
- [ ] Find the cause of the ~1-in-4 failed jumps
- [ ] Display: fix single wide planes in the kernel (now split into two)
- [ ] Longer term: mainline kernel, so SteamOS can boot natively with systemd
