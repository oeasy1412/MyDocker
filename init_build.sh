#!/bin/bash
# sudo debootstrap --variant=minbase jammy ./my-rootfs http://us.archive.ubuntu.com/ubuntu/
if [ ! -f ./my-rootfs ]; then
    mkdir -p my-rootfs
    cd my-rootfs
    mkdir -p {bin,sbin,usr,usr/bin,usr/sbin,usr/lib,etc,etc/init.d,lib,proc,sys,dev,run,var}
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