#!/bin/bash
# sudo debootstrap --variant=minbase jammy ./my-rootfs http://us.archive.ubuntu.com/ubuntu/
mkdir -p my-rootfs
cd my-rootfs
mkdir -p {bin,sbin,usr,usr/bin,usr/sbin,usr/lib,etc,etc/init.d,lib,proc,sys,dev,run,var}
cp ../busybox .

sudo ./busybox --install bin

sudo mknod -m 666 dev/null c 1 3
sudo mknod -m 666 dev/zero c 1 5
sudo mknod -m 666 dev/random c 1 8
sudo mknod -m 666 dev/urandom c 1 9
sudo mknod -m 666 dev/tty c 5 0
sudo mknod -m 600 dev/console c 5 1

echo 'root:x:0:0:root:/root:/bin/bash' > etc/passwd
echo 'root:x:0:' > etc/group