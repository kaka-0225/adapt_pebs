#!/usr/bin/env bash
set -euo pipefail

# kexec into the newly built memtis kernel
KERNEL_VERSION="5.15.19-htmm"
VMLINUX="/boot/vmlinuz-${KERNEL_VERSION}"
INITRD="/boot/initrd.img-${KERNEL_VERSION}"

if [[ ! -f "$VMLINUX" ]]; then
	echo "ERROR: missing kernel image: $VMLINUX" >&2
	exit 1
fi

if [[ ! -f "$INITRD" ]]; then
	echo "ERROR: missing initrd: $INITRD" >&2
	exit 1
fi

echo "Loading kexec kernel: $KERNEL_VERSION"
sudo kexec -l "$VMLINUX" --initrd="$INITRD" --command-line="$(cat /proc/cmdline) mem=90G "

echo "Executing kexec..."
sudo kexec -e
