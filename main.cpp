#include "network.h"

#include <fstream>

// --- Linux System Call Headers ---
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// 定义子进程的栈空间大小
#define STACK_SIZE (1024 * 1024) // 1 MB

using namespace std;

// 传递给子进程的参数结构体
struct ChildArgs {
    char** argv;
    string cgroup_path;
};
static void cleanup_proc(void* _arg) { umount("./proc"); }
static void cleanup_cgroup(const string& cgroup_path) { rmdir(cgroup_path.c_str()); }

map<pid_t, NetworkConfig> network_configs;
IPAllocator ip_pool("10.0.0", 2, 254); // 10.0.0.2-254

// 创建 cgroup 并设置资源限制
static string setup_cgroups(pid_t child_pid) {
    string cgroup_path = "/sys/fs/cgroup/my_container_" + to_string(child_pid);
    // 创建 cgroup 目录
    mkdir(cgroup_path.c_str(), 0755);
    // 设置内存限制为 100MB
    ofstream mem_file(cgroup_path + "/memory.max");
    mem_file << "100M";
    mem_file.close();
    // 设置 CPU 限制为 50%
    ofstream cpu_file(cgroup_path + "/cpu.max");
    cpu_file << "50000 100000"; // 50% of CPU time
    cpu_file.close();
    // 添加进程到 cgroup
    ofstream procs_file(cgroup_path + "/cgroup.procs");
    procs_file << child_pid;
    procs_file.close();
    return cgroup_path;
}

// 设置用户命名空间映射
static void setup_uid_gid_maps(pid_t child_pid) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    // 写入 UID 映射
    string uid_map_path = "/proc/" + to_string(child_pid) + "/uid_map";
    ofstream uid_map(uid_map_path);
    uid_map << "0 " << uid << " 1";
    uid_map.close();
    // 写入 GID 映射
    string gid_map_path = "/proc/" + to_string(child_pid) + "/gid_map";
    ofstream gid_map(gid_map_path);
    gid_map << "0 " << gid << " 1";
    gid_map.close();
}

// 子进程 (容器) 将要执行的主函数
static int child_main(void* arg) {
    ChildArgs* args = static_cast<ChildArgs*>(arg);
    cout << "[Container] My PID is: " << getpid() << "\n";
    cout << "[Container] Setting hostname...\n";
    if (sethostname("my-container", 12)) {
        perror("sethostname failed");
        return 1;
    }
    cout << "[Container] Changing to rootfs directory...\n";
    if (chdir("./my-rootfs") != 0) {
        perror("chdir failed");
        return 1;
    }
    // 挂载新的 /proc 文件系统
    // 如果不这样做，在容器里执行 ps aux 看到的会是宿主机的进程
    cout << "[Container] Mounting /proc...\n";
    if (mount("proc", "./proc", "proc", 0, NULL) != 0) {
        perror("mount proc failed");
        return 1;
    }
    pthread_cleanup_push(cleanup_proc, nullptr);
    // 切换根目录 (chroot)
    cout << "[Container] Changing root...\n";
    if (chroot(".") != 0) {
        perror("chroot failed");
        return 1;
    }
    // 注意：这需要在容器内执行，因为我们需要修改容器内的映射文件
    setup_uid_gid_maps(getpid());
    // 设置控制终端
    if (isatty(STDIN_FILENO)) {
        setsid();
        ioctl(STDIN_FILENO, TIOCSCTTY, 1);
    }
    // 执行用户指定的命令
    cout << "[Container] Executing command: " << args->argv[0] << "\n";
    execvp(args->argv[0], args->argv);
    // 如果 execvp 成功，下面的代码将不会被执行
    perror("execvp failed");
    pthread_cleanup_pop(1);
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc < 3 || string(argv[1]) != "run") {
        cerr << "Usage: " << argv[0] << " run <command>\n";
        return 1;
    }
    cout << "[Host] Parent process started, PID: " << getpid() << "\n";
    // 准备子进程的栈
    char* stack = new char[STACK_SIZE];
    char* stack_top = stack + STACK_SIZE; // 栈是从高地址向低地址增长的
    // 准备传递给子进程的参数
    ChildArgs child_args;
    child_args.argv = &argv[2];
    // 定义我们希望为子进程创建的命名空间
    // CLONE_NEWPID: 新的 PID 命名空间，容器内进程号从 1 开始
    // CLONE_NEWNS:  新的 Mount 命名空间，容器内的挂载操作不影响宿主机
    // CLONE_NEWUTS: 新的 UTS 命名空间，可以设置独立的主机名
    int clone_flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | SIGCHLD | CLONE_NEWIPC;
    // 使用 clone() 创建子进程
    pid_t child_pid = clone(child_main, stack_top, clone_flags, &child_args);
    if (child_pid == -1) {
        perror("clone failed");
        delete[] stack;
        return 1;
    }
    cout << "[Host] Created container process with PID: " << child_pid << "\n";
    // 设置用户命名空间映射（在宿主机端）
    setup_uid_gid_maps(child_pid);
    // 设置网络命名空间
    setup_network(child_pid);
    // 设置 cgroups 资源限制
    child_args.cgroup_path = setup_cgroups(child_pid);
    // 等待子进程退出
    int status;
    waitpid(child_pid, &status, 0);
    if (WIFEXITED(status)) {
        cout << "[Host] Container process exited with status: " << WEXITSTATUS(status) << "\n";
    } else if (WIFSIGNALED(status)) {
        cout << "[Host] Container process killed by signal: " << WTERMSIG(status) << "\n";
    }
    // 清理资源
    cleanup_network(child_pid);
    cleanup_cgroup(child_args.cgroup_path);
    delete[] stack;
    return 0;
}