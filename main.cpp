#include "network.h"

#include <csignal>
#include <cstring>
#include <fstream>
#include <grp.h>
#include <termios.h>
#include <vector>

// --- Linux System Call Headers ---
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// 定义子进程的栈空间大小
#define STACK_SIZE (1024 * 1024) // 1 MB
const char* SOCKET_PATH = "/var/run/my-docker.sock";

using namespace std;

// 传递给子进程的参数结构体
struct ChildArgs {
    string cgroup_path;
    int client_sock_fd;
    int sync_pipe_write_fd;
    vector<char*> argv;
};
static void cleanup_proc(void* _arg) { umount("./proc"); }
static void cleanup_cgroup(const string& cgroup_path) { rmdir(cgroup_path.c_str()); }

std::map<pid_t, pid_t> network_processes;
// IPAllocator ip_pool("10.0.0", 2, 254); // 10.0.0.2-254

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

int listen_sock_fd = -1;

// 容器进程的主函数
static int child_main(void* arg) {
    ChildArgs* args = static_cast<ChildArgs*>(arg);
    // close(args->sync_pipe_read_fd); // 子进程关闭读端
    // 1. 暂停自己，并通知父进程可以开始写 map
    char sync_char = 'A';
    write(args->sync_pipe_write_fd, &sync_char, 1);
    close(args->sync_pipe_write_fd);
    raise(SIGSTOP); // 暂停自己
    // // I/O 重定向：将容器的标准输入、输出、错误重定向到客户端 socket
    // dup2(args->client_sock_fd, STDIN_FILENO);
    // dup2(args->client_sock_fd, STDOUT_FILENO);
    // dup2(args->client_sock_fd, STDERR_FILENO);
    // close(args->client_sock_fd);
    cout << "[Container] My PID is: " << getpid() << "\n";

    if (sethostname("my-container", 12)) {
        perror("sethostname failed");
        return 1;
    }
    if (chdir("./my-rootfs") != 0 || chroot(".") != 0) {
        perror("chroot failed");
        return 1;
    }
    if (mount("proc", "./proc", "proc", 0, NULL) != 0) {
        perror("mount proc failed");
        return 1;
    }
    setenv("TERM", "xterm", 1); 
    cout << "[Container] Executing command: " << args->argv[0] << "\n";
    execvp(args->argv[0], args->argv.data());
    // 如果 execvp 成功，下面的代码将不会被执行
    perror("execvp failed");
    return 1;
}

// Daemon: 处理单个客户端连接的函数
void handle_client_connection(int client_sock_fd) {
    char buffer[1024] = {0};
    read(client_sock_fd, static_cast<char*>(buffer), sizeof(buffer)); // 读取客户端发来的命令
    string cmd_str(static_cast<char*>(buffer));
    cout << "[Daemon] Received command: " << cmd_str << "\n";
    // 解析命令 (简单实现)
    vector<string> parts;
    char* token = strtok(static_cast<char*>(buffer), " ");
    while (token != NULL) {
        parts.push_back(string(token));
        token = strtok(NULL, " ");
    }
    if (parts.empty() || parts[0] != "run") {
        write(client_sock_fd, "Invalid command\n", 16);
        close(client_sock_fd);
        return;
    }
    // --- 创建容器 ---
    char* stack = new char[STACK_SIZE];
    char* stack_top = stack + STACK_SIZE;
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe failed");
        return;
    }
    ChildArgs child_args;
    child_args.client_sock_fd = client_sock_fd;
    child_args.sync_pipe_write_fd = pipefd[1];
    for (size_t i = 1; i < parts.size(); ++i) {
        child_args.argv.push_back(strdup(parts[i].c_str()));
    }
    child_args.argv.push_back(nullptr);
    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWUSER | CLONE_NEWNET | SIGCHLD;
    pid_t child_pid = clone(child_main, stack_top, clone_flags, &child_args);
    if (child_pid == -1) {
        perror("[Daemon] clone failed");
        close(client_sock_fd);
        delete[] stack;
        return;
    }
    close(pipefd[1]); // 父进程关闭写端

    // --- 父进程 (Handler): 设置网络并等待容器退出 ---
    cout << "[Daemon] Created container with PID: " << child_pid << "\n";
    char sync_buf;
    read(pipefd[0], &sync_buf, 1); // 等待子进程，直到它准备好
    close(pipefd[0]);
    setup_uid_gid_maps(child_pid);
    child_args.cgroup_path = setup_cgroups(child_pid);
    setup_network(child_pid);
    kill(child_pid, SIGCONT);
    // 等待子进程退出
    int status;
    waitpid(child_pid, &status, 0);
    if (WIFEXITED(status)) {
        cout << "[Host] Container process exited with status: " << WEXITSTATUS(status) << "\n";
    } else if (WIFSIGNALED(status)) {
        cout << "[Host] Container process killed by signal: " << WTERMSIG(status) << "\n";
    }
    cleanup_network(child_pid);
    cleanup_cgroup(child_args.cgroup_path);
    close(client_sock_fd);
    delete[] stack;
}

// Daemon 的主函数
void daemon_main() {
    if (getuid() != 0) {
        cerr << "Daemon must be run as root.\n";
        return;
    }
    cout << "[Daemon] Starting...\n";
    listen_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_sock_fd < 0) {
        perror("socket failed");
        return;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(static_cast<char*>(addr.sun_path), SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH); // 如果 socket 文件已存在，先删除
    if (bind(listen_sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return;
    }
    struct group* grp = getgrnam("mydocker");
    if (grp == NULL) {
        perror("getgrnam failed: group 'mydocker' not found. Please create it.");
        close(listen_sock_fd);
        unlink(SOCKET_PATH);
        return;
    }
    gid_t gid = grp->gr_gid;
    if (chown(SOCKET_PATH, -1, gid) < 0) { // -1 不改变所有者 UID
        perror("chown for socket failed");
        close(listen_sock_fd);
        unlink(SOCKET_PATH);
        return;
    }
    if (chmod(SOCKET_PATH, 0660) < 0) {
        perror("chmod for socket failed");
        close(listen_sock_fd);
        unlink(SOCKET_PATH);
        return;
    }
    if (listen(listen_sock_fd, 5) < 0) {
        perror("listen failed");
        return;
    }
    cout << "[Daemon] Listening on " << SOCKET_PATH << "\n";
    while (true) {
        int client_sock_fd = accept(listen_sock_fd, NULL, NULL);
        if (client_sock_fd < 0) {
            perror("accept failed");
            continue;
        }
        cout << "[Daemon] Accepted new connection." << "\n";
        // Fork 一个子进程来处理这个连接，主进程继续监听
        pid_t handler_pid = fork();
        if (handler_pid == 0) {    // 子进程 (Handler)
            close(listen_sock_fd); // Handler 不需要监听
            handle_client_connection(client_sock_fd);
            exit(0);
        } else if (handler_pid > 0) { // 父进程 (Daemon)
            close(client_sock_fd);    // Daemon 不需要与客户端直接通信
            // 可以选择等待 handler 结束，或者让它成为僵尸进程由 init 接管
            waitpid(handler_pid, NULL, WNOHANG);
        } else {
            perror("fork for handler failed");
            close(client_sock_fd);
        }
    }
}

// Client 的主函数
void client_main(int argc, char* argv[]) {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket failed");
        return;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect failed. Is the daemon running with sudo?");
        // sudo groupadd mydocker
        // sudo usermod -aG mydocker $USER
        return;
    }

    // 将命令拼接成一个字符串发送
    string command;
    for (int i = 1; i < argc; ++i) {
        command += argv[i];
        if (i < argc - 1) {
            command += " ";
        }
    }
    // command += "\n";
    write(sock_fd, command.c_str(), command.length());
    // --- 使用 fork 实现双向通信，避免 I/O 死锁 ---
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork for client failed");
        close(sock_fd);
        return;
    }
    if (pid == 0) {
        // --- 子进程: 负责将用户输入(STDIN)转发到 socket ---
        char buffer[1024];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
            if (write(sock_fd, buffer, n) != n) {
                perror("write to socket failed");
                break;
            }
        }
        exit(0);
    } else {
        // --- 父进程: 负责将 socket 的数据转发到终端(STDOUT) ---
        char buffer[1024];
        ssize_t n;
        while ((n = read(sock_fd, buffer, sizeof(buffer))) > 0) {
            if (write(STDOUT_FILENO, buffer, n) != n) {
                perror("write to stdout failed");
                break;
            }
        }
        // 当 socket 连接被服务器端关闭 (例如容器退出), read 会返回 0
        // 此时我们需要杀死负责读取用户输入的子进程，否则它会一直卡住
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0); // 等待子进程结束
    }
    close(sock_fd);
}

// 优雅地关闭 Daemon
void cleanup_daemon(int sig) {
    cout << "\n[Daemon] Shutting down because of sig " << sig << " (" << strsignal(sig) << ")\n";
    if (listen_sock_fd != -1) {
        close(listen_sock_fd);
    }
    unlink(SOCKET_PATH);
    exit(0);
}

int main(int argc, char* argv[]) {
    if (argc == 1) {
        signal(SIGINT, cleanup_daemon);
        signal(SIGTERM, cleanup_daemon);
        daemon_main();
    } else if (argc < 3 || std::string(argv[1]) != "run") {
        std::cerr << "Usage: " << argv[0] << " run <command>\n";
        return 1;
    } else {
        client_main(argc, argv);
    }
    return 0;
}