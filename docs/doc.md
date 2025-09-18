## more details
- 1. **CS 结构**
```sh
# 启动 守护进程 + run一个程序的底层
> ps aux | grep my-docker
# sudo 命令出于安全隔离会openpty()+fork()创建一个新的会话&终端(pts/11)
# 而原本的终端(pts/10)负责：阻塞在 poll(fds, ...) 或 select(fds, ...)，监听自己的 STDIN 和 master_fd 缓冲区。poll()调用返回为可读时，就写到STDOUT
root       25233  0.0  0.0   8924  5248 pts/10   S+   14:08   0:00 sudo build/my-docker
# sudo 创建的辅助进程，作为新会话(pts/11)的leader（Ss），fork()的子进程 execvp("build/my-docker", ...)，父进程作为leader处理信号、子进程退出码、清理等工作
root       25234  0.0  0.0   8924  2300 pts/11   Ss   14:08   0:00 sudo build/my-docker
root       25235  0.0  0.0   6088  3584 pts/11   S+   14:08   0:00 build/my-docker      # 这是主守护进程(Daemon)
# 使用 fork 实现双向通信，避免 I/O 死锁
username   25612  0.0  0.0   6088  3072 pts/12   S+   14:09   0:00 build/my-docker run /bin/sh # 客户端的父进程(Socket -> STDOUT)
username   25613  0.0  0.0   6088   256 pts/12   S+   14:09   0:00 build/my-docker run /bin/sh # 客户端的子进程(STDIN  -> Socket)
root       25614  0.0  0.0   7116  3332 pts/11   S+   14:09   0:00 build/my-docker      # Daemon fork 的 Handler 进程，使用 clone() 创建真正的容器进程
username   25711  0.0  0.0   4028  2304 pts/13   S+   14:09   0:00 grep --color=auto my-docker
```

- 2. **ns 隔离**
```sh
 ~/> sudo ls -l /proc/25615/ns/
total 0
lrwxrwxrwx 1 root root 0 Sep 18 15:16 cgroup -> 'cgroup:[4026532292]' # new
lrwxrwxrwx 1 root root 0 Sep 18 15:16 ipc -> 'ipc:[4026532290]' # new
lrwxrwxrwx 1 root root 0 Sep 18 15:16 mnt -> 'mnt:[4026532227]' # new
lrwxrwxrwx 1 root root 0 Sep 18 15:16 net -> 'net:[4026532293]' # new
lrwxrwxrwx 1 root root 0 Sep 18 15:16 pid -> 'pid:[4026532291]' # new
lrwxrwxrwx 1 root root 0 Sep 18 15:16 pid_for_children -> 'pid:[4026532291]' # new
lrwxrwxrwx 1 root root 0 Sep 18 15:16 time -> 'time:[4026531834]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 time_for_children -> 'time:[4026531834]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 user -> 'user:[4026532226]' # new
lrwxrwxrwx 1 root root 0 Sep 18 15:16 uts -> 'uts:[4026532289]'   # new
 ~/> sudo ls -l /proc/1/ns/
total 0
lrwxrwxrwx 1 root root 0 Sep 18 15:16 cgroup -> 'cgroup:[4026531835]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 ipc -> 'ipc:[4026532206]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 mnt -> 'mnt:[4026532217]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 net -> 'net:[4026531840]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 pid -> 'pid:[4026532219]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 pid_for_children -> 'pid:[4026532219]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 time -> 'time:[4026531834]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 time_for_children -> 'time:[4026531834]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 user -> 'user:[4026531837]'
lrwxrwxrwx 1 root root 0 Sep 18 15:16 uts -> 'uts:[4026532218]'

# P.S. 但是 docker 的 默认行为是不做 User Namespace 映射的（）
# 对开启 User Namespace 的容器，当容器内的root进程（在宿主机上其实是UID 1001）尝试写入 /host/data 时，它会因为权限不足而被拒绝 (Permission Denied)，因为它不是文件所有者。
# 为了解决这个问题，Docker Daemon 必须采取一个非常暴力的措施：它会自动递归 chown 挂载点目录，将其在宿主机上的所有权递归地修改为那个映射后的高编号 UID (1001)
# 1. 性能雪崩: 对于包含大量文件的数据卷，执行 chown -R 是一个极其缓慢的操作，它会大大延长容器的启动时间。
# 2. 破坏宿主机权限: 原本属于你普通用户（UID 1000）的 /host/data 目录，现在被改成了 UID 1001 用户所有。你自己在宿主机上无法访问这些文件了！ 这是一个非常糟糕且出乎意料的副作用，对于开发者来说是不可接受的。 一些特定的用户才能访问的硬件设备（如USB），在启用了 User Namespace 后，容器内的非特权用户可能无法获得访问这些设备的权限。
# 4. 性能开销: 每次文件操作需查询映射表 `/proc/<pid>/uid_map` ，进行一次 UID/GID 的转换

## 结论就是：Docker 将 “开箱即用的易用性” 和 “最大的向后兼容性” 放在了比 “默认的最高安全性” 更高的优先级上。
```
- nsenter 验证
```sh
sudo nsenter -t 25615 -m ls
sudo nsenter -t 25615 -m ps -ef
sudo nsenter -t 25615 -m top
sudo nsenter -t 25615 -n ip addr
sudo nsenter -t 25615 -m -n ping 8.8.8.8 -c 1 # 没有 -n 将会使用宿主机的网络命名空间，这一点可以通过 tshark 证明 （见Q&A）
```

## Q & A 
> Q1: why not **chroot**?
- chroot 只是改变了进程看待路径的“起点”，但并没有改变进程所在的挂载表
- pivot_root 则是彻底更换了整个挂载命名空间的根挂载点。(这一点在使用 nsenter 时十分明显)
```cpp
struct path {
    struct vfsmount *mnt;    // 挂载点信息
    struct dentry *dentry;   // 目录项（含文件系统位置）
};
struct fs_struct {
    int users;               // 引用计数
    spinlock_t lock;         // 自旋锁
    struct path root;        // 该进程认为的根目录 "/"
    struct path pwd;         // 该进程的当前工作目录 "."
};
struct task_struct {
    struct fs_struct *fs;    // 指向进程的文件系统视图
    struct nsproxy *nsproxy; // 指向进程所属的命名空间集合
    ...
};
struct nsproxy {
    struct mnt_namespace *mnt_ns; // 指向所属的 Mount Namespace
    // ... 指向其他类型的命名空间 (uts, ipc, net, pid, ...)
};
struct mnt_namespace {
    struct vfsmount *root;   // 该命名空间的根挂载点
    // ... 其他信息
};
```
- chroot("/new_root") 进程级操作：仅修改了 `current->fs->root`，完全没有触碰 `current->nsproxy->mnt_ns`
- pivot_root("new_root", "put_old") 命名空间级操作：直接作用于Mount Namespace，同时修改`current->fs->root` 和 `current->nsproxy->mnt_ns->root`

> Q2: why must **sudo groupadd mydocker**?
```cpp
const char* SOCKET_PATH = "/var/run/my-docker.sock"; // listen_sock_fd bind()
// 守护进程 root权限 创建的 socket_file rw-r--r--(644) 普通用户无法访问
// 解决方案：`sudo groupadd mydocker` 将用户加入该组，允许特定的普通用户执行特权操作
```

> Q10086: why use **slirp4netns**?
- it work easily（先跑起来好吗doge
```sh
# 我说 wireshark 是对的 :)
sudo nsenter -t $CONTAINER_PID -n tshark -i tap0
Running as user "root" and group "root". This could be dangerous.
Capturing on 'tap0'
 ** (tshark:169494) 17:21:42.594876 [Main MESSAGE] -- Capture started.
 ** (tshark:169494) 17:21:42.595026 [Main MESSAGE] -- File: "/tmp/wireshark_tap0KRRPC3.pcapng"
    1 0.000000000   10.0.2.100 → 8.8.8.8      DNS 72 Standard query 0x90b4 A bilibili.com
    2 0.039073921      8.8.8.8 → 10.0.2.100   DNS 136 Standard query response 0x90b4 A bilibili.com A 8.134.50.24 A 139.159.241.37 A 47.103.24.173 A 119.3.70.188
    3 0.040154421   10.0.2.100 → 8.134.50.24  ICMP 98 Echo (ping) request  id=0x0010, seq=1/256, ttl=64
    4 0.046640025  8.134.50.24 → 10.0.2.100   ICMP 98 Echo (ping) reply    id=0x0010, seq=1/256, ttl=255 (request in 3)
    5 5.031307355 02:b9:e6:90:d0:f0 → 52:55:0a:00:02:02 ARP 42 Who has 10.0.2.2? Tell 10.0.2.100
    6 5.031522555 52:55:0a:00:02:02 → 02:b9:e6:90:d0:f0 ARP 64 10.0.2.2 is at 52:55:0a:00:02:02
```