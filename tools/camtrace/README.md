# Quest 2 tracking cameras

The four OV7251 tracking cameras (640x480 mono, MIPI RAW10, ~50 Hz each) are
powered and exposed by the SyncBoss MCU; the SoC side is Qualcomm's Spectra
driver (CSIPHY -> IFE raw dump) with Meta's "stream mode" frame API. Meta's
camera HAL (libfastpathcamerahal) drives it directly; there is no open driver
in userspace, so the start-up is recorded on Android and replayed on Linux.

Frames alternate between a normal exposure (room, for head tracking) and a
very short one where only the controllers' infrared LEDs show.

## Record (Android, root)

```sh
make module KDIR=... KSRC=... LOCALVERSION=...      # builds module/qkx_camtrace.ko
adb push module/qkx_camtrace.ko tools/syncboss/android-txtrace.sh /data/local/tmp/
adb shell su -c '
  echo 1 > /proc/sys/kernel/kptr_restrict
  A=$(grep -w video_devices /proc/kallsyms | cut -d" " -f1)
  echo 2 > /proc/sys/kernel/kptr_restrict
  insmod /data/local/tmp/qkx_camtrace.ko vdev_table=0x$A copy_any=1
  sh /data/local/tmp/android-txtrace.sh start
  echo > /sys/kernel/tracing/trace; echo x > /proc/qkx_camtrace
  kill <pid of vendor.oculus.hardware.sensors@1.0-service>   # it restarts
  sleep 25'
adb exec-out su -c 'cat /proc/qkx_camtrace' > run4-cam.bin
adb shell su -c 'cat /sys/kernel/tracing/trace' > run4-syncboss-tx.trace
```

Do not `rmmod` qkx_camtrace (an ioctl blocked in the wrapper would crash);
reboot instead. `vdev_table` reaches cam-req-mgr and cam_sync, which only
allow one open (the HAL's).

## Replay (Linux)

```sh
python3 replay.py <dir with run4-*> 10 --frames 4 --out /root/camframes
```

Kernel handles are learnt from live results and substituted into later
arguments and packets; Android's gralloc image buffers (MAP_BUF) become
driver allocations. `decode.py` prints a recording's timeline.

## Status

All four cameras stream (~45-50 Hz each). Not yet: auto exposure (Android
keeps sending SyncBoss type-3 `a1` exposure updates), frame timestamps to
IMU time, camera calibration, a service that publishes frames.
