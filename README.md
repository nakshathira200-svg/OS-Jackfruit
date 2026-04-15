
# Multi-Container Runtime

## Team Information

- Name 1: Nakshathira
- SRN: PES2UG24AM096 
- Name 2: poojitha  
- SRN: PES2UG244AM112 

---

## Project Summary

This project implements a lightweight Linux container runtime in C with:

- A **user-space supervisor (`engine.c`)** managing multiple containers
- A **kernel-space monitor (`monitor.c`)** enforcing memory limits
- A **bounded-buffer logging system**
- A **CLI interface using IPC**
- Controlled **Linux scheduling experiments**

---

## Build, Load, and Run Instructions

### 1. Environment Setup

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
````

---

### 2. Build the Project

```bash
make
```
  
---

### 3. Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

---

### 4. Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create container-specific rootfs:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### 5. Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

### 6. Run Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

---

### 7. CLI Commands

```bash
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```

---

### 8. Run Experiments

```bash
# Copy workload into container
cp workload_binary ./rootfs-alpha/

# Run workloads inside container
```

---

### 9. Cleanup

```bash
sudo rmmod monitor
```

---

## Demo with Screenshots

### 1. Multi-container Supervision

**Description:** Multiple containers running under one supervisor
**Screenshot:**
<img width="626" height="170" alt="Screenshot 2026-04-15 at 5 02 47 PM" src="https://github.com/user-attachments/assets/d32dbb65-a5a9-414c-90e7-626791d55a53" />

---

### 2. Metadata Tracking (`ps`)

**Description:** Output showing container metadata
**Screenshot:**
<img width="631" height="180" alt="Screenshot 2026-04-15 at 5 03 15 PM" src="https://github.com/user-attachments/assets/8ab65dee-0ec1-4924-bfa5-0d24ba4b8fe1" />


---

### 3. Bounded-buffer Logging

**Description:** Logs captured via producer-consumer pipeline
**Screenshot:**
<img width="630" height="436" alt="Screenshot 2026-04-15 at 5 03 26 PM" src="https://github.com/user-attachments/assets/09314782-d15f-4434-b203-0780b6990165" />

---

### 4. CLI and IPC

**Description:** CLI sending command → supervisor response
**Screenshot:**
<img width="625" height="80" alt="Screenshot 2026-04-15 at 5 03 45 PM" src="https://github.com/user-attachments/assets/5c1fc398-b531-4a16-99b0-c77adedba6cb" />

---

### 5. Soft Limit Warning

**Description:** Kernel log showing soft limit exceeded
**Screenshot:**
<img width="632" height="77" alt="Screenshot 2026-04-15 at 5 03 56 PM" src="https://github.com/user-attachments/assets/97c118b8-6a05-4d05-86e2-9007001cbcc0" />

---

### 6. Hard Limit Enforcement

**Description:** Container killed after exceeding hard limit
**Screenshot:**
<img width="634" height="146" alt="Screenshot 2026-04-15 at 5 04 07 PM" src="https://github.com/user-attachments/assets/d2df78db-b5e2-4b24-b262-f6f49983cdbc" />

---

### 7. Scheduling Experiment

**Description:** Observed behavior differences between workloads
**Screenshot:**
<img width="631" height="159" alt="Screenshot 2026-04-15 at 5 04 21 PM" src="https://github.com/user-attachments/assets/9bf2a79f-3c3b-4a6f-bc09-91ad0a6b7deb" />

---

### 8. Clean Teardown

**Description:** No zombies, proper cleanup
**Screenshot:**
<img width="627" height="214" alt="Screenshot 2026-04-15 at 5 04 39 PM" src="https://github.com/user-attachments/assets/b1c69003-9710-4aa3-8010-42d3bc541364" />

---

## Engineering Analysis

### 1. Isolation Mechanisms

* Containers use:

  * PID namespace
  * UTS namespace
  * Mount namespace
* Filesystem isolation via:

  * `chroot` / `pivot_root`
* `/proc` is mounted inside container

**Key Insight:**
Kernel is shared across all containers; only views are isolated.

---

### 2. Supervisor and Process Lifecycle

* Long-running supervisor manages:

  * Container creation via `clone()`
  * Metadata tracking
  * Signal handling (`SIGCHLD`)
* Prevents zombie processes via proper reaping

---

### 3. IPC, Threads, and Synchronization

#### IPC Mechanisms:

* **Control Path:** UNIX socket / FIFO (CLI → supervisor)
* **Logging Path:** Pipes (container → supervisor)

#### Synchronization:

* Mutex for shared metadata
* Condition variables for bounded buffer

#### Race Conditions Prevented:

* Concurrent writes to logs
* Metadata corruption
* Buffer overflow deadlocks

---

### 4. Memory Management and Enforcement

* RSS used to track memory usage
* Kernel module:

  * Tracks PIDs
  * Enforces limits

#### Policies:

* Soft limit → warning
* Hard limit → `SIGKILL`

**Why kernel space?**
User space cannot reliably enforce memory limits at runtime.

---

### 5. Scheduling Behavior

* Experiments with:

  * `nice` values
  * CPU-bound vs I/O-bound workloads

#### Observations:

* Lower nice value → higher CPU share
* I/O-bound processes get better responsiveness

---

## Design Decisions and Tradeoffs

### 1. Namespace Isolation

* Choice: `chroot`
* Tradeoff: Less secure than `pivot_root`
* Reason: Simpler implementation

---

### 2. Supervisor Architecture

* Choice: Single long-running process
* Tradeoff: Single point of failure
* Reason: Centralized control simplifies design

---

### 3. IPC Design

* Choice: UNIX domain sockets
* Tradeoff: Slightly more complex than pipes
* Reason: Bidirectional communication needed

---

### 4. Logging System

* Choice: Bounded buffer with threads
* Tradeoff: Synchronization overhead
* Reason: Prevents data loss and blocking

---

### 5. Kernel Monitor

* Choice: LKM with ioctl
* Tradeoff: Kernel complexity
* Reason: Required for enforcement

---

## Scheduler Experiment Results

| Workload Type | Nice Value | Execution Time | Observation      |
| ------------- | ---------- | -------------- | ---------------- |
| CPU-bound     | 0          | TODO           | Higher CPU usage |
| CPU-bound     | 10         | TODO           | Slower execution |
| I/O-bound     | 0          | TODO           | Responsive       |

**Conclusion:**

* Scheduler prioritizes fairness
* Nice values influence CPU allocation
* I/O-bound processes benefit from scheduling heuristics




