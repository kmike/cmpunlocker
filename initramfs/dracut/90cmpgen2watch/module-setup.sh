#!/bin/bash
# dracut module 90cmpgen2watch — initramfs stage of the advertisement-
# triggered Gen2 retrain. Catches advertisement windows that open while
# the NVIDIA module loads from the initramfs (server boot shapes, where
# systemd userspace starts later than the ~430 ms window).
check() {
    # include only when a supported card is present
    for d in /sys/bus/pci/devices/*; do
        [ -r "$d/vendor" ] || continue
        [ "$(cat "$d/vendor")" = "0x10de" ] || continue
        [ -r "$d/device" ] || continue
        case "$(cat "$d/device")" in
            0x20c2|0x2082) return 0 ;;
        esac
    done
    return 1
}

depends() { return 0; }

install() {
    inst_simple /usr/local/sbin/cmp-gen2-watch /usr/bin/cmp-gen2-watch
    inst_hook pre-udev 99 "$moddir/run.sh"
}
