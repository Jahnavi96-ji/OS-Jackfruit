# OS-Jackfruit — Mini Container Runtime

> A lightweight Docker-like container runtime built from scratch in C, featuring Linux namespaces, a kernel memory monitor module, bounded-buffer logging, and a full CLI.

---

## Team Details

| Name | SRN | GitHub |
|------|-----|--------|
| Jahnavi | PES1UG24CS196 | [Jahnavi96-ji](https://github.com/Jahnavi96-ji) |
| J Rishitha Radhae | PES1UG24CS193| rishii2908 |

---

## Project Overview

OS-Jackfruit is a mini container runtime that mimics core Docker functionality using low-level Linux system calls. It consists of two major components:

| Component | File | Description |
|-----------|------|-------------|
| User-space runtime | `engine.c` | Supervisor process, CLI, logging, IPC |
| Kernel module | `monitor.c` | Memory monitoring via ioctl, RSS tracking |

---

## Screenshots

### Screenshot 1 — Two Containers Running
![Two Containers Running](screenshots/screenshot1_two_containers_running.png)

### Screenshot 2 — CLI ps with Metadata
![PS Metadata](screenshots/screenshot2_ps_metadata.png)

### Screenshot 3 — Log Output
![Log Output](screenshots/screenshot3_logs_output.png)

### Screenshot 4 — Stop Containers
![Stop Containers](screenshots/screenshot4_stop_containers.png)

### Screenshot 5 — Soft Limit in dmesg
![Soft Limit](screenshots/screenshot5_soft_limit_dmesg.png)

### Screenshot 6 — Hard Limit + Container Killed
![Hard Limit](screenshots/screenshot6_hard_limit_killed.png)

### Screenshot 7 — Scheduling Difference
![Scheduling](screenshots/screenshot7_scheduling_difference.png)

### Screenshot 8 — No Zombies
![No Zombies](screenshots/screenshot8_no_zombies.png)

---

## How to Run

### Prerequisites
- Ubuntu 22.04 or 24.04 (bare metal or VM, NOT WSL)
- Secure Boot disabled (required for kernel module)
- gcc-12, build-essential, linux-headers

### Step 1: Clone and Setup
```bash
git clone https://github.com/Jahnavi96-ji/OS-Jackfruit.git
cd OS-Jackfruit
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) gcc-12
```

### Step 2: Prepare Root Filesystems
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Step 3: Build
```bash
cd boilerplate
make clean
make
```

### Step 4: Load Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Step 5: Start Supervisor (Terminal 1)
```bash
sudo ./engine supervisor ../rootfs-base
```

### Step 6: Use CLI (Terminal 2)
```bash
# Start containers
sudo ./engine start alpha ../rootfs-alpha /looper
sudo ./engine start beta  ../rootfs-beta  /looper

# List containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha

# Stop containers
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Step 7: Memory Limit Test
```bash
cp memory_hog ../rootfs-alpha/
sudo ./engine start memtest ../rootfs-alpha /memory_hog --soft-mib 5 --hard-mib 10
sleep 5
sudo dmesg | grep container_monitor
sudo ./engine ps
```

### Step 8: Scheduling Experiment
```bash
cp cpu_hog ../rootfs-alpha/
cat > ../rootfs-alpha/cpu_hog30 << 'EOF'
#!/bin/sh
/cpu_hog 30
EOF
chmod +x ../rootfs-alpha/cpu_hog30

sudo ./engine start cpu-normal ../rootfs-alpha /cpu_hog30 --nice 0
sudo ./engine start cpu-low    ../rootfs-alpha /cpu_hog30 --nice 19

sleep 35
sudo ./engine logs cpu-normal
sudo ./engine logs cpu-low
```

### Step 9: Unload Module
```bash
sudo rmmod monitor
```

---

## Task Descriptions

### Task 1 — Multi-Container Supervisor
The supervisor process uses `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` to create isolated containers. Each container gets its own PID namespace, UTS namespace (hostname), and mount namespace. `chroot()` isolates the filesystem to the container's rootfs. Zombie reaping is handled by `waitpid(-1, WNOHANG)` in the event loop.

### Task 2 — CLI (start, stop, ps, logs)
A UNIX domain socket at `/tmp/mini_runtime.sock` connects the CLI client to the supervisor. Each command (start, stop, ps, logs) sends a `control_request_t` struct and receives a `control_response_t`. The supervisor dispatches requests in its event loop using `select()`.

### Task 3 — Bounded-Buffer Logging
A classic producer-consumer bounded buffer with capacity 16 is used for log routing. Container stdout/stderr is captured via a pipe. A dedicated pipe-reader thread acts as producer, pushing `log_item_t` chunks into the buffer. The logging consumer thread pops chunks and writes them to per-container log files in `logs/<id>.log`. Synchronization uses a mutex + two condition variables (`not_empty`, `not_full`).

### Task 4 — Kernel Module Memory Monitor
A Linux kernel module (`monitor.ko`) registers a character device at `/dev/container_monitor`. The engine registers each container's PID via `ioctl(MONITOR_REGISTER)`. A kernel timer fires every second, iterates the monitored list, checks RSS via `get_mm_rss()`, emits a `SOFT LIMIT` warning via `printk` once per container, and sends `SIGKILL` + logs a `HARD LIMIT` message when the hard limit is exceeded.

### Task 5 — Scheduler Experiments
Two containers run the same CPU-bound workload (`cpu_hog`) at different scheduler priorities using `nice()`. The container with `--nice 0` (normal priority) reports every second consistently. The container with `--nice 19` (lowest priority) shows skipped seconds in its logs, demonstrating that the Linux CFS scheduler allocates less CPU time to lower-priority processes.

### Task 6 — Clean Teardown
On `SIGINT`/`SIGTERM`, the supervisor sends `SIGKILL` to all running containers, calls `waitpid()` until no children remain (eliminating zombies), drains the bounded buffer, joins the logging thread, closes all file descriptors, and unlinks the control socket. The kernel module's exit function safely frees all list nodes under a spinlock before deregistering the device.

---

## Design Decisions

### Spinlock vs Mutex in monitor.c
A **spinlock** (`spinlock_t`) was chosen over a mutex for the monitored list in the kernel module. The timer callback runs in **softirq context** (atomic context), where sleeping is forbidden. Mutexes can sleep, making them unsafe here. `spin_lock_irqsave` / `spin_unlock_irqrestore` are used throughout to safely protect the list across both the softirq timer path and the process-context ioctl path.

### UNIX Domain Socket for IPC
A UNIX domain socket was chosen for the control plane because it supports bidirectional, structured communication between the supervisor and CLI clients. It allows multiple clients to connect concurrently and is simpler to use than FIFOs for request-response patterns.

### Pipe per Container for Logging
Each container gets its own pipe. The write end is passed to the child via `dup2()` before `execv()`. The read end is drained by a dedicated per-container pipe-reader thread in the supervisor. This decouples log I/O from the main event loop and prevents any single container from blocking others.

### `list_for_each_entry_safe` in Kernel Module
The timer callback uses `list_for_each_entry_safe` instead of `list_for_each_entry` because entries are deleted during iteration (when a process exits or hits the hard limit). The `_safe` variant saves the next pointer before processing each entry, preventing use-after-free bugs.

---

## References

- [Linux `clone(2)` man page](https://man7.org/linux/man-pages/man2/clone.2.html)
- [Linux Kernel Module Programming Guide](https://sysprog21.github.io/lkmpg/)
- [Linux `ioctl(2)` man page](https://man7.org/linux/man-pages/man2/ioctl.2.html)
- [POSIX Threads (pthreads) Documentation](https://man7.org/linux/man-pages/man7/pthreads.7.html)
- [Linux Completely Fair Scheduler (CFS)](https://www.kernel.org/doc/html/latest/scheduler/sched-design-CFS.html)
- [Alpine Linux Mini Root Filesystem](https://alpinelinux.org/downloads/)
- [Original Project Repository](https://github.com/shivangjhalani/OS-Jackfruit)
