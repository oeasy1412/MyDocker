#ifndef _MY_NETWORK_
#define _MY_NETWORK_

#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>

struct NetworkConfig {
    std::string host_if; // 宿主机端接口名
    std::string cont_if; // 容器端接口名
    std::string cont_ip; // 容器IP地址
    std::string subnet;  // 子网
    pid_t child_pid;     // 容器PID
};

class IPAllocator {
  private:
    std::string base_ip;
    int cur;
    int max;
    std::set<int> used_ips;

  public:
    IPAllocator(std::string base, int st, int end) : base_ip(std::move(base)), cur(st), max(end) {}
    std::string allocate() {
        while (used_ips.find(cur) != used_ips.end()) {
            cur = (cur + 1) % max;
            if (cur == 0) {
                cur = 1;
            }
        }
        used_ips.insert(cur);
        std::string ip = base_ip + "." + std::to_string(cur);
        return ip;
    }
    void release(int host) { used_ips.erase(host); }
};

extern std::map<pid_t, NetworkConfig> network_configs;
extern IPAllocator ip_pool;

// 创建 veth pair 并配置网络
void setup_network(pid_t child_pid) {
    // 创建 veth pair
    NetworkConfig config;
    config.child_pid = child_pid;
    config.host_if = "veth_host_" + std::to_string(child_pid);
    config.cont_if = "veth_cont_" + std::to_string(child_pid);
    config.cont_ip = ip_pool.allocate();
    config.subnet = "10.0.0.0/24";
    // 使用 system 命令简化网络设置
    std::string cmd = "ip link add " + config.host_if + " type veth peer name " + config.cont_if;
    system(cmd.c_str());
    // 将容器端 veth 放入容器的网络命名空间
    cmd = "ip link set " + config.cont_if + " netns " + std::to_string(child_pid);
    system(cmd.c_str());
    // 配置宿主机端
    cmd = "ip link set " + config.host_if + " up";
    system(cmd.c_str());
    // 配置容器端（在容器的网络命名空间中执行）
    cmd = "nsenter -t " + std::to_string(child_pid) + " -n ip link set " + config.cont_if + " up";
    system(cmd.c_str());
    // 为容器端分配 IP 地址
    cmd = "nsenter -t " + std::to_string(child_pid) + " -n ip addr add " + config.cont_ip + "/24 dev " + config.cont_if;
    system(cmd.c_str());
    // 设置默认路由
    cmd = "nsenter -t " + std::to_string(child_pid) + " -n ip route add default via 10.0.0.1";
    system(cmd.c_str());
    // 配置宿主机端路由
    cmd = "ip addr add 10.0.0.1/24 dev " + config.host_if;
    system(cmd.c_str());
    // 启用 IP 转发
    system("sysctl -w net.ipv4.ip_forward=1");
    // 设置 NAT
    cmd = "iptables -t nat -A POSTROUTING -s " + config.subnet + " -j MASQUERADE";
    system(cmd.c_str());

    network_configs[child_pid] = config;
}

// 清理网络资源
void cleanup_network(pid_t child_pid) {
    if (network_configs.find(child_pid) == network_configs.end()) {
        std::cerr << "[Host] No network config found for PID: " << child_pid << "\n";
        return;
    }
    NetworkConfig& config = network_configs[child_pid];
    std::cout << "[Host] Cleaning up network for container PID: " << child_pid << "\n";
    // 删除 NAT 规则
    std::string del_nat_cmd = "iptables -t nat -D POSTROUTING -s " + config.subnet +
                              " -j MASQUERADE -m comment --comment \"container_" + std::to_string(child_pid) + "\"";
    system(del_nat_cmd.c_str());
    // 删除宿主机端接口
    std::string del_if_cmd = "ip link delete " + config.host_if;
    system(del_if_cmd.c_str());
    // 从映射中移除
    network_configs.erase(child_pid);
    size_t pos = config.cont_ip.find_last_of('.');
    int host_part = stoi(config.cont_ip.substr(pos+1));
    ip_pool.release(host_part);
}

#endif // _MY_NETWORK_