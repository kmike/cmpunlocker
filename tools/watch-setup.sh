#!/bin/bash
set -euo pipefail

# Install/remove the advertisement-triggered PCIe Gen2 retrain
# (replacement for the legacy gen2.service hammer). Two stages:
#   initramfs hooks  — spawned pre-udev; survives the pivot
#   systemd unit     — second net for windows that open later
# The watcher binary is built from tools/cmp-gen2-watch.c (a C compiler
# is required, as it already is for the patched kernel modules).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WATCH_SRC="${SCRIPT_DIR}/cmp-gen2-watch.c"
WATCH_BIN="/usr/local/sbin/cmp-gen2-watch"
UNIT_NAME="cmp-gen2-watch.service"
UNIT_SRC="${PROJECT_DIR}/systemd/${UNIT_NAME}"
UNIT_DST="/etc/systemd/system/${UNIT_NAME}"
DRACUT_SRC="${PROJECT_DIR}/initramfs/dracut/90cmpgen2watch"
DRACUT_DST="/usr/lib/dracut/modules.d/90cmpgen2watch"
IT_HOOK_SRC="${PROJECT_DIR}/initramfs/initramfs-tools/hooks/cmp-gen2-watch"
IT_TOP_SRC="${PROJECT_DIR}/initramfs/initramfs-tools/scripts/init-top/cmp-gen2-watch"

source "${PROJECT_DIR}/common/lib.sh"

usage() {
    cat <<EOF
Usage:
  sudo $0 install   Build + install both watcher stages (armed for next boot)
  sudo $0 remove    Disable and remove both stages (driver remains installed)
  sudo $0 verify    Show negotiated link speed per card (LnkSta — the only
                    honest indicator; nvidia-smi and LnkCap can lag/lie)
  $0 status         Show install state
EOF
}

require_root() { [[ "${EUID}" -eq 0 ]] || die "Run this action as root"; }

supported_gpus() {
    for id in 20c2 2082; do
        lspci -D -d "10de:${id}" 2>/dev/null | awk '{print $1}'
    done
}

build() {
    command -v cc >/dev/null 2>&1 \
        || die "no C compiler found (a compiler is already required to build the patched modules)"
    info "Building ${WATCH_BIN} from source"
    cc -O2 -Wall -o "${WATCH_BIN}.tmp" "${WATCH_SRC}"
    mv "${WATCH_BIN}.tmp" "${WATCH_BIN}"
}

disable_legacy() {
    # Supersede the timer-based hammer (and older helpers) if present.
    for unit in gen2.service cmpretrain.service cmp-gen2-retrain.service; do
        if systemctl list-unit-files "${unit}" >/dev/null 2>&1 \
           && systemctl is-enabled "${unit}" >/dev/null 2>&1; then
            systemctl disable --now "${unit}" 2>/dev/null || true
            systemctl reset-failed "${unit}" 2>/dev/null || true
            warn "disabled legacy unit ${unit} (superseded by ${UNIT_NAME})"
        fi
    done
}

install_stage() {
    require_root
    build

    # userspace stage (clean stale wants-symlinks from earlier unit
    # versions that used a different Install target, then enable)
    install -m 0644 "${UNIT_SRC}" "${UNIT_DST}"
    rm -f /etc/systemd/system/multi-user.target.wants/${UNIT_NAME}
    systemctl daemon-reload
    systemctl enable "${UNIT_NAME}" >/dev/null
    ok "Enabled ${UNIT_NAME} (starts on next boot; not started now)"

    # initramfs stages — install whichever generators exist
    if command -v dracut >/dev/null 2>&1 && [ -d /usr/lib/dracut/modules.d ]; then
        mkdir -p "${DRACUT_DST}"
        install -m 0755 "${DRACUT_SRC}/module-setup.sh" "${DRACUT_DST}/"
        install -m 0755 "${DRACUT_SRC}/run.sh" "${DRACUT_DST}/"
        ok "Installed dracut module 90cmpgen2watch"
    fi
    if [ -d /etc/initramfs-tools ]; then
        mkdir -p /etc/initramfs-tools/hooks /etc/initramfs-tools/scripts/init-top
        install -m 0755 "${IT_HOOK_SRC}" /etc/initramfs-tools/hooks/cmp-gen2-watch
        install -m 0755 "${IT_TOP_SRC}" /etc/initramfs-tools/scripts/init-top/cmp-gen2-watch
        ok "Installed initramfs-tools hooks"
    fi
    if command -v update-initramfs >/dev/null 2>&1; then
        info "Rebuilding initramfs"
        update-initramfs -u -k "$(uname -r)"
    elif command -v dracut >/dev/null 2>&1; then
        info "Rebuilding initramfs (dracut)"
        dracut -f
    fi
    disable_legacy
    ok "Gen2 watcher armed for the next boot"
}

remove_stage() {
    require_root
    systemctl disable --now "${UNIT_NAME}" 2>/dev/null || true
    systemctl reset-failed "${UNIT_NAME}" 2>/dev/null || true
    rm -f "${UNIT_DST}" "${WATCH_BIN}"
    rm -rf "${DRACUT_DST}"
    rm -f /etc/initramfs-tools/hooks/cmp-gen2-watch \
          /etc/initramfs-tools/scripts/init-top/cmp-gen2-watch
    systemctl daemon-reload 2>/dev/null || true
    if command -v update-initramfs >/dev/null 2>&1; then
        update-initramfs -u -k "$(uname -r)" >/dev/null 2>&1 || true
    fi
    ok "Removed Gen2 watcher (all stages)"
}

verify_stage() {
    local gpus=0
    for bdf in $(supported_gpus); do
        gpus=1
        local sta
        sta="$(setpci -s "${bdf}" CAP_EXP+12.w 2>/dev/null || true)"
        if [[ "${sta}" =~ ^[[:xdigit:]]{4}$ ]]; then
            local speed=$((0x${sta} & 0x0f)) width=$(((0x${sta} >> 4) & 0x3f))
            local verdict="Gen${speed}"
            (( speed >= 2 )) && verdict="Gen${speed} OK"
            echo "${bdf}: LnkSta=0x${sta} speed=${speed} width=x${width} -> ${verdict}"
        else
            echo "${bdf}: LnkSta unreadable"
        fi
    done
    (( gpus )) || warn "no supported GPUs present"
    echo "Log (userspace stage): /var/log/cmp-gen2-watch.log; initramfs stage: journalctl -k | grep cmp-gen2-watch"
}

status_stage() {
    echo "binary:  $([[ -x ${WATCH_BIN} ]] && echo present || echo MISSING)"
    echo "unit:    $(systemctl is-enabled ${UNIT_NAME} 2>/dev/null || echo not-installed)"
    echo "dracut:  $([[ -d ${DRACUT_DST} ]] && echo present || echo absent)"
    echo "i-t:     $([[ -f /etc/initramfs-tools/hooks/cmp-gen2-watch ]] && echo present || echo absent)"
}

case "${1:-}" in
    install) install_stage ;;
    remove)  remove_stage ;;
    verify)  verify_stage ;;
    status)  status_stage ;;
    *) usage; exit 1 ;;
esac
