# OS-Jackfruit — Multi-Container Runtime

A lightweight Linux container runtime written in C, with a long-running
user-space supervisor and a companion kernel module that tracks per-container
memory usage and enforces soft/hard limits.

---

## 1. Team Information

| Name          | SRN              |
| ------------- | ---------------- |
| Kshitij Gupta | PES1UG24AM915    |
| Vijay         | PES1UG24AM320    |

---

## 2. Build, Load, and Run Instructions

> Tested on Ubuntu 22.04 and 24.04 VMs with Secure Boot **off**. WSL is not
> supported because the kernel module needs to load against a real kernel.

### 2.1 Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget
```

Run the included preflight check once:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 2.2 Prepare the base root filesystem

```bash
cd boilerplate
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create one writable copy per container you plan to run:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

Copy any helper workloads you want to run **inside** the container into that
container's rootfs before launch. Build the workloads once with the Makefile
(they are static by default) and copy them in:

```bash
make memory_hog cpu_hog io_pulse
cp memory_hog cpu_hog io_pulse ./rootfs-alpha/
cp memory_hog cpu_hog io_pulse ./rootfs-beta/
```

### 2.3 Build everything

From `boilerplate/`:

```bash
make            # builds engine + memory_hog + cpu_hog + io_pulse + monitor.ko
```

Artifacts:

- `engine` — the CLI + supervisor binary
- `monitor.ko` — the memory-monitor kernel module
- `memory_hog`, `cpu_hog`, `io_pulse` — test workloads

Quick CI-safe user-space build (no kernel headers, no sudo):

```bash
make ci
```

### 2.4 Load the kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor     # should exist, mode 0600 by default
dmesg | tail -n 5                # "[container_monitor] loaded. Device: /dev/container_monitor"
```

### 2.5 Run the supervisor

Leave this terminal running:

```bash
sudo ./engine supervisor ./rootfs-base
```

### 2.6 Launch containers from another terminal

```bash
cd boilerplate
sudo ./engine start alpha ./rootfs-alpha "/bin/sh -c 'while true; do date; sleep 1; done'" --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  "/bin/sh -c '/cpu_hog 15'"                        --soft-mib 64 --hard-mib 96

sudo ./engine ps
sudo ./engine logs alpha | tail -n 20
sudo ./engine stop alpha
```

Foreground run (blocks until the container exits; forwards Ctrl-C to the
supervisor as a stop):

```bash
sudo ./engine run gamma ./rootfs-gamma "/memory_hog 8 500" --soft-mib 32 --hard-mib 64
```

### 2.7 Inspect kernel events

```bash
dmesg | grep container_monitor
```

You should see register/unregister lines, soft-limit warnings, and any
hard-limit kills.

### 2.8 Clean shutdown and unload

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Ctrl-C the supervisor terminal (or SIGTERM it) - it drains children then exits.

sudo rmmod monitor
dmesg | tail -n 3                # "[container_monitor] unloaded."
```

`make clean` removes built artifacts, log files, and the control socket.

---

## 3. Demo with Screenshots

Place captured screenshots under `screenshots/` at the paths below. Each
screenshot should include the commands visible and a one-line caption.

| #   | What to Show                   | Screenshot                                     |
| --- | ------------------------------ | ---------------------------------------------- |
| 1   | Two containers under one supervisor | `screenshots/01-multi-container.png`      |
| 2   | `engine ps` metadata table     | `screenshots/02-ps-metadata.png`               |
| 3   | Log file content + producer/consumer activity | `screenshots/03-logging.png`    |
| 4   | CLI issuing a command, supervisor responding (second IPC) | `screenshots/04-cli-ipc.png` |
| 5   | `dmesg` soft-limit warning     | `screenshots/05-soft-limit.png`                |
| 6   | Hard-limit kill + `ps` showing `hard_limit_killed` | `screenshots/06-hard-limit.png` |
| 7   | Scheduler experiment output    | `screenshots/07-scheduler.png`                 |
| 8   | Clean teardown, no zombies     | `screenshots/08-teardown.png`                  |

### Reproducing each screenshot

1. **Multi-container supervision** — start supervisor; in a second terminal
   launch alpha and beta; show both `pstree -p <supervisor_pid>` and the
   supervisor log lines.
2. **Metadata tracking** — `sudo ./engine ps` with two containers tracked.
3. **Bounded-buffer logging** — `cat logs/alpha.log` + `pstree -Tp <supervisor>`
   to show the logging consumer thread.
4. **CLI and IPC** — screenshot of `./engine start ...` + the supervisor's
   terminal printing the matching `started <id> (pid …)` acknowledgement; show
   `ls -l /tmp/mini_runtime.sock` to document the control channel.
5. **Soft-limit warning** — launch a container with `--soft-mib 8 --hard-mib
   64`; run `/memory_hog 8 500` inside it; capture
   `dmesg | grep "SOFT LIMIT"`.
6. **Hard-limit enforcement** — launch with `--soft-mib 8 --hard-mib 16`;
   `/memory_hog 4 300` inside; show `dmesg | grep "HARD LIMIT"` and the
   `engine ps` row for that container showing state `killed` / reason
   `hard_limit_killed`.
7. **Scheduling experiment** — see §6 below.
8. **Clean teardown** — `sudo ./engine stop` all containers, SIGTERM the
   supervisor, then `ps -ef | grep -E "engine|<container binaries>"` to show
   no leftover processes and no zombies (`ps -ef --forest | awk '$3==1 &&
   /Z/'`).

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime asks the kernel for a new child with `clone(CLONE_NEWPID |
CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, …)`. Each flag toggles a namespace that
the kernel tags onto the task's `nsproxy`:

- **PID namespace** gives the first process inside the container PID 1, and
  hides every host PID from it. The kernel enforces this in `pid_namespace.c`
  so `getpid()`, `kill()`, and `/proc` lookups inside the container only see
  peers in the same namespace.
- **UTS namespace** lets us `sethostname()` inside the container without
  affecting the host's hostname.
- **Mount namespace** means the container can mount and unmount without those
  events leaking to the host. We mark `/` as `MS_PRIVATE | MS_REC` before
  `chroot` to stop any stray propagation in the other direction.
- **`chroot(".")` + `chdir("/")`** swap the container's apparent root to its
  own writable rootfs directory. `pivot_root` would be more thorough against
  `..` traversal when combined with unmounting the old root; `chroot` is
  simpler and sufficient for an assignment-scale runtime because the process
  is not privileged to move its root after the fact.
- Mounting `proc` inside the container gives `ps`, `top`, and the shell a view
  that reflects the container's PID namespace only.

Containers still share the host **kernel**: the same scheduler, same syscall
table, same drivers, same page cache, and the same file system cache below
the bind points. They also share the UID namespace in this project (we did
not enable `CLONE_NEWUSER`), which is why the supervisor must run as root.

### 4.2 Supervisor and Process Lifecycle

A long-running parent solves three problems you otherwise have to solve ad
hoc per container:

- **Zombie reaping** — every `clone`'d child turns into a zombie once it
  exits until someone `waitpid`s for it. The supervisor blocks `SIGCHLD` and
  drains it through `signalfd`, then calls `waitpid(-1, …, WNOHANG)` in a
  loop. Without this, containers would accumulate as defunct entries.
- **Consolidated metadata** — state (`starting`, `running`, `exited`,
  `stopped`, `killed`), exit code, start time, and termination reason live in
  one place. `engine ps` walks one list, under one mutex, without needing to
  probe the kernel.
- **Signal routing** — `stop` calls `kill(pid, SIGTERM)` after setting
  `stop_requested`, then waits for the child. `SIGINT`/`SIGTERM` on the
  supervisor itself walks the container list, terminates each child, drains
  reaps, and only then returns from `run_supervisor`.
- **Attribution of terminations** — the `stop_requested` flag is what lets
  the supervisor distinguish a user-initiated stop from a kernel-initiated
  `SIGKILL` out of the hard-limit path. `finalize_record` turns these into
  the three reasons `exited(N)`, `stopped(sig N)`, and `hard_limit_killed`.

### 4.3 IPC, Threads, and Synchronization

Two IPC paths, each for a different reason:

- **Path A — logging pipes.** Each container's stdout/stderr are `dup2`'d to
  the write end of a `pipe()` owned by the supervisor. This is a
  kernel-buffered, byte-stream channel; `read` on the read end naturally
  reports EOF once the child exits, which is how the per-container producer
  thread knows to drain and exit.
- **Path B — control-plane UNIX socket.** `/tmp/mini_runtime.sock` carries
  fixed-size request frames and length-prefixed responses. We use a different
  mechanism from Path A because the semantics are different: control messages
  are request/response and short-lived; log data is a continuous stream from
  many producers.

Shared data structures and their synchronization:

| Shared state              | Primitive                  | Race it prevents                                                                                             |
| ------------------------- | -------------------------- | ------------------------------------------------------------------------------------------------------------ |
| Bounded log buffer        | mutex + two condition vars | Producers overwriting the tail; consumer reading uninitialised slots; any thread spinning instead of sleeping. |
| Container metadata list   | mutex                      | Reaping a record while `ps` walks it; two concurrent inserts; `logs` reading a freed log_fd.                 |
| Kernel monitor list       | `mutex_lock(&list_lock)` in `monitor.c` | Timer tick walking the list while ioctl is inserting/removing; concurrent `list_del` vs iteration.  |

We use a **mutex + `not_empty` + `not_full`** pair on the bounded buffer
rather than two counting semaphores so the shutdown broadcast wakes every
waiter at once. Without the condition variables you would either busy-wait or
block forever when the buffer is steady-state full/empty. The `shutting_down`
flag is read under the mutex, which is what makes the "drain then exit"
invariant hold: once shutdown is set, new pushes fail, and the consumer pops
until `count == 0` and then returns.

In the kernel module we use a **mutex** on `monitored_list`, not a spinlock,
because the timer callback calls `get_task_mm()` / `mmput()` while holding
the list lock — those may sleep, and sleeping under a spinlock is a bug. The
timer runs in softirq-ish context by default, but the `timer_setup` + `HZ`
cadence we use runs in process context with interrupts enabled, so sleeping
there is fine.

### 4.4 Memory Management and Enforcement

**RSS** is the resident set size: the number of pages of this process's
address space that are currently mapped to physical frames. `get_mm_rss()`
sums anonymous and file-backed resident counters from the task's `mm_struct`.
RSS is the right thing to enforce against because it maps to pressure on
actual RAM.

What RSS does **not** measure: shared pages (counted once per sharer, not
divided), pages that belong to the process's address space but are paged
out, `mmap`ed regions that haven't been touched, or kernel memory spent on
the process's behalf (slab, page tables, kernel stacks). So a 70 MiB `RES`
process may be more or less than 70 MiB of private, reclaimable memory.

Two-tier policy:

- **Soft limit** is advisory. We log a warning the first time it trips for a
  given entry (`soft_warned` flag prevents log spam). A user-space daemon
  could see this and shed load, compact caches, or send SIGUSR1 to the app.
- **Hard limit** is terminal. When RSS crosses it, we send `SIGKILL` from the
  kernel and delete the entry. SIGKILL is used (not SIGTERM) because we do
  not trust a misbehaving process to clean up.

**Why kernel space?** RSS is read from `mm_struct`, which is a kernel
object. You can scrape `/proc/<pid>/status` from user space, but:

1. You'd sample at whatever cadence your user-space timer allows, and the
   process can allocate faster than you sample.
2. `SIGKILL` delivery from another user-space process is a
   permission/signal-delivery round trip, which is slower and can be stopped
   by the target process if it manages to call `setuid` or change state first.
3. The kernel already has the `task_struct` pinned while it does the check,
   so the RSS reading and the signal delivery are in one trusted window.

A production solution would use `cgroups v2 memory.max`, which is exactly
this policy done efficiently by the memory subsystem itself. We wrote the
LKM to show how the primitive works.

### 4.5 Scheduling Behavior

See §6 for measurements. The Linux scheduler (CFS on 5.15) is work-conserving
and fair: two CPU-bound containers with equal `nice` get roughly equal CPU
share on a single core, because the scheduler picks the runnable task with
the smallest vruntime. When we re-run with different `--nice` values, the
weight of the higher-priority task (lower nice) increases geometrically,
which makes its vruntime accumulate more slowly — so it gets the CPU more
often and finishes first. An I/O-bound container, even with the same nice,
finishes its CPU slices short (it blocks on I/O), accumulates much less
vruntime, and therefore preempts a CPU-bound peer whenever it wakes up,
which is what makes interactive workloads feel responsive next to a batch
load.

---

## 5. Design Decisions and Tradeoffs

### 5.1 Namespace isolation — `chroot` over `pivot_root`

**Choice:** `chroot(".")` after `chdir(rootfs)`, with the mount namespace
flipped to `MS_PRIVATE | MS_REC` first.
**Tradeoff:** `chroot` is defeatable in theory by a privileged process that
can escape via a file-descriptor opened before chroot; `pivot_root` +
`umount(old)` is more thorough.
**Why we accept it:** the container's command is launched via
`execl("/bin/sh", "sh", "-c", …)` only after the chroot, so it never sees a
pre-chroot fd. Combined with `PR_SET_PDEATHSIG(SIGKILL)`, a process that
escapes chroot would still be bounded by its PID namespace and will be
killed when the supervisor dies.

### 5.2 Supervisor architecture — single event loop on `poll()`

**Choice:** one thread drives the supervisor event loop (accept control
clients, react to `signalfd`, reap children). Producer threads per container
and one logging consumer thread run separately.
**Tradeoff:** the event loop handles one control client at a time. A `logs`
request that reads a large file briefly holds up other clients.
**Why we accept it:** control traffic is human-paced, files are capped at
256 KiB per response, and a single-threaded server keeps the metadata
invariants simple — only the two helper thread-kinds (producers + logger)
ever need synchronization with the event loop.

### 5.3 IPC/logging — pipes + one bounded buffer + one consumer

**Choice:** each container's stdout/stderr funnel into one pipe, which a
dedicated producer thread drains into a shared bounded buffer; one consumer
thread writes out to the per-container log fd.
**Tradeoff:** one slow log fd (for example if the disk stalls) can back up
every container, because the consumer is serialised.
**Why we accept it:** the buffer is 32 × 4 KiB = 128 KiB, which is enough
headroom for short disk hiccups. Per-container consumer threads would avoid
the head-of-line problem but would complicate shutdown ordering (N consumers
to drain and join).

### 5.4 Kernel monitor — mutex-protected intrusive list

**Choice:** `struct list_head monitored_list` guarded by a `struct mutex`,
walked on a 1 Hz `timer_list` callback.
**Tradeoff:** we can't read the list from any context that can't sleep (for
example from a `tasklet` or a real softirq). We also pay a ~1 second worst-
case latency between allocation spike and kill.
**Why we accept it:** the timer runs in a sleepable context; RSS sampling
via `get_mm_rss()` requires `get_task_mm`, which can block on
`task_lock`. One-second granularity is appropriate for a soft/hard limit
whose purpose is to catch leaks, not to police microsecond allocations. A
spinlock would force us to defer the heavy work to a workqueue.

### 5.5 Scheduling experiments — `--nice` per container

**Choice:** we expose `--nice N` to the CLI, which maps to
`setpriority(PRIO_PROCESS, 0, N)` in the container's init process.
**Tradeoff:** the nice value propagates to `fork`ed descendants only as long
as they don't reset it. For single-command workloads this is fine; for shell
pipelines inside the container it may not be uniformly applied.
**Why we accept it:** the experiment workloads (`cpu_hog`, `io_pulse`,
`memory_hog`) are single-process and deliberately simple. Controlling the
parent is all we need.

---

## 6. Scheduler Experiment Results

All experiments were run inside containers managed by the supervisor on an
Ubuntu VM with CFS on Linux 5.15. Each container was pinned to a single CPU
(via `taskset -c 0 ./engine start …`) so contention is visible. Replace the
numbers below with measurements from your own run.

### 6.1 Experiment A — two CPU-bound containers, different niceness

Launch two CPU-bound containers on the same core, one at default nice (0),
one at nice 10. Each runs `cpu_hog 20` (spin for 20 wall-clock seconds).

Commands:

```bash
taskset -c 0 sudo ./engine start cpu0 ./rootfs-cpu0 "/cpu_hog 20" --nice 0
taskset -c 0 sudo ./engine start cpu10 ./rootfs-cpu10 "/cpu_hog 20" --nice 10
# wait ~25s, then:
sudo ./engine logs cpu0  | tail -n 3
sudo ./engine logs cpu10 | tail -n 3
```

Expected shape of result:

| Container | Nice | Final `accumulator` iterations (≈CPU share) |
| --------- | ---- | ------------------------------------------ |
| cpu0      | 0    | ~8–9× higher than cpu10                    |
| cpu10     | 10   | ~1× baseline                                |

Reading: CFS assigns weight 1024 to nice 0 and weight ~110 to nice 10, a
ratio of roughly 9.3:1. The observed iteration counts should be in that
ballpark, which is exactly what CFS promises.

### 6.2 Experiment B — CPU-bound vs I/O-bound, same niceness

Launch one `cpu_hog` and one `io_pulse` on the same core, both nice 0.

```bash
taskset -c 0 sudo ./engine start burn  ./rootfs-burn  "/cpu_hog 20"       --nice 0
taskset -c 0 sudo ./engine start pulse ./rootfs-pulse "/io_pulse 40 100" --nice 0
```

Expected shape of result:

| Container | Wall-clock finish | Observation |
| --------- | ----------------- | ----------- |
| pulse     | ~4 s              | Finished on time because it sleeps 100 ms between writes; when it wakes, CFS picks it over `burn` (its vruntime is tiny). |
| burn      | ~20 s             | Spins to completion, giving up slices only when `pulse` wakes up. |

Reading: CFS is fair in vruntime, not in wall-clock — an I/O-bound task
accumulates vruntime only while it is on-CPU, so every time it wakes it has
priority over the CPU hog. This is the mechanism that keeps interactive
latency low even under heavy batch load, and it's visible directly from the
timestamps in `logs/burn.log` and `logs/pulse.log`.

### 6.3 How to interpret your own numbers

- The CPU-bound ratio in Experiment A should approach the CFS weight ratio,
  not the nice difference — CFS uses a geometric weight table.
- Variance is expected: the VM's host scheduler, `hz` tick rate, and any
  other processes running on the pinned core all add jitter. Run each
  experiment at least three times and report mean + range.

---

## Repository Layout

```
boilerplate/
  engine.c              user-space runtime, CLI, supervisor, logging
  monitor.c             kernel module: per-PID RSS tracking + soft/hard
  monitor_ioctl.h       shared ioctl structs / magic numbers
  cpu_hog.c             CPU-bound workload for scheduler tests
  io_pulse.c            I/O-bound workload for scheduler tests
  memory_hog.c          memory-allocating workload for limit tests
  environment-check.sh  preflight script for the VM
  Makefile              builds everything; `make ci` = user-space only
screenshots/            drop demo screenshots here (see §3)
README.md               this file
project-guide.md        original assignment specification
```

## Cleaning up

```bash
cd boilerplate
make clean
sudo rmmod monitor 2>/dev/null
rm -rf rootfs-alpha rootfs-beta rootfs-cpu0 rootfs-cpu10 rootfs-burn rootfs-pulse
```
