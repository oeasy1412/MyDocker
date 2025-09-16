#ifndef _MY_NETWORK_
#define _MY_NETWORK_

#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// 存储 容器PID -> slirp4netns进程PID 的映射
extern std::map<pid_t, pid_t> network_processes;

// 创建 veth pair 并配置网络 (slirp4netns 版本)
// 我们不再需要复杂的 NetworkConfig，只需要知道容器的 PID
void setup_network(pid_t child_pid) {
    std::cout << "[Host] Setting up network for container PID " << child_pid << " using slirp4netns...\n";
    pid_t slirp_pid = fork();
    if (slirp_pid < 0) {
        perror("fork failed for slirp4netns");
        return;
    }
    if (slirp_pid == 0) {
        std::string pid_str = std::to_string(child_pid);
        char* args[] = {(char*)"slirp4netns",
                        (char*)"--configure",             // 让 slirp4netns 自动配置 TAP 设备
                        (char*)"--disable-host-loopback", // 禁止从容器访问宿主机的 loopback 地址
                        (char*)pid_str.c_str(),           // 目标网络命名空间 (容器的PID)
                        (char*)"tap0",                    // 在容器内创建的 TAP 设备名
                        NULL};
        execvp(args[0], const_cast<char**>(args));
        // 如果成功，这之后的代码都不会被执行
        perror("execvp for slirp4netns failed");
        exit(1);
    } else {
        std::cout << "[Host] Started slirp4netns process with PID: " << slirp_pid << "\n";
        network_processes[child_pid] = slirp_pid;
    }
}

// 清理网络资源
void cleanup_network(pid_t child_pid) {
    if (network_processes.find(child_pid) == network_processes.end()) {
        std::cerr << "[Host] No network process found for container PID: " << child_pid << "\n";
        return;
    }
    pid_t slirp_pid = network_processes[child_pid];
    std::cout << "[Host] Cleaning up network for container PID: " << child_pid
              << " (killing slirp4netns PID: " << slirp_pid << ")\n";
    // 只需要杀死对应的 slirp4netns 进程，它所管理的网络命名空间和设备会自动清理
    if (kill(slirp_pid, SIGTERM) != 0) {
        perror("Failed to kill slirp4netns process");
    }
    // 等待进程真正退出
    waitpid(slirp_pid, NULL, 0);
    network_processes.erase(child_pid);
}

#endif // _MY_NETWORK_