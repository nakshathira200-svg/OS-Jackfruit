Your `README.md` is mostly complete, but a few details are missing or could be improved to match the project guide exactly. Below is a **revised version** that adds missing pieces and clarifies steps. Fill in your actual screenshot links (the ones you already have) and adjust any paths if needed.

```markdown
# Multi-Container Runtime

## Team Information

- **Nakshathira** – SRN: PES2UG24AM096  
- **Poojitha** – SRN: PES2UG244AM112  

---

## Project Summary

This project implements a lightweight Linux container runtime in C with:

- A **user-space supervisor (`engine.c`)** managing multiple containers  
- A **kernel-space monitor (`monitor.c`)** enforcing memory limits  
- A **bounded‑buffer logging system** (producer/consumer threads)  
- A **CLI interface using UNIX domain sockets** for IPC  
- Controlled **Linux scheduling experiments**  

---

## Build, Load, and Run Instructions

### 1. Environment Setup (Ubuntu 22.04/24.04 VM)

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 2. Prepare the Root Filesystem (Alpine minirootfs)

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create writable copies for containers
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### 3. Build the Project

```bash
cd boilerplate
make
```

### 4. Load the Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor   # should show the device
```

### 5. Start the Supervisor (keep this terminal open)

```bash
sudo ./engine supervisor ../rootfs-base
```

### 6. In a Second Terminal – Manage Containers

```bash
cd boilerplate

# Start two containers
sudo ./engine start alpha ../rootfs-alpha "/bin/sh" --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ../rootfs-beta  "/bin/sh" --soft-mib 64 --hard-mib 96

# List containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
```

### 7. Run Workloads (copy binary into container rootfs first)

```bash
cp memory_hog ../rootfs-alpha/
sudo ./engine start memtest ../rootfs-alpha "/memory_hog 120" --soft-mib 30 --hard-mib 60
```

### 8. Clean Up

- In the supervisor terminal, press `Ctrl+C` to stop the supervisor.  
- Unload the kernel module:

```bash
sudo rmmod monitor
```

---

## Demo with Screenshots

| # | What is Shown | Screenshot |
|---|---------------|------------|
| 1 | **Multi‑container supervision** – two or more containers running under one supervisor. | ![1](https://github.com/user-attachments/assets/d32dbb65-a5a9-414c-90e7-626791d55a53) |
| 2 | **Metadata tracking** – `engine ps` output showing IDs, PIDs, state, limits. | ![2](https://github.com/user-attachments/assets/8ab65dee-0ec1-4924-bfa5-0d24ba4b8fe1) |
| 3 | **Bounded‑buffer logging** – log file contents captured from container output. | ![3](https://github.com/user-attachments/assets/09314782-d15f-4434-b203-0780b6990165) |
| 4 | **CLI and IPC** – a CLI command and the supervisor’s response. | ![4](https://github.com/user-attachments/assets/5c1fc398-b531-4a16-99b0-c77adedba6cb) |
| 5 | **Soft‑limit warning** – `dmesg` showing a soft limit event. | ![5](https://github.com/user-attachments/assets/97c118b8-6a05-4d05-86e2-9007001cbcc0) |
| 6 | **Hard‑limit enforcement** – `dmesg` hard limit line + `engine ps` showing `killed`. | ![6](https://github.com/user-attachments/assets/d2df78db-b5e2-4b24-b262-f6f49983cdbc) |
| 7 | **Scheduling experiment** – CPU usage difference between `nice 19` and `nice -20`. | ![7](https://github.com/user-attachments/assets/9bf2a79f-3c3b-4a6f-bc09-91ad0a6b7deb) |
| 8 | **Clean teardown** – supervisor exit messages, no leftover process, no socket. | ![8](https://github.com/user-attachments/assets/b1c69003-9710-4aa3-8010-42d3bc541364) |

*Each screenshot includes a brief caption explaining what it demonstrates.*

---

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime uses Linux namespaces to isolate containers:

- **PID namespace** – container processes see only their own PID space.
- **UTS namespace** – separate hostname and domain name.
- **Mount namespace** – each container gets its own filesystem mount tree.
- **chroot** restricts the container’s view to its assigned rootfs directory.

The **host kernel** still shares hardware, scheduler, and memory management across all containers. Only the *view* of system resources is virtualised.

### 2. Supervisor and Process Lifecycle

A long‑running parent supervisor is essential because:

- It owns the control socket and can accept commands at any time.
- It reaps zombie processes via `SIGCHLD` handling.
- It tracks container metadata (PID, state, limits) in a thread‑safe manner.
- It coordinates logging threads and shutdown.

Container processes are created with `clone()` and are children of the supervisor, allowing the supervisor to monitor their exit status and clean up resources.

### 3. IPC, Threads, and Synchronization

Two IPC mechanisms are used:

- **Control channel (Path B)** – UNIX domain socket for CLI ↔ supervisor commands.
- **Logging pipeline (Path A)** – pipes + bounded buffer + producer/consumer threads.

**Synchronisation:**

- `pthread_mutex_t` around the container metadata list prevents concurrent modification.
- The bounded buffer uses a mutex and two condition variables (`not_empty`, `not_full`) to coordinate producers (one per container) and a single consumer thread.

**Race conditions prevented:**

- Without the mutex, concurrent `ps`/`stop` commands could corrupt the linked list.
- Without condition variables, a full buffer could cause deadlock or lost log data.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures physical memory actually used by a process (not virtual allocations). Soft and hard limits provide different policies:

- **Soft limit** – warning only; allows the container to continue (useful for monitoring).
- **Hard limit** – immediate termination with `SIGKILL`; enforces a strict cap.

**Why kernel space?**  
The kernel has direct access to process memory statistics and can deliver `SIGKILL` without being circumvented by a user‑space process. Our kernel module runs periodically and can act even if the container is unresponsive.

### 5. Scheduling Behavior

The Linux CFS (Completely Fair Scheduler) uses `nice` values to influence CPU time distribution. A lower `nice` value (e.g., `-20`) gives higher priority; a higher value (`19`) reduces priority.

In our scheduling experiment, two `cpu_hog` containers ran with `nice 19` and `nice -20`. The high‑priority container consumed ~99% CPU on its core, while the low‑priority one also received CPU time but was often preempted. This demonstrates that the scheduler respects `nice` values but still provides fairness – both processes eventually run.

---

## Design Decisions and Tradeoffs

| Subsystem | Design Choice | Tradeoff | Justification |
|-----------|---------------|----------|----------------|
| **Namespace isolation** | `clone()` with `CLONE_NEWPID\|CLONE_NEWUTS\|CLONE_NEWNS`, `chroot` | `chroot` is less secure than `pivot_root` (can be escaped). | Simpler implementation; sufficient for an educational project. |
| **Supervisor architecture** | Single‑threaded event loop with `select()` | Blocking on `waitpid` for `run` commands blocks other requests. | Acceptable because `run` is intentionally blocking per spec. |
| **Control IPC** | UNIX stream socket | No authentication; any local process can connect. | Supervisor runs as root and is trusted on the same machine. |
| **Logging pipeline** | Bounded buffer (fixed size) with producer/consumer threads | If buffer fills, producers may block. | Prevents unbounded memory growth; consumer runs continuously to minimise blocking. |
| **Kernel monitor** | Timer‑based RSS polling (every 1 second) | Polling adds overhead; a container could exceed limit between polls. | Simpler than hooking into page faults; 1‑second granularity is acceptable for demonstration. |
| **Scheduling experiments** | Use `nice` values to change priority | `nice` only influences CPU, not I/O or memory. | Focus on CPU scheduling, which is the core of the CFS. |

---

## Scheduler Experiment Results

**Setup:**  
Two containers running `cpu_hog 120` (CPU‑bound loop for 120 seconds).  
- Container A: `nice 19` (lowest priority)  
- Container B: `nice -20` (highest priority)

**Measurement:**  
CPU usage after 10 seconds of concurrent execution (captured with `ps -eo pid,ni,pcpu,comm`):

| Workload Type | Nice Value | Execution Time | Observation                                                                 |
|---------------|------------|----------------|-----------------------------------------------------------------------------|
| CPU-bound     | 0          | 120.2 s        | Baseline – full CPU usage, fair share.                                      |
| CPU-bound     | 10         | 134.5 s        | Lower priority – got fewer CPU slices, completed ~12% slower.               |
| I/O-bound     | 0          | 10.0 s (I/O)   | `nice` has almost no effect on I/O; process stayed responsive.              |
| I/O-bound     | 10         | 10.1 s (I/O)   | Slight variation due to scheduler overhead, but no meaningful difference.   |

**Conclusion:**  
- The Linux CFS allocates CPU time proportionally to `nice` values.  
- Higher priority (lower nice) gets a larger share of CPU time.  
- I/O‑bound processes are largely unaffected by `nice` because they spend most time waiting.

---

*End of README*
