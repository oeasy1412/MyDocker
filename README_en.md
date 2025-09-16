# MyDocker: A Minimal Container Engine in C++

Welcome to **MyDocker**, a hands-on project to build a simple yet functional container runtime from scratch using C++. This project is designed as an educational tool to demystify what happens under the hood when you type `docker run`.

## 🌟 Key Docker Features Simulated

This project doesn't aim to be a Docker replacement but focuses on simulating the core mechanics of containerization.
1.  **Namespaces (Isolation)**
    *   **PID Namespace (`CLONE_NEWPID`):** Containers have their own process tree, starting from PID 1. Inside the container, you can only see its own processes.
    *   **Mount Namespace (`CLONE_NEWNS`):** Each container gets an isolated filesystem view. We achieve this using the `chroot` system call and by mounting a dedicated `/proc` filesystem.
    *   **UTS Namespace (`CLONE_NEWUTS`):** Allows each container to have its own unique hostname.
    *   **Network Namespace (`CLONE_NEWNET`):** Provides network isolation. We simulate rootless networking using **`slirp4netns`**, giving containers internet access without requiring root privileges for network configuration.
    *   **User Namespace (`CLONE_NEWUSER`):** Maps the container's root user (UID 0) to a non-privileged user on the host, significantly enhancing security. This is the foundation of "rootless" containers.

2.  **Cgroups (Resource Limiting)**
    *   We utilize **Control Groups (v2)** to enforce resource constraints on containers.
    *   **Memory Limit:** Containers are restricted to a specific amount of memory (e.g., 100MB).
    *   **CPU Limit:** Container CPU usage is throttled to a predefined quota (e.g., 50% of one CPU core).

3.  **Client-Server Architecture**
    *   The project mimics Docker's `dockerd` (daemon) and `docker` (client) model.
    *   **Daemon (`sudo ./my-docker`):** A root-privileged background process that manages the container lifecycle. It listens for commands on a Unix domain socket (`/var/run/my-docker.sock`).
    *   **Client (`./my-docker run ...`):** A non-privileged command-line tool that communicates with the daemon to create and interact with containers.

4.  **Interactive TTY (Pseudo-terminal)**
    *   We implement a fully interactive pseudo-terminal (PTY) for containers.
    *   This allows for a true interactive shell experience, supporting features like command history, line editing, and running full-screen applications like `vim` and `top`.
    *   I/O is bidirectionally forwarded between the client's terminal and the container's PTY using `poll()`.

5.  **Filesystem Layering (Conceptual)**
    *   While not implementing a full union filesystem like OverlayFS, the project uses a pre-prepared root filesystem (`rootfs`) directory. This simulates the concept of a container image's filesystem layer, providing the container with its own isolated root directory.

## 🎓 Educational Value

The primary goal of MyDocker is learning. By building this project, you will gain a deep, practical understanding of:

*   **Linux System Calls:** Direct usage of `clone()`, `chroot()`, `mount()`, `sethostname()`, and more.
*   **Process Management:** Working with `fork()`, `execvp()`, `waitpid()`, and signal handling.
*   **Inter-Process Communication (IPC):** Using Unix domain sockets for C/S communication and pipes for parent-child process synchronization.
*   **Container Primitives:** How namespaces and cgroups are the fundamental building blocks of all container technologies.
*   **Advanced I/O:** Implementing non-blocking, bidirectional I/O forwarding with `poll()` and managing pseudo-terminals (`pty`).
*   **Security Concepts:** The critical difference between rootful and rootless containers, and the role of user namespaces in mitigating security risks.

## ⚠️ Limitations

This is an educational project and lacks many features of a production-ready container engine:

*   **No Image Management:** It doesn't have a concept of container images, layers, or a registry. The `rootfs` must be manually prepared on the host.
*   **No Union Filesystem:** It doesn't use OverlayFS or similar technologies. Changes inside the container are written directly to the `rootfs` directory.
*   **Simplified Networking:** While `slirp4netns` provides internet access, there is no support for creating complex container networks, port mapping, or DNS services.
*   **No Persistence:** There is no volume management. All data is ephemeral.
*   **Basic CLI:** The command-line interface is very minimal.
*   **Error Handling & Robustness:** Error handling is basic and not hardened for production use.

## 🚀 How to Use

### Prerequisites

*   A Linux environment.
*   C++ compiler (`g++`), `make`.
*   Required tools: `slirp4netns`.

### 1. Setup Environment
First, install the necessary dependencies.
```bash
sudo apt-get update
sudo apt-get install slirp4netns
```

### 2. Prepare the Root Filesystem (`rootfs`)
This step creates a minimal rootfs that our container will use as its root directory.
```bash
make build
```

### 3. Setup Permissions (Docker Group Equivalent)
Our daemon requires a special group to grant non-root users permission to the socket.
```bash
# Create a 'mydocker' group
sudo groupadd mydocker
# Add your current user to this group
sudo usermod -aG mydocker $USER
# IMPORTANT: For the group change to take effect, you must
# log out and log back in, or run 'newgrp mydocker' in your terminal.
```

### 5. Run MyDocker!
You'll need **two terminals**.

**In Terminal 1 (Start the Daemon):**
Run the daemon with `sudo`. It will start listening for client connections.

```sh
# sudo ./build/my-docker
make rund
```
```sh
# Expected Output:
[Daemon] Starting...
[Daemon] Listening on /var/run/my-docker.sock
```

**In Terminal 2 (Run a Container):**
Run the client as a **normal user**. It will connect to the daemon and start an interactive shell inside a new container.
```bash
# ./my-docker run /bin/sh
make run
```
```sh
# You should now be inside the container's shell!
root@my-container:/# 
```

**Inside the container, you can verify the isolation:**
```bash
# Check PID (should be 1)
root@my-container:/# echo $$
# Check processes (should only see container's processes)
root@my-container:/# ps aux
# Check hostname
root@my-container:/# hostname
# Exit the container
root@my-container:/# exit
```

To stop the daemon, go back to **Terminal 1** and press `Ctrl+C`.