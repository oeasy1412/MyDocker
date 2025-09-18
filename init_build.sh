#!/bin/bash
# sudo debootstrap --variant=minbase jammy ./my-rootfs http://us.archive.ubuntu.com/ubuntu/
set -e
ROOTFS=""
while true;do
    case "$1" in
        --fs)
            if [ -z "$2" ]; then
                echo "Error: --fs requires an argument" >&2
                exit 1
            fi
            ROOTFS="$2"
            shift 2;;
        *) break;;
    esac
done

if [ ! -d ${ROOTFS} ]; then
    mkdir -p ${ROOTFS}
    cd ${ROOTFS}
    mkdir -p {bin,sbin,usr/{bin,sbin,lib},etc/init.d,lib,proc,sys,dev/pts,run,var,.old_root}
    cp ../app/busybox bin/busybox

    sudo bin/busybox --install bin

    sudo mknod -m 666 dev/null c 1 3
    sudo mknod -m 666 dev/zero c 1 5
    sudo mknod -m 666 dev/random c 1 8
    sudo mknod -m 666 dev/urandom c 1 9
    sudo mknod -m 666 dev/tty c 5 0
    sudo mknod -m 600 dev/console c 5 1

    if [ ! -f etc/passwd ]; then
        echo "root:x:0:0:root:/root:/bin/sh" > etc/passwd
        echo "root:x:0:" > etc/group
        chmod 644 etc/passwd etc/group
    fi
    if [ ! -f etc/resolv.conf ]; then
        echo "nameserver 8.8.8.8" > etc/resolv.conf
        echo "nameserver 8.8.4.4" >> etc/resolv.conf
    fi
    if [ ! -f etc/nsswitch.conf ]; then
        echo "hosts:          files dns" > etc/nsswitch.conf
    fi
fi