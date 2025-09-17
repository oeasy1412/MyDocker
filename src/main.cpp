#include "network.h"

#include <csignal>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>
// pty
#include <grp.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <utmp.h>
// Linux sysCall
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// 定义子进程的栈空间大小
#define STACK_SIZE (1024 * 1024)                     // 1 MB
const char* SOCKET_PATH = "/var/run/my-docker.sock"; // listen_sock_fd bind()

using namespace std;

// 传递给子进程的参数结构体
struct ChildArgs {
    string cgroup_path;
    int pty_slave_fd = -1;       // PTY 从设备端的文件描述符，将成为容器的控制终端
    int sync_pipe_write_fd = -1; // 用于父子进程同步的管道写端
    vector<char*> argv;
};

// 全局监听套接字
int listen_sock_fd = -1;
// 全局变量，存储 (容器PID -> slirp4netns进程PID) 的映射
std::map<pid_t, pid_t> network_processes;
struct termios original_termios;

void restore_terminal_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    cout << "\n[Client] Terminal mode restored.\n";
}

static void cleanup_cgroup(const string& cgroup_path) { rmdir(cgroup_path.c_str()); }

// 创建 cgroup 并设置资源限制
static string setup_cgroups(pid_t child_pid) {
    string cgroup_path = "/sys/fs/cgroup/my_container_" + to_string(child_pid);
    mkdir(cgroup_path.c_str(), 0755);
    // 设置内存限制为 100MB
    ofstream mem_file(cgroup_path + "/memory.max");
    if (mem_file.is_open()) {
        mem_file << "100M";
        mem_file.close();
    }
    // 设置 CPU 限制为 50%
    ofstream cpu_file(cgroup_path + "/cpu.max");
    if (cpu_file.is_open()) {
        cpu_file << "50000 100000"; // 50% of CPU time
        cpu_file.close();
    }
    // 将容器进程添加到 cgroup
    ofstream procs_file(cgroup_path + "/cgroup.procs", std::ios_base::app);
    if (procs_file.is_open()) {
        procs_file << child_pid;
        procs_file.close();
    }
    return cgroup_path;
}

// 设置用户命名空间映射
static void setup_uid_gid_maps(pid_t child_pid) {
    // 确保在写入前，/proc/sys/user/max_user_namespaces 的值大于0
    // 并且当前用户在 /etc/subuid 和 /etc/subgid 中有映射范围
    // 禁用 setgroups，这是写入 gid_map 的前提
    uid_t uid = getuid();
    gid_t gid = getgid();
    // 写入 UID 映射
    string uid_map_path = "/proc/" + to_string(child_pid) + "/uid_map";
    ofstream uid_map(uid_map_path);
    if (uid_map.is_open()) {
        uid_map << "0 " << uid << " 1";
        uid_map.close();
    } else {
        perror("Failed to open uid_map");
    }
    uid_map.close();
    // 写入 GID 映射
    string gid_map_path = "/proc/" + to_string(child_pid) + "/gid_map";
    ofstream gid_map(gid_map_path);
    if (gid_map.is_open()) {
        gid_map << "0 " << gid << " 1";
        gid_map.close();
    } else {
        perror("Failed to open gid_map");
    }
}

// 容器进程的主函数
static int child_main(void* arg) {
    ChildArgs* args = static_cast<ChildArgs*>(arg);
    // 同步: 通过管道向父进程发送一个字节
    char sync_char = 'A';
    if (write(args->sync_pipe_write_fd, &sync_char, 1) < 0) {
    }
    close(args->sync_pipe_write_fd);
    // 同步: 暂停自己
    raise(SIGSTOP);

    // 将 `PTY从设备端` 设置为本进程的控制终端
    // 这个函数会自动处理 setsid(), ioctl(TIOCSCTTY), dup2() 等操作
    // 将进程的标准输入、输出、错误重定向到 pty_slave_fd
    if (login_tty(args->pty_slave_fd) < 0) {
        perror("login_tty failed");
        exit(1);
    }
    cout << "[Container] My PID is: " << getpid() << "\n";
    setenv("TERM", "xterm", 1);
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
    // 执行用户指定的命令
    cout << "[Container] Executing command: " << args->argv[0] << "\n";
    execvp(args->argv[0], args->argv.data());
    // 如果 execvp 成功，下面的代码将不会被执行
    perror("execvp failed");
    return 1;
}

// Daemon: 处理单个客户端连接的函数
void handle_client_connection(int client_sock_fd) {
    // 先接收窗口大小
    struct winsize win_size;
    if (read(client_sock_fd, &win_size, sizeof(win_size)) != sizeof(win_size)) {
        perror("[Daemon] read winsize failed");
        close(client_sock_fd);
        return;
    }
    // 读取客户端发来的命令
    char buffer[1024] = {0};
    if (read(client_sock_fd, static_cast<char*>(buffer), sizeof(buffer)) < 0) {
        perror("read failed");
        return;
    }
    string cmd_str(static_cast<char*>(buffer));
    cout << "[Daemon] Received command: " << cmd_str << "\n";
    // 解析命令 (简单实现)
    vector<string> parts;
    char* token = strtok(static_cast<char*>(buffer), " \n\r\t");
    while (token != NULL) {
        parts.push_back(string(token));
        token = strtok(NULL, " \n\r\t");
    }
    if (parts.empty() || parts[0] != "run") {
        if (write(client_sock_fd, "Invalid command\n", 16) < 0) {
        }
        close(client_sock_fd);
        return;
    }
    // 创建 PTY
    // master_fd 留在父进程(Handler)，用于和容器通信
    // slave_fd  交给子进程(Container)，成为其控制终端
    int master_fd = -1, slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) {
        perror("openpty failed");
        close(client_sock_fd);
        close(master_fd);
        close(slave_fd);
        return;
    }
    cout << "[Daemon] Received window size: (" << win_size.ws_row << " rows," << win_size.ws_col << " cols)\n";
    if (ioctl(master_fd, TIOCSWINSZ, &win_size) < 0) {
        perror("ioctl TIOCSWINSZ failed");
    }
    // 创建用于父子进程同步的管道
    int pipefd[2];
    if (pipe(static_cast<int*>(pipefd)) < 0) {
        perror("pipe failed");
        return;
    }
    // 创建容器
    std::unique_ptr<char[]> stack = std::make_unique<char[]>(STACK_SIZE);
    char* stack_top = stack.get() + STACK_SIZE;
    ChildArgs child_args;
    child_args.pty_slave_fd = slave_fd;
    child_args.sync_pipe_write_fd = pipefd[1];
    for (size_t i = 1; i < parts.size(); ++i) {
        child_args.argv.push_back(const_cast<char*>(parts[i].c_str()));
    }
    child_args.argv.push_back(nullptr);
    // clone 参数
    int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWUSER | CLONE_NEWNET | SIGCHLD;
    pid_t child_pid = clone(child_main, stack_top, clone_flags, &child_args);
    // 父进程 (Handler) 清理
    close(slave_fd);
    close(pipefd[1]);
    if (child_pid == -1) {
        perror("[Daemon] clone failed");
        close(client_sock_fd);
        close(master_fd);
        close(slave_fd);
        return;
    }
    // 父进程 (Handler) 设置网络并等待容器退出
    cout << "[Daemon] Created container with PID: " << child_pid << "\n";
    // 同步: 等待子进程，直到它准备好
    char sync_buf;
    if (read(pipefd[0], &sync_buf, 1) < 0) {
        perror("read failed");
        return;
    }
    close(pipefd[0]);
    // 同步: 子进程已暂停
    // string container_pts_path = "./my-rootfs/dev/pts/0";
    setup_uid_gid_maps(child_pid);
    child_args.cgroup_path = setup_cgroups(child_pid);
    setup_network(child_pid);
    // 同步: 唤醒子进程
    kill(child_pid, SIGCONT);

    // 使用 poll 同时监听客户端和容器终端
    struct pollfd fds[2];
    fds[0].fd = client_sock_fd; // 监听来自客户端的数据
    fds[0].events = POLLIN;
    fds[1].fd = master_fd; // 监听来自容器 PTY 的数据
    fds[1].events = POLLIN;
    while (true) {
        if (poll(static_cast<pollfd*>(fds), 2, -1) < 0) {
            if (errno == EINTR)
                continue; // 被信号中断，正常，继续
            perror("poll failed");
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(client_sock_fd, static_cast<char*>(buffer), sizeof(buffer));
            if (n <= 0)
                break; // 客户端断开
            if (write(master_fd, static_cast<char*>(buffer), n) < 0)
                break; // 写入容器失败
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(master_fd, static_cast<char*>(buffer), sizeof(buffer));
            if (n <= 0)
                break; // 容器退出，PTY关闭
            if (write(client_sock_fd, static_cast<char*>(buffer), n) < 0)
                break; // 写入客户端失败
        }
    }
    kill(child_pid, SIGKILL);    // 确保容器进程被杀死
    waitpid(child_pid, NULL, 0); // 回收僵尸进程
    cleanup_network(child_pid);
    usleep(10000); // 等待内核清理cgroup
    cleanup_cgroup(child_args.cgroup_path);
    close(client_sock_fd);
    close(master_fd);
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
        // poll 循环中 Handler与客户端之间的 socket
        int client_sock_fd = accept(listen_sock_fd, NULL, NULL);
        if (client_sock_fd < 0) {
            perror("accept failed");
            continue;
        }
        cout << "[Daemon] Accepted new connection.\n";
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
    strncpy(static_cast<char*>(addr.sun_path), SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect failed. Is the daemon running and are you in groups mydocker?");
        // sudo groupadd mydocker
        // sudo usermod -aG mydocker $USER
        return;
    }
    // 将本地终端设置为原始模式
    if (isatty(STDIN_FILENO) == 0) {
        cerr << "Not a terminal.\n";
        return;
    }
    if (tcgetattr(STDIN_FILENO, &original_termios) < 0) { // 保存终端设置
        perror("tcgetattr failed");
        return;
    }
    // 注册退出函数，确保终端模式一定会被恢复
    atexit(restore_terminal_mode);
    struct termios raw = original_termios;
    // cfmakeraw 会关闭回显(ECHO)，关闭规范模式(ICANON)等
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        perror("tcsetattr failed");
        return;
    }
    // 获取并发送窗口大小
    struct winsize win_size;
    ioctl(STDIN_FILENO, TIOCGWINSZ, &win_size);
    if (write(sock_fd, &win_size, sizeof(win_size)) != sizeof(win_size)) {
        perror("write winsize failed");
        close(sock_fd);
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
    if (write(sock_fd, command.c_str(), command.length())) {
    }
    // 使用 fork 实现双向通信，避免 I/O 死锁
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork for client failed");
        close(sock_fd);
        return;
    }
    if (pid == 0) {
        // 子进程: 负责将用户输入(STDIN)转发到 socket
        char buffer[1024];
        ssize_t n;
        while ((n = read(STDIN_FILENO, static_cast<char*>(buffer), sizeof(buffer))) > 0) {
            if (write(sock_fd, static_cast<char*>(buffer), n) != n) {
                perror("write to socket failed");
                break;
            }
        }
        exit(0);
    } else {
        // 父进程: 负责将 socket 的数据转发到终端(STDOUT)
        char buffer[1024];
        ssize_t n;
        while ((n = read(sock_fd, static_cast<char*>(buffer), sizeof(buffer))) > 0) {
            if (write(STDOUT_FILENO, static_cast<char*>(buffer), n) != n) {
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
        // [Daemon 模式] 如果没有参数，则作为守护进程启动
        signal(SIGINT, cleanup_daemon);
        signal(SIGTERM, cleanup_daemon);
        daemon_main();
    } else if (argc < 3 || std::string(argv[1]) != "run") {
        std::cerr << "Usage: " << argv[0] << " run <command>\n";
        return 1;
    } else {
        // [Client 模式] 作为客户端启动
        client_main(argc, argv);
    }
    return 0;
}