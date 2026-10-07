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
- [x] VR with Monado: 3DoF head, Touch controllers (rotation + inputs), `qkx-vr`

## Next: VR

Done so far: tracking cameras stream on Linux (`tools/camtrace`); all
calibration located on Android: `/persist/calibration/camera_calibration.json`
(OV7251 pinhole + Fisheye62, DeviceFromCamera), `imu_calibration.json`, and the
controllers' LED models in `/data/vendor/misc/sensors/controllercal/<id>`
(Rift S style "TrackedObject" JSON: 15 LEDs with position, normal, cone).
Copy them with `adb su` (never mount persist from Linux).

1. **Camera service**: port `replay.py` to C/C++, publish frames with
   timestamps, auto exposure (SyncBoss type-3 `a1` updates), separate the
   normal and LED exposures.
2. **Monado driver `quest2`**: SyncBoss IMU + controllers, the cameras, the
   calibration above. Head: 3DoF first, then 6DoF with Basalt through Monado's
   VIT/SLAM interface. Controllers: Monado's constellation tracker (used for
   Rift CV1 and PS Sense) with the LED models, fused with controller IMUs.
3. **Display path for VR**: Monado compositor output, distortion and
   chromatic correction, shown per eye (gamescope side-by-side mode or DRM).
4. **SteamVR**: Monado's SteamVR driver (`steamvr_drv`) in the SteamOS chroot.
5. Tracking quality: timestamps (SyncBoss nsync vs camera SOF), calibration
   conversion (Fisheye62 to Monado/Basalt camera models).

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
