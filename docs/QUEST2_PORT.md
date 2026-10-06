# Quest 2 port and SteamOS (Holo) roadmap

Goal: kexec from stock Quest 2 Android into a Linux kernel that runs the ARM64
SteamOS base used by the Steam Frame (Valve/Collabora "Holo Core", Arch
Linux aarch64), with graphics, input, audio and networking good enough to play
games. Status: **planning; nothing below is validated on Quest 2.**

## Survey results (2026-10-06, headset build 52150470034600150)

Captured with `tools/survey.sh` from a Magisk-rooted Quest 2:

- `hollywood`, DT model `Hollywood (DVT)`, compatible `qcom,kona`, 6 GB, 8 CPUs
- kernel `4.19.325-cip128-st12-g48f31e2db128`, MODVERSIONS on, no module
  signature enforcement, 4K pages, 39-bit VA, kprobes on
- all loader platform devices exist under the same names as Quest Pro
- **syncboss is `spi1.0`** (`spi0.0` is `cm710x`). Handled by the new
  `syncboss_dev` loader parameter; `tools/run.sh` picks it per board.
- staging window `0x90000000-0x93400000` is free System RAM
- ramoops is at `0x9ba00000+0x90000`, the same as Quest Pro, so the retained
  log at `0x9ba80000` (`marker_read.c`, target `init/main.c`) is unchanged
- `kexec_in_progress` (optional) and `send_irq` (only for `disconnect_qmp=1`)
  are absent
- same PM8150B/SMB5 and `ac4d000.qcom,cpas-cdm0` devices as the target patches
  expect

Kernel source: Meta's `oculus-quest2-kernel-master` has no exact commit for
this build. `6e4286073` (build 5220228, the first public `-cip128-st12`) is
the closest. All three `kernel/patches/` apply to it unchanged.

Userspace constraint: Holo Core follows current Arch, whose systemd (≥258)
requires Linux ≥5.4 with cgroup v2 only. The 4.19 vendor kernel cannot boot it
unmodified, so running the real Holo OS requires a mainline SM8250 kernel
(Route B below). The vendor kernel still serves as the first bring-up target.

## Why this is plausible

Quest 2 (XR2, `hollywood`) and Quest Pro (XR2+, `seacliff`) are both
`qcom,kona` (SM8250 family). Everything SoC-level the loader touches should be
identical:

| Loader assumption | Where | Quest 2 expectation |
|---|---|---|
| `of_machine_is_compatible("qcom,kona")` | `loader.c` init | same |
| WDT `17c10000.qcom,wdt` | `loader.c`, `prepare.py` | same (SoC block) |
| DWC3 `a600000.dwc3` | `loader.c`, `usb_log.c` | same (SoC block) |
| AOP QMP `c300000.qcom,qmp-aop` | `loader.c` | same (SoC block) |
| KGSL `3d00000.qcom,kgsl-3d0` | `loader.c` | same (SoC block) |
| core-hang regs `0x180x0060` | `loader.c` | same (8 Kryo 585 cores) |
| 4K pages, 39-bit VA, EL1 entry | `loader.c`, `transition.S` | expected, verify config |

Board-level items are the real port work:

| Item | Quest Pro value | Quest 2 |
|---|---|---|
| RAM | 12 GB | 6 GB, different carveouts |
| Staging window | `0x90000000-0x93400000` | verify against `iomem` |
| syncboss | `spi0.0` | verify in `spi-devices` |
| Android kernel source | `fa2e480a85…` | Meta's Quest 2 release matching `uname -r` |
| Target kernel | Quest Pro tree + `kernel/patches/` | Quest 2 tree + rebased patches |
| Panel/display init | seacliff panels, `qkx-splash` | different panel(s) |
| Install scripts | refuse non-`seacliff` | add `hollywood` |
| Secure ION ranges | `ion_secmap` | same mechanism, re-collect |

## Phase 0: access (blocking)

The loader needs root `insmod` on stock Quest 2 firmware. Nothing else
in this plan matters until that works on the firmware version on the headset.
Record the firmware's `ro.build.fingerprint`; every module build is tied to it.

## Phase 1: survey (read-only)

```sh
tools/survey.sh out/q2-survey
```

The script collects identity, `iomem`, config, runtime DTB, platform/SPI/UDC
devices and kallsyms. It also checks every name, symbol, config option and
memory range the loader assumes. Every `FAIL` line in `out/q2-survey/report` is
a port task. Nothing is written to the device.

## Phase 2: loader port

1. Build `quest_kexec.ko`, `marker_read.ko` and `usb_log.ko` against the
   Quest 2 Android kernel source with the captured config and exact local
   version (see HOW_TO_BUILD.md).
2. Replace hard-coded board values with a board profile selected by DT
   model/`ro.product.device`. These values are the staging window, syncboss bus
   name and the device whitelist in install scripts. Keep Quest Pro values
   unchanged and default.
3. Move the staging window if the survey rejects `0x90000000`. `prepare.py`,
   `loader.c` and the initrd/DTB addresses must change together.
4. Run the reversible tests in order: `test_transition=1`, `test_full_copy=1`,
   `test_cpus=2`, `execute=1 preflight_only=1/2/3`. Do the real jump only
   after all of them pass.

## Phase 3: target kernel, minimal userspace

1. Rebase `kernel/patches/0001-0003` onto the Quest 2 kernel source. The
   SMB5/SPMI and QMP-adopt changes are SoC/PMIC-level and should apply with
   small conflicts. Drop the seacliff-only drivers.
2. Boot `make initramfs` (BusyBox + USB ECM) with the retained log enabled.
3. Validate CPUs, USB gadget, the watchdog handover, then storage mounts
   (`qkx-mount-os`).

## Phase 4: Holo Core userspace

Install the Holo Core aarch64 rootfs into an image on `/data`, as
`tools/os-install.sh` does for alternate Android, and `switch_root` into it.

Known risk: Holo tracks current Arch. Its systemd, Mesa and gamescope expect a
modern kernel, but the Quest 2 vendor kernel is downstream 4.19. Check
Holo's systemd minimum kernel first. The decision point:

- **Route A: downstream 4.19 kernel.** It keeps the working display, sensors,
  Wi-Fi and DSPs. Graphics would need Mesa Turnip's KGSL backend (Vulkan, GL
  through Zink) and gamescope on the downstream `msm_drm` (SDE) KMS. This needs
  an older systemd, or patches, if Holo's is too new.
- **Route B: mainline SM8250 kernel.** It uses upstream `msm` DRM and
  freedreno/Turnip unmodified, which is the configuration Holo is built for.
  It needs a Quest 2 DTS, panel driver(s), and handling of the inherited
  hypervisor/SMMU state. Syncboss, cameras and audio are not upstream.

Recommendation: prototype Route A to reach a desktop quickly, and use the
result to decide whether Route B is worth the cost.

## Phase 5: "play games"

The realistic first milestone is flat (2D) gaming: Steam Big Picture under
gamescope on the panel, with a Bluetooth gamepad, Wi-Fi and audio. Needed:

- display: KMS output to the panel; per-eye distortion is not needed for 2D
- input: Bluetooth HID (gamepads)
- Wi-Fi/BT firmware from the vendor partition
- audio: ADSP + codec bring-up (vendor firmware, ALSA UCM)
- x86 games: FEX-Emu, which is part of Valve's ARM64 stack

## Phase 6: VR (long term, research)

Full VR needs reverse engineering beyond kernel porting:

- syncboss SPI protocol in userspace (IMU, controllers, proximity), then a
  Monado driver
- 6DoF head tracking: the Quest 2 tracking cameras need the camera stack,
  then SLAM (e.g. Basalt in Monado). Meta's tracking is proprietary.
- lens distortion/chromatic correction per Quest 2 optics
- SteamVR/OpenXR on ARM64: Steam Frame's tracking stack is tied to Frame
  hardware; Monado is the practical OpenXR runtime.

## Status 2026-10-06: kexec to BusyBox shell works on Quest 2

Validated: loader staging/relocation/CPU/shutdown rehearsals, real jump,
target boots to userspace in <1 s with 8 CPUs and ~5.6 GB, USB ECM shell at
10.42.0.2. Recipe:

```sh
# Android kernel for the loader (closest public source, exact release string)
git -C q2-kernel checkout 6e4286073
make module KDIR=<android-out> KSRC=<q2-kernel> LOCALVERSION=-g48f31e2db128
# Target: 6e4286073 + kernel/patches/000{1,2,3} + kernel/patches/quest2/*
tools/prep.sh out/q2-captured <target Image> out/initramfs.gz out/q2-payload qkx_keep_wdt=1
tools/run.sh out/q2-payload watchdog_recovery=1    # re-activate root first
```

Quest 2 specific findings:

- **USB:** dwc3-msm starts peripheral mode (pdphy extcon deferred) before
  SMB5 sees VBUS. SMB5's later DPDM request reset the HS PHY into UTMI
  non-driving mode, so the host never saw a device. Fixed by
  `quest2/0004`.
- **Retained log:** `0x9ba80000` is the second half of Android's pmsg zone
  (dump zones are 0x28000 each on this layout). Read it promptly after
  return; `tools/read-log.sh` also searches the pmsg snapshot.
- **Diagnostics:** `qkx_keep_wdt=1` (target) with `watchdog_recovery=1`
  (loader) turns hangs into DRAM-preserving resets. `init.sh` warm-resets
  if the host does not configure the gadget within 30 s, and replays USB
  dmesg lines into the retained log first. The LED shows blue at init,
  green when the UDC is bound, and red on failure (the tri-led driver
  currently fails to probe in the target).
- Never hold the power key while debugging: a long-press is a cold reset and
  loses the retained log. Root must be re-activated after every boot.
- `freeze_processes` can return -EBUSY right after Android boots; retry.

## Status 2026-10-06 (later): Linux rootfs on UFS

Alpine 3.22 (OpenRC) boots from a pinned 8 GiB image on userdata, with SSH at
10.42.0.2 using the key in `out/linux/`.

```sh
tools/linux-rootfs.sh out/linux 8          # build rootfs.img in an arm64 podman container
tools/linux-install.sh out/linux/rootfs.img out/q2-captured <target Image> out/linux
tools/run.sh out/linux/payload watchdog_recovery=1
ssh -i out/linux/id_ed25519 root@10.42.0.2
```

- **UFS:** Android's UFS shutdown hook (device power-down, link off, VCC
  off) left a device that never completed DME link startup in the target
  (`dme-link-startup: error code 1` ×4). The loader's `keep_ufs=1`, now
  default for `hollywood` in `run.sh`, skips that hook. The target's host reset
  and RST_n pulse then bring up the SK hynix part at HS-G4 ×2.
- Userdata is `sda9` on Quest 2. `qkx-install.sh` now resolves it by name.
- `init.sh` switch_roots into the rootfs when `/etc/qkx/maps/rootfs.map`
  exists, and falls back to the BusyBox shell on failure.
- Intermittent: about 1 in 4 jumps resets via PS_HOLD before the target's RAM
  log starts. A retry works. The cause is not yet found.
- Do not unbind `ufshcd-qcom` live: `ufshcd_remove` touches registers with
  clocks off, which raises an SError panic.

## Status 2026-10-06 (evening): display, RAM stability, GPU bring-up

- **Display: KMS works, the panel image does not.** `msm_drm` exposes DSI-1 at
  1920x3664@120 (DSC, 2 LM / 2 DSC / 2 DSI) and modesets succeed. The panel
  only ever shows rows of small purple blocks, for `modetest` dumb buffers
  and for imported buffers alike. Suspect the DSC/PPS or panel re-init after
  kexec: the panel was configured by Android and is never power-cycled.
  `display_panel_avdd` is also disabled by regulator late cleanup at ~32 s.
- **Device tree order:** `capture.sh` now restores boot-FDT node order with
  `dt-reorder.py`. The shuffled order hid the GMU from kgsl (forward
  sibling search, `-ENXIO`), renumbered the CPUs and broke the tri-LED.
- **Hangs under memory load (root cause):** Android places
  `size`/`alloc-ranges` reserved regions (secure display, QSEE TA, DSP heaps,
  CMA) dynamically and hands some to the hypervisor. In the target that RAM
  became free memory, and the first touch hard-hung the SoC with no watchdog
  bite. `run.sh` now reads this Android boot's placement and adds no-map
  fences (`tools/fence-reserved.py`); the target places fresh pools elsewhere.
  After the fix, 6x256 MiB copy loops ran 2 min at max DDR. Pinning the pools
  in place instead broke boot: secure ION no-map ranges lie inside them.
- **Thermal:** CPU zones use the `user_space` policy (Android's
  thermal-engine does the throttling), so nothing throttles in Linux.
  `/etc/local.d/qkx-thermal.start` sets schedutil plus clock caps; 3 min of
  full CPU load reached 75.6 °C.
- **Retained log moved** to `0x9ba40000` (ramoops dump zone 1 tail; the old
  slot is in the pmsg zone that logd overwrites). The loader mirrors its log
  there from phase 1. A watchdog bite comes back cold on Quest 2 (RAM lost),
  so the target turns the WDT bark into a warm reset (`qkx_keep_wdt=1`).
- **GPU:** kgsl probes (`/dev/kgsl-3d0`). Turnip with the KGSL backend is
  built on the host (`out/mesa/mesa-turnip-kgsl.tar.gz`, Mesa 25.1.9,
  `-Dfreedreno-kmds=msm,kgsl`), and firmware (`a650_sqe.fw`, `a650_gmu.bin`,
  `a650_zap.*`) is in rootfs `/lib/firmware`.
- **GPU works:** `vulkaninfo` reports `Turnip Adreno (TM) 650`, Vulkan
  1.3, Mesa 25.1.9. The blocker was the GPU RSC: the loader's GPU suspend
  leaves it in the sleep state, and kgsl's first cold boot skipped the wake
  sequence, so every GMU RPMh vote stalled (HFI id 30 ack and `OOB_set`
  timeouts). Fixed by `quest2/0006`.
- **Next for graphics:** Turnip on KGSL has no DRM display fd, so
  `VK_KHR_display` (`vkcube --wsi display`) sees no device. GPU frames must
  reach the panel as dma-bufs imported into `msm_drm`, the job of a compositor
  (gamescope).
- `kernel/patches/quest2/quest2_target_defconfig` is the full target config
  (device config plus kexec options, lockup detectors).
- **Open:** about 1 in 4 jumps still resets during the Android-side shutdown,
  leaving no log (bite = cold). Retry works.

## Status 2026-10-06 (night): compositor and CPU idle

- **gamescope runs on the DRM backend** with Turnip-KGSL and Xwayland, and
  `vkcube` runs inside it at ~40 commits/s. Patches:
  `patches/mesa/0001` (report msm_drm nodes via VK_EXT_physical_device_drm),
  `patches/gamescope/0001` (LINEAR when planes lack IN_FORMATS) and
  `0002` (libliftoff: SDE has `input_fence` not IN_FENCE_FD, and 8-bit
  `alpha`). Run with `--prefer-output DSI-1 --force-orientation normal`,
  `LIBSEAT_BACKEND=noop WLR_LIBINPUT_NO_DEVICES=1`.
- **Builds:** `tools/builder/Containerfile` (qkx-builder image) and
  `tools/build-userspace.sh mesa|gamescope`, with persistent trees in
  `out/build/` (gamescope rebuild after a patch: about 40 s).
- **CPU7 not waking from deep idle:** a soft lockup in `kick_all_cpus_sync`
  (sshd seccomp BPF JIT) had every CPU but CPU7 answering. `init.sh` now
  disables all cpuidle states except WFI.
- `tools/tests/ion2kms`: CPU-drawn ION buffer imported into msm_drm. It
  shows the same purple blocks, so the GPU is not involved.

## Status 2026-10-07: GPU frames on the panel (gamescope + vkcube)

**Root cause of the "purple block noise":** Quest 2's panel mode uses a dual
layer-mixer topology (2 LM, 2 DSC encoders, 2 DSI links). Scanning out
through ONE plane spanning the full 1920 px width shows rows of purple
blocks, whatever the mode (90 or 120 Hz) or buffer origin. One half-width
plane per mixer displays correctly. The Quest Pro `qkx-splash` already did
that (planes 58 + 80). Test tool: `tools/tests/ion2kms` `[secs] [cached]
[bars|grey] [split] [mode]`.

- `patches/gamescope/0003`: with `GAMESCOPE_DRM_SPLIT_PLANES=1` every layer
  is presented as a left and a right half on separate planes (twin libliftoff
  layers).
- **Result: user-confirmed spinning `vkcube` in gamescope on the panel**
  (Turnip-KGSL render, Xwayland, DRM backend).
- Working command (Alpine rootfs, root):
  ```sh
  export XDG_RUNTIME_DIR=/tmp/xdg VK_ICD_FILENAMES=/usr/local/share/vulkan/icd.d/freedreno_icd.aarch64.json \
    LIBSEAT_BACKEND=noop WLR_LIBINPUT_NO_DEVICES=1 GAMESCOPE_DRM_SPLIT_PLANES=1
  gamescope --backend drm --prefer-output DSI-1 --force-orientation normal -W 1920 -H 3664 -- vkcube --wsi xcb
  ```
- **Open:** the kernel-side fix for single wide planes (SDE source split with
  DSC), the panel orientation/per-eye layout and lens distortion, input
  devices, and the 1-in-4 jump failure.

## Status 2026-10-07: correct per-eye image in the headset

- **Problem:** the split-plane output showed the panel framebuffer as is.
  The Quest 2 panel is one portrait 1920x3664 panel with the eyes stacked
  along the framebuffer's y axis, each rotated by 90°, so the user saw two
  cubes. SDE cannot rotate linear RGB planes (atomic test rejects
  rotate-90/270).
- `patches/gamescope/0004`: with `GAMESCOPE_QKX_STEREO=1` the composite shader
  maps each panel pixel to its eye and to a rotated coordinate in a logical
  per-eye screen (eyeH x panel width). That screen is scaled uniformly into the
  normal layout, so no layout code changes. `GAMESCOPE_QKX_FLIPX=1` is the
  correct orientation (user-confirmed: one upright cube per eye). Needs
  `--force-composition`.
- Launcher: `rootfs/usr/local/bin/qkx-gamescope <cmd>` (installed on the
  headset).
- `build-userspace.sh` now resets source trees to HEAD (it used the git
  index, which can hold earlier patch states).
- **Next:** lens distortion and chromatic correction in the same pass, input,
  Steam.

## Status 2026-10-07: Steam client UI in the headset (SteamOS chroot)

- Valve's Frame base rootfs plus `deckard-steam-rel` etc. built by
  `tools/steamos-rootfs.sh` into a 24 GiB ext4 image, installed with
  `QKX_NAME=steamos tools/linux-install.sh` and run as a chroot from Alpine by
  `qkx-steamos steam` (mirrors the image's `steam.service`, restarts on exit 42).
- Kernel needed SysV IPC and namespaces (`quest2_target_defconfig`); the
  Android paranoid-network groups 3003/3004 are given to the steam user.
- Missing from Valve's public repos, now provided:
  - GTK 2 (`steamui.so` links it): `tools/steamos/build-gtk2.sh`.
  - The client's bundled FFmpeg is unversioned: `qkx-steam.sh` adds the
    soname links after every self-update.
  - GPU: `tools/build-userspace.sh mesa-holo` builds Turnip on KGSL + Zink
    (GL 4.6, EGL, GBM, glvnd) against glibc in `qkx-holo-builder`
    (`tools/builder/Containerfile.holo`); installed to `/usr/local` in the
    image with `ld.so.conf.d/00-qkx-mesa.conf`. glibc 2.43 declares
    `call_once`/`once_flag`, so Mesa 25.1's C11 emulation takes those two from
    glibc (edit in the `mesa-holo` fetch step).
  - Alpine-side Mesa also gained Zink, so Xwayland has glamor/GLX (Steam's
    vgui needs a GLX visual).
  - `lsof`: the client uses it to authenticate the web helper's localhost
    websocket ("unexpected transport error" without it).
  - `/etc/local.d/qkx-gpu.start` opens `/dev/kgsl-3d0`, `/dev/ion` and
    `/dev/dri/*` to the steam user (group ids differ between Alpine and the
    image).
- Result (user-confirmed): the Steam sign-in screen (phone-app QR) in the
  headset.
- **Next:** controllers and head tracking (syncboss), SteamVR, FEX for x86.

## Status 2026-10-07: Touch controllers and headset IMU via SyncBoss

- SyncBoss (nRF52 MCU on `spi1.0`, Meta's in-tree driver) carries the headset
  IMU and the Touch controller radio. Bring-up on Linux:
  `transaction_length` = 512 (sysfs), keep `/dev/syncboss0` open, read
  `/dev/syncboss_stream0`. `firmware_class` timeout 1 s (the proximity
  calibration files are Android-only and otherwise stall open() ~4 min).
- Commands were found by tracing Android's sensors HAL with a kprobe on
  `queue_tx_packet` (`tools/syncboss/android-txtrace.sh`, `txdecode.py`) and
  replaying subsets (`sbstart.py`): **110 [0]** enables headset IMU 0,
  **133 [0]** starts the controller radio. 90/203/204 crash the MCU (sent by
  the HAL on shutdown); 46 stalls the stream.
- Headset IMU, packet type 80 (~1 kHz): u64 µs timestamp, accel xyz (f32, g),
  gyro xyz (f32), temperature (f32).
- Controllers, packet type 143: 8-byte id, descriptor (byte 10: 0 left,
  1 right), link info, then records `[key, flags, data]` (lengths in
  `qkx-controllers`): `0x24` buttons (bit0 A/X, bit1 B/Y, bit2 stick, bit3
  Meta/Menu), `0x63` trigger (low 12 bits) and grip (high 12), inverted,
  `0x82` stick int16 x/y, `0x41` motion (u32 ts + 6 x int16), `0x45`
  capacitive touch (not mapped yet).
- `rootfs/usr/local/bin/qkx-controllers` (OpenRC `qkx-controllers`): both
  controllers as one uinput gamepad (Xbox layout, Meta = guide). Verified
  every input on the headset.
- **Next:** Steam picking up the pad; orientation from the IMUs (3DoF head
  and controllers); then SteamVR/OpenXR. Positional (6DoF) tracking needs the
  tracking cameras (SLAM / LED constellation) and is a research item.

## Status 2026-10-07: tracking cameras stream on Linux

- See `tools/camtrace/README.md`. Meta's camera HAL use of the Spectra driver
  was recorded on Android (`module/qkx_camtrace.ko`) and is replayed on Linux
  (`tools/camtrace/replay.py`) with the SyncBoss camera commands. All four
  OV7251 cameras stream (640x480 RAW10, ~50 Hz); short "LED" exposures show
  the controllers' infrared LED rings.
- SyncBoss camera commands seen: 47/46/44 with mask 0x0f (cameras), type-3
  sub-commands `a3` (period 20000 us), `ef` (exposure slots), `64`, `e2`,
  `8f <controller id> ... 28` (controller LED timing), `a1` (exposure/gain,
  continuously from Android's auto exposure).
- **Do not mount `persist` (sda2) from Linux**: it reset the headset (likely
  protected storage). Read calibration from Android instead.
- Kernel learnt-handle substitution must ignore small values (lengths): only
  values with a non-zero upper half, plus IOMMU handles from QUERY_CAP.
