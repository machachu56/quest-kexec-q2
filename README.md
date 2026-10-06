# Quest-KeXec: SteamOS on the Meta Quest 2

Boot a Linux system with **SteamOS** (the ARM64 SteamOS of Valve's Steam Frame)
on a **rooted Meta Quest 2**, without unlocking the bootloader and without
touching any partition.

A kernel module loaded into the running Android kernel hands the CPU to a
custom kernel (kexec). That kernel boots a small Alpine Linux system from an
image file on the headset's storage, which starts Steam from a SteamOS image,
full screen in the headset. Reboot the headset and you are back in Android.

Based on [quest-kexec](https://github.com/YusufOzmen01/quest-kexec) (Quest Pro)
and [kexec-at-home](https://github.com/provide-salad/kexec-at-home).
Not affiliated with Meta or Valve.

> **Root is required** (Magisk). The project was developed on a Quest 2 with
> build 52150470034600150 (kernel `4.19.325-cip128-st12-g48f31e2db128`). Other
> builds need a matching loader module (see [step 3](#3-loader-module)).

## Status

| Works | Not yet |
|---|---|
| kexec from Android into a 4.19 kernel built from Meta's source | Head and controller **tracking** (3DoF/6DoF) |
| Alpine Linux root (OpenRC), SSH over USB Ethernet | Lens distortion correction |
| UFS storage, 8 CPUs, ~5.6 GB RAM, GPU (Adreno 650) | Audio, Wi-Fi, Bluetooth |
| Display: both eyes, upright image (patched gamescope) | Tracking cameras |
| Vulkan 1.3 (Turnip on KGSL) and OpenGL 4.6 (Zink) | SteamVR / OpenXR |
| SteamOS (Steam Frame) userspace as a chroot | x86 games (FEX not set up yet) |
| **Steam client UI at boot**, signed in with the phone QR code | Battery/thermal management (clocks are capped) |
| **Touch controllers** as a gamepad (all buttons, triggers, grips, sticks) | |

Detailed engineering notes: [docs/QUEST2_PORT.md](docs/QUEST2_PORT.md).
Roadmap: [TODO.md](TODO.md).

## Safety

- Nothing is flashed, formatted or repartitioned. Everything lives in
  `/data/local/tmp/` on the headset: the loader, and the Linux images as
  ordinary (pinned) files in the Android data partition.
- A reboot (or a failed jump) always comes back to Android.
- Remove everything with:
  ```sh
  adb shell su -c 'rm -rf /data/local/tmp/qkx-linux /data/local/tmp/qkx*'
  ```
- About 1 in 4 jumps fails and the headset reboots into Android. Re-activate
  root and run it again.
- Root (Magisk) must be re-activated after **every** Android boot.

## Host requirements

A Linux x86_64 PC (developed on Fedora 44) with:

- `podman` and `qemu-user-static` (ARM64 containers build all the userspace)
- `adb`, `git`, `make`, `curl`, `python3`, `dtc`/`fdtput`, `zstd`
- `aarch64-linux-gnu-` binutils/gcc (kernel builds use clang, below)
- About 60 GB of free disk space

The kernels are built with Android's clang 14 (r450784e), the compiler Meta
uses for this kernel.

## Building

The commands below use one workspace directory:

```text
$W/quest-kexec          this repository
$W/toolchains/          Android clang
$W/q2-kernel            Meta's Quest 2 kernel source
$W/q2-android-out       build of the kernel Android runs (for the loader module)
$W/q2-target            target kernel source (worktree with this repo's patches)
$W/q2-target-out        target kernel build
```

```sh
W=~/quest; mkdir -p $W && cd $W
git clone -b quest2-port <this repository URL> quest-kexec
```

### 1. Toolchain

```sh
mkdir -p $W/toolchains/clang-r450784e
curl -L https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/master-kernel-build-2022/clang-r450784e.tar.gz |
  tar -xz -C $W/toolchains/clang-r450784e
export PATH=$W/toolchains/clang-r450784e/bin:$PATH
clang --version      # Android ... clang version 14.0.7
```

### 2. Kernel source

```sh
git clone --filter=blob:none -b oculus-quest2-kernel-master \
  https://github.com/facebookincubator/oculus-linux-kernel $W/q2-kernel
git -C $W/q2-kernel checkout 6e4286073
```

`6e4286073` (build 5220228) is the published commit closest to the kernel on
the headset.

### 3. Loader module

The module is loaded into the running Android kernel, so it is built against
a build of that kernel (for symbol versions). Connect the headset with adb,
activate root, then:

```sh
adb shell uname -r      # 4.19.325-cip128-st12-g48f31e2db128
LOCAL=-g48f31e2db128    # the part after "-st12"
K="ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang HOSTLD=ld.lld CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu-"

mkdir -p $W/q2-android-out
adb exec-out 'su -c "zcat /proc/config.gz"' > $W/q2-android-out/.config
$W/q2-kernel/scripts/config --file $W/q2-android-out/.config --disable LOCALVERSION_AUTO
make -C $W/q2-kernel O=$W/q2-android-out $K LOCALVERSION=$LOCAL olddefconfig
make -C $W/q2-kernel O=$W/q2-android-out $K LOCALVERSION=$LOCAL \
  KCFLAGS=-I$W/q2-kernel/drivers/pinctrl -j$(nproc) Image modules

cd $W/quest-kexec
make module KDIR=$W/q2-android-out KSRC=$W/q2-kernel LOCALVERSION=$LOCAL
modinfo -F vermagic module/quest_kexec.ko   # must start with `adb shell uname -r`
```

### 4. Target kernel

Meta's kernel plus the patches in `kernel/patches/quest2/` (kexec hand-off,
USB, UFS, GPU wake-up, watchdog), with the config in the same directory.

```sh
git -C $W/q2-kernel worktree add -b qkx-q2-target $W/q2-target 6e4286073
git -C $W/q2-target am $W/quest-kexec/kernel/patches/quest2/*.patch
mkdir -p $W/q2-target-out
cp $W/quest-kexec/kernel/patches/quest2/quest2_target_defconfig $W/q2-target-out/.config
make -C $W/q2-target O=$W/q2-target-out $K LOCALVERSION=-qkx olddefconfig
make -C $W/q2-target O=$W/q2-target-out $K LOCALVERSION=-qkx \
  KCFLAGS=-I$W/q2-target/drivers/pinctrl -j$(nproc) Image
IMAGE=$W/q2-target-out/arch/arm64/boot/Image
```

### 5. Device capture

The target kernel boots with the headset's own device tree, captured from
the running Android (root active):

```sh
cd $W/quest-kexec
tools/capture.sh out/q2-captured
```

### 6. Linux system (Alpine)

```sh
make initramfs                       # static BusyBox (in an arm64 container if needed)
tools/linux-rootfs.sh out/linux 8    # 8 GiB Alpine image + SSH key
```

`linux-rootfs.sh` also builds, on first use, the patched **Mesa** (Turnip on
KGSL, Zink) and **gamescope** (Quest 2 per-eye output) with
`tools/build-userspace.sh mesa|gamescope` in an ARM64 builder container. Builds
are incremental (`out/build/`); under emulation the first one takes a while.

### 7. SteamOS image

```sh
tools/steamos-rootfs.sh out/steamos 24
```

This downloads Valve's published Steam Frame base system and Steam packages,
builds the pieces their public repositories lack (GTK 2 for the Steam client,
Mesa for glibc), and writes `out/steamos/steamos.img` (24 GiB, mostly free
space for games).

## Installing

Activate root on the headset, then:

```sh
cd $W/quest-kexec
tools/linux-install.sh out/linux/rootfs.img out/q2-captured $IMAGE out/linux
QKX_NAME=steamos tools/linux-install.sh out/steamos/steamos.img out/q2-captured $IMAGE out/linux
```

Each image is copied into a pinned file under `/data/local/tmp/qkx-linux`
(the SteamOS one is a 3 GB compressed transfer). Both commands also rebuild the
kexec payload in `out/linux/payload`.

## Running

1. Boot the headset into Android and **activate root**.
2. Share the PC's internet with the headset (Steam needs it):
   ```sh
   sudo tools/host-share-internet.sh on
   ```
3. Jump:
   ```sh
   tools/run.sh out/linux/payload watchdog_recovery=1
   ```
   After a few seconds a USB Ethernet device appears on the PC. If the
   headset reboots into Android instead (about 1 in 4 times), re-activate
   root and repeat.
4. Within about a minute **Steam starts in the headset**. Sign in with the
   QR code and the Steam mobile app. The Touch controllers work as one
   gamepad: sticks and A/B to navigate, Meta is the guide (Steam) button,
   Menu is Start.

The headset hands the PC the address 10.42.0.1 over DHCP on the USB link
(with NetworkManager: `nmcli device connect <interface>` if it stays down).
Then:

```sh
ssh -i out/linux/id_ed25519 root@10.42.0.2
```

| Task | Command (on the headset) |
|---|---|
| Steam session | `rc-service qkx-steam stop` / `start` / `restart` |
| Boot to the Linux shell only, without Steam | `mkdir -p /etc/qkx && touch /etc/qkx/no-autosteam` |
| Shell inside SteamOS | `qkx-steamos shell` |
| Logs | `/var/log/qkx-steam.log`, `/var/log/qkx-controllers.log`, Steam's own in `/steamos/home/steam/.local/share/Steam/logs/` |
| Back to Android | `reboot` |

Keyboard and mouse can be forwarded from the PC: `tools/qkx-input.py list`,
then `tools/qkx-input.py send <device>`.

## Repository layout

| Path | Contents |
|---|---|
| `module/` | Loader kernel module (runs in Android's kernel) |
| `kernel/patches/quest2/` | Target kernel patches and config |
| `initramfs/` | Initramfs: USB networking, maps the images, switch_root |
| `rootfs/` | Files added to the Alpine image (services, launchers, controller driver) |
| `patches/` | Mesa and gamescope patches |
| `tools/` | Host scripts: build, install, run, capture |
| `tools/syncboss/` | Tools used to work out the controller/IMU protocol |
| `docs/` | Port notes; the original Quest Pro README is `docs/README_QUEST_PRO.md` |

## How it works, briefly

- **Hand-off:** `quest_kexec.ko` stages the kernel, initramfs and device tree
  in memory, quiesces Android's drivers and jumps. The target kernel is Meta's
  4.19 with fixes for state the previous kernel leaves behind (USB PHY, UFS
  link, GPU power controller).
- **Storage:** the images are pinned files in the data partition. The initramfs
  maps their blocks with device-mapper (`qkx-rootfs`, `qkx-steamos`).
- **Display:** the panel is one 1920x3664 portrait screen with an eye on each
  half. gamescope is patched to compose a rotated image per eye.
- **SteamOS:** the Steam Frame userspace needs a newer kernel than 4.19 for its
  systemd, so it runs as a chroot. `qkx-steamos` replaces `steam.service`.
- **Controllers:** the headset MCU ("SyncBoss") relays controller radio and
  IMU data. Commands were found by tracing Meta's Android service.
  `qkx-controllers` turns the stream into a Linux gamepad.

## License

GPL-2.0. See `LICENSE`. AI has been used heavily on this project.
