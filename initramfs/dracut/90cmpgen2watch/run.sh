#!/bin/sh
# pre-udev: start the watcher detached before coldplug/module load.
# The binary dual-logs (stdout + /dev/kmsg) itself; the journal trace
# survives switch_root. Dies at pivot by design; the systemd unit
# covers rootfs-loaded-module shapes after pivot.
/usr/bin/cmp-gen2-watch --duration-ms 60000 &
