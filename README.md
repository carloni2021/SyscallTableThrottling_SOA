# Syscall Throttling — Linux Kernel Module

A Linux kernel module that intercepts selected system calls and enforces a per-window rate limit on specific programs and/or users. When the limit is exceeded, the calling thread is blocked until the next time window opens.

Built on top of the syscall-table discoverer by [Prof. Francesco Quaglia](https://github.com/FrancescoQuaglia/Linux-sys_call_table-discoverer) as part of the Advanced Operating Systems course.

---

## Table of Contents

- [Quick Start](#quick-start)
- [Overview](#overview)
- [Architecture](#architecture)
- [Design Decisions](#design-decisions)
  - [Syscall Table Discovery](#1-syscall-table-discovery)
  - [Kernel ≥ 5.15: x64_sys_call Patching](#2-kernel--515-x64_sys_call-patching)
  - [Process Identification via Inode](#3-process-identification-via-inode)
  - [Data Structures](#4-data-structures)
  - [Throttling Algorithm](#5-throttling-algorithm)
  - [Blocking vs Non-Blocking Syscalls](#6-blocking-vs-non-blocking-syscalls)
  - [Thundering Herd Prevention](#7-thundering-herd-prevention)
  - [Drain Protocol for Safe Unload](#8-drain-protocol-for-safe-unload)
  - [Write-Protection Bypass](#9-write-protection-bypass)
- [User Interface](#user-interface)
- [Build & Usage](#build--usage)
- [Testing & Validation](#testing--validation)
- [Kernel Version Compatibility](#kernel-version-compatibility)
- [File Structure](#file-structure)

---

## Quick Start

```bash
# Build module and userspace tools
make

# Load module and create device node
make load

# Register a program, a syscall and a UID, set limit, enable
sudo ./throttleClient add_prog /bin/bash
sudo ./throttleClient add_sys 1          # sys_write (nr=1)
sudo ./throttleClient add_uid 1000
sudo ./throttleClient set_max 5          # 5 calls/second
sudo ./throttleClient monitor 1

# Watch a loop being throttled (run in another terminal)
while true; do echo hello; done

# Query statistics
./throttleClient stats

# Unload
make unload
```

---

## Overview

The module allows a privileged user to:

1. Register one or more **programs** (by path) and/or **UIDs** to monitor.
2. Register one or more **syscall numbers** to intercept.
3. Set a **maximum call rate** (calls per second, default 100).
4. **Enable monitoring**: any registered program/UID invoking a monitored syscall beyond the rate limit is transparently blocked until the next 1-second window.
5. Query **statistics**: peak blocking delay, average blocking delay, peak/average number of simultaneously blocked threads.

The throttle applies when **both** conditions hold:
```
syscall_is_registered(nr)  AND  (prog_is_registered(exe) OR uid_is_registered(euid))
```

---

## Architecture

```
userspace (throttleClient)
        │  ioctl
        ▼
/dev/throttleDriver  (throttle_dev.c)
        │
        ▼
config_lock + hashtables + bitmap  (throttle_main.c / throttle_core.c)
        │
        ▼
sys_call_table[nr] = generic_sct_wrapper  (throttle_hook.c)
        │
        ├─ throttle_check()   → block if over limit
        │       │
        │       └─ throttle_wq (wait queue, woken by hrtimer every 1s)
        │
        └─ orig_fn(regs)      → original kernel handler
```

---

## Design Decisions

### 1. Syscall Table Discovery

The kernel does not export the address of `sys_call_table`. Rather than relying on `kallsyms_lookup_name` (not available to modules since kernel 5.7) or any exported symbol, the module uses a **hardware-only page-table walk** starting from `CR3`.

**How it works (`throttle_mem.c`, `throttle_discovery.c`):**

- `sys_vtpmo(vaddr)` walks the four-level x86-64 page table (PML4 → PDP → PDE → PTE) reading the physical address from `CR3` directly, returning the physical frame number or `NO_MAP` if the page is not present.
- `find_sys_call_table()` scans the kernel virtual address range `[0xffffffff00000000, 0xfffffffffff00000)` page by page. For each mapped page, `sct_validate_page()` checks whether 7 well-known indices of `sys_ni_syscall` (134, 174, 182, 183, 214, 215, 236) all point to the same address — a pattern stable across kernel versions that uniquely identifies the syscall table.

**Why this approach:**  
It requires no exported kernel symbols and no `/proc/kallsyms` parsing. It works on kernels with `CONFIG_KALLSYMS_ALL=n` and survives KASLR because the scan covers the entire possible range. (A `kprobe` is used separately for `x64_sys_call` on kernel ≥ 5.15 — see below.)

---

### 2. Kernel ≥ 5.15: x64_sys_call Patching

Starting from Linux 5.15, the syscall dispatch path changed:

```
Before 5.15:   entry_SYSCALL_64 → sys_call_table[nr](regs)
From   5.15:   entry_SYSCALL_64 → do_syscall_64 → x64_sys_call(regs, nr)
```

`x64_sys_call` is a compiler-generated `switch` statement — it does **not** use `sys_call_table`. Replacing entries in `sys_call_table` alone is no longer sufficient to intercept syscalls on modern kernels.

**Solution: redirect `x64_sys_call` to our (hooked) syscall table via an external stub.**

#### Step 1 — Find `x64_sys_call` via kprobe

`x64_sys_call` is not exported. The module resolves its address using a `kprobe` — the same approach used by [Prof. Quaglia](https://github.com/FrancescoQuaglia/Linux-sys_call_table-discoverer) and [F-masci](https://github.com/F-masci/syscall-throttling):

```c
struct kprobe kp = { .symbol_name = "x64_sys_call" };
register_kprobe(&kp);   // kp.addr is now resolved
unregister_kprobe(&kp); // unregistered immediately — no overhead at runtime
```

The kprobe is registered and unregistered in a single step during `module_init`. No kprobe remains active after the address is captured, so there is zero runtime overhead from the probe.

The same helper (`lookup_unexported`) is reused on kernel ≥ 6.4 to resolve `execmem_alloc`, `execmem_free`, and `set_memory_x`, which are also no longer exported in those versions.

#### Step 2 — External stub

We patch the first 5 bytes of `x64_sys_call` with a `JMP rel32` pointing to a small **external stub** — self-contained machine code allocated with `module_alloc()` *outside* the module's own memory.

If `CONFIG_RETPOLINE` is enabled, a 19-byte Spectre v2-safe stub is used (resolved at init via the same `lookup_unexported` helper):

```asm
movabs r10, <sys_call_table_addr>   ; 49 BA [8 bytes]
mov    rax, [r10 + rsi*8]          ; 49 8B 04 F2  — sys_call_table[nr]
jmp    __x86_indirect_thunk_rax    ; E9 [4 bytes]  — retpoline
```

Otherwise, the 14-byte direct dispatch is used:

```asm
movabs r10, <sys_call_table_addr>   ; 49 BA [8 bytes]
jmpq   *(%r10, %rsi, 8)            ; 41 FF 24 F2
```

`rsi` holds the syscall number per the x86-64 ABI used by `do_syscall_64`. The stub dispatches directly through `sys_call_table[nr]`, which already contains our `generic_sct_wrapper` for hooked syscalls and the original handler for all others.

**Why allocate the stub outside the module?**  
Module memory is freed by the kernel immediately after `module_exit()` returns. If the stub lived inside the module, any CPU still executing it after `rmmod` would fault. Allocating it separately with `module_alloc()` gives us full control over its lifetime: we free it explicitly after the drain protocol guarantees no CPU is inside it.

**Why `stop_machine` for patching?**  
Writing 5 bytes to kernel text is not atomic. `stop_machine()` halts all other CPUs, serialises the write, and ensures no CPU is executing the patched region while another is modifying it. CR0.WP is disabled only inside the stopped context.

---

### 3. Process Identification via Inode

To determine whether a syscall invocation comes from a monitored program, the module reads `current->mm->exe_file` and extracts the **inode number + device ID** of the executable.

**Why not `current->comm`?**  
`comm` is a 16-byte string that can be modified by the process itself (`prctl(PR_SET_NAME, ...)`), is truncated, and is not unique across different executables with the same name. Using inode+device provides a stable, filesystem-level identity that:
- Survives binary renames.
- Distinguishes hard links on different filesystems.
- Cannot be spoofed by the monitored process.

`mmap_read_lock(mm)` is held while reading `exe_file` to prevent concurrent `execve` or `exit_mm` from invalidating the pointer.

---

### 4. Data Structures

The module maintains three independent lookup structures for the monitoring set:

| What | Structure | Lookup key | Complexity |
|------|-----------|------------|------------|
| Monitored programs | Kernel hashtable (`DEFINE_HASHTABLE`, 256 buckets) | inode number | O(1) avg |
| Monitored UIDs | Kernel hashtable (`DEFINE_HASHTABLE`, 256 buckets) | uid value | O(1) avg |
| Monitored syscalls | Kernel bitmap (`DECLARE_BITMAP`, NR_syscalls bits) | syscall number | O(1) |

The bitmap is the most important one for the hot path: `test_bit(nr, syscall_bitmap)` is a single bitwise instruction with no memory allocation, executed on every syscall invocation before any locking.

The matching logic in `throttle_check()` is:

```
throttle applies  ⟺  test_bit(nr, syscall_bitmap)
                  AND (inode matches prog_table OR euid matches uid_table)
```

---

### 5. Throttling Algorithm

The module uses a **fixed-window counter** approach:

- A single `atomic_t call_count` tracks calls in the current window.
- An `hrtimer` fires every 1 second (`CLOCK_MONOTONIC`) and atomically resets `call_count` to 0.
- On each monitored syscall invocation, `throttle_check()` does `atomic_inc_return(&call_count)`. If the returned value exceeds `max_calls`, the thread is blocked on `throttle_wq` until the timer fires.

`CLOCK_MONOTONIC` is used instead of `CLOCK_REALTIME` to be immune to NTP adjustments and wall-clock jumps.

The timer callback runs in softirq context, so no sleeping is allowed inside it — only `atomic_set`, `spin_lock_irqsave`, and `wake_up_nr`.

**Delay measurement:**  
The module measures the throttle-induced delay as the time spent inside `throttle_wq`, from when the thread is blocked to when it is woken by the timer:

```c
enter_time = ktime_get();
wait_event_interruptible_exclusive(throttle_wq, ...);
exit_time = ktime_get();
// delay = time waiting for throttle only — orig_fn() is called after exit_time
```

This measurement is precise because `orig_fn(regs)` is called **after** `exit_time` is recorded, so any time the syscall itself spends blocking (e.g. waiting for I/O) never enters the delay statistics.

---

### 6. Blocking vs Non-Blocking Syscalls

The spec explicitly states that monitored syscalls can be of any nature — blocking or non-blocking. This has an observable effect on thread behaviour.

**Non-blocking syscall (e.g. `getpid`, `write` to a pipe):**
```
thread → wrapper → [throttle_wq, if over limit] → orig_fn() → returns quickly
```
The throttle delay is the dominant and only source of latency. Delay statistics directly reflect throttling pressure.

**Blocking syscall (e.g. `read` from a socket, `accept`, `futex`):**
```
thread → wrapper → [throttle_wq, if over limit] → orig_fn() → [blocks on I/O] → returns
```
The thread incurs two independent blocking phases. The throttle delay (measured by the module) and the I/O wait time (inside `orig_fn`) are completely separate. A thread that is not throttled at all may still spend seconds inside `orig_fn` — this time is never attributed to the throttle.

A practical consequence: even with `MAX` set to a low value, blocking syscalls may appear to execute fewer than `MAX` per second simply because they take time to complete, not because the throttle is activating.

---

### 7. Thundering Herd Prevention

When the timer fires and resets the window, it must wake blocked threads. A naive `wake_up_all()` would wake every waiting thread simultaneously. With 1000 threads waiting and `max_calls = 100`, all 1000 would wake, 900 would find the counter already full, and go back to sleep — wasting 900 context switches per second.

**Solution:** two cooperating mechanisms.

**Exclusive waiters:** threads block with `wait_event_interruptible_exclusive()`, which marks them as *exclusive* in the wait queue. Exclusive waiters are woken one at a time by `wake_up_nr()`.

**Controlled wake count:** the timer calls `wake_up_nr(&throttle_wq, max_calls)`, waking exactly as many threads as there are available slots in the new window. Threads that do not get a slot re-enter the wait loop and try again on the next tick.

```c
/* timer callback */
wake_up_nr(&throttle_wq, READ_ONCE(max_calls));   /* not wake_up_all */

/* blocked thread */
ret = wait_event_interruptible_exclusive(throttle_wq, /* _exclusive flag */
          atomic_read(&call_count) < max_calls || ...);
/* must recheck after waking — another thread may have taken the slot first */
count = atomic_inc_return(&call_count);
if (count > max_calls) { atomic_dec(&call_count); /* go back to sleep */ }
```

---

### 8. Drain Protocol for Safe Unload

#### The problem

Normally, a kernel module prevents `rmmod` from proceeding while threads are inside its code by maintaining a reference count (`try_module_get()`). This approach is incompatible here: threads can be *sleeping* inside `generic_sct_wrapper` (blocked in `throttle_check()`), which would prevent the module from ever unloading.

The alternative — unloading while threads are inside the module — risks a **use-after-free**: the kernel frees module memory after `module_exit()` returns, but a thread still executing `generic_sct_wrapper` would then fault on its next instruction.

#### The solution: manual drain

Instead of blocking `rmmod`, we allow it to proceed but make `throttle_exit()` **wait** for all in-flight threads to finish naturally.

```
[every thread entering generic_sct_wrapper]
    atomic_inc(&active_threads_in_wrapper)     // "I'm inside"
    ... throttle_check(), orig_fn() ...
    if (atomic_dec_and_test(&active_threads_in_wrapper) && module_unloading)
        wake_up(&unload_wq)                    // "I was the last one"
```

`throttle_exit()` follows this sequence:

```
1. module_unloading = 1
   wake_up_all(&throttle_wq)        // unblock any sleeping threads immediately

2. remove_all_hooks()               // no new threads will enter the wrapper

3. synchronize_rcu()                // [first] wait for all CPUs to observe the
                                    // hook removal; ensures any thread that had
                                    // already read the old SCT pointer has had
                                    // time to enter the wrapper and increment
                                    // the counter before we check it

4. wait_event(unload_wq,
       active_threads_in_wrapper == 0)  // drain: wait for all in-flight threads

5. synchronize_rcu()                // [second] final barrier on all CPUs

6. restore_x64_sys_call()           // safe to free the stub now
   [module memory is freed]
```

**Why two `synchronize_rcu()` calls?**

The first one closes a race: CPU A may have read `sys_call_table[nr]` and seen the old wrapper pointer *before* `remove_all_hooks()` ran, but not yet have entered the wrapper. Without `synchronize_rcu()`, we might check `active_threads_in_wrapper == 0` before CPU A increments it, declare the drain complete, and free module memory while CPU A is about to execute it. `synchronize_rcu()` guarantees all CPUs have passed through a quiescent state, so CPU A will have entered (and incremented) before we proceed.

The second one is a symmetric barrier ensuring no CPU is still running wrapper code before we free the stub.

**Race during hook removal:** if a thread enters the wrapper after its hook was already removed (`hooks[i].active == 0`), `orig_fn` will be `NULL`. The wrapper falls back to calling `sys_call_table_ptr[nr]` directly, which has already been restored to the original handler — the syscall executes normally, without throttling.

---

### 9. Write-Protection Bypass

The syscall table resides in kernel read-only memory. To replace entries, two hardware protections must be temporarily disabled:

- **CR0.WP (Write Protect bit):** when set, the CPU enforces page-table write permissions even in ring 0. Clearing it allows kernel code to write to read-only pages.
- **CR4.CET (Control-flow Enforcement Technology):** present on modern CPUs, enforces shadow stack and indirect branch tracking. Writing to executable memory (like `x64_sys_call`) requires disabling it first.

Both registers are written using inline assembly (`mov %0, %%cr0`) with a `"+m"` memory constraint rather than the kernel's `write_cr0()` wrapper, which performs additional checks (CR pinning) that would block the modification.

`preempt_disable()` is called before modifying CR0/CR4 and `preempt_enable()` after restoring them, to prevent the thread from being migrated to another CPU between the disable and restore operations (CR registers are per-CPU).

---

## User Interface

The module exposes `/dev/throttleDriver` as a character device. All configuration is done via `ioctl`:

| Command | Direction | Description |
|---------|-----------|-------------|
| `IOCTL_ADD_PROG` | write | Register a program by filesystem path |
| `IOCTL_DEL_PROG` | write | Unregister a program |
| `IOCTL_ADD_UID` | write | Register a UID |
| `IOCTL_DEL_UID` | write | Unregister a UID |
| `IOCTL_ADD_SYSCALL` | write | Hook a syscall (by number) |
| `IOCTL_DEL_SYSCALL` | write | Unhook a syscall |
| `IOCTL_SET_MONITOR` | write | Enable / disable throttling |
| `IOCTL_GET_STATUS` | read | Current state: enabled, max_calls, call_count |
| `IOCTL_SET_MAX` | write | Set calls-per-second limit |
| `IOCTL_GET_STATS` | read | Peak/avg delay, peak/avg blocked threads, total calls |
| `IOCTL_RESET_STATS` | — | Reset statistics counters |

Write commands require `euid == 0`. Read commands are available to all users.

Listing registered programs, UIDs, and syscalls is done via `read()` on the device (not via ioctl), allowing a dynamically-sized text response with no fixed buffer limits:

```bash
./throttleClient list     # reads /dev/throttleDriver in a loop until EOF
```

---

## Build & Usage

```bash
# Build module and userspace tools
make

# Load module and create device node
make load

# Configure
sudo ./throttleClient add_prog /path/to/binary
sudo ./throttleClient add_uid 1000
sudo ./throttleClient add_sys 1        # hook sys_write (nr=1)
sudo ./throttleClient set_max 50       # 50 calls/second
sudo ./throttleClient monitor 1        # enable

# Query
./throttleClient status
./throttleClient stats

# Unload
make unload
```

---

## Testing & Validation

Two test programs are provided, each self-configuring (they open the device, register themselves, run, verify, and clean up):

### throttleTest — Non-blocking syscall, multi-thread

```bash
sudo ./throttleTest <num_threads> <duration_sec> <MAX> [rate_per_thread]

# Examples:
sudo ./throttleTest 8 6 200        # 8 threads at full speed, limit 200/s
sudo ./throttleTest 8 6 200 50     # each thread attempts 50 calls/s
```

Spawns N threads that invoke `getpid` (nr=39) in a tight loop. At the end, reads the driver statistics and automatically verifies that the observed call rate did not exceed `MAX`. Prints `PASS` or `FAIL`.

### throttleTest2 — Blocking syscall + UID-based throttling

```bash
sudo ./throttleTest2 <num_workers> <duration_sec> <MAX>

# Example:
sudo ./throttleTest2 8 6 200
```

Runs two independent tests:

**Test 1 — Blocking syscall (`read` on `/dev/zero`):**  
Verifies that throttling works correctly for a syscall that naturally blocks in kernel space. `/dev/zero` is used as the source so that no additional I/O latency is introduced — the only limiting factor is the module's rate limit. This demonstrates that the flow `thread → wrapper → sleep in throttle_wq → orig_fn() → data` works identically to the non-blocking case.

**Test 2 — UID-based throttling:**  
Verifies that the rate limit is applied based on the caller's effective UID, independently of the program name. Two groups of child processes run in parallel via `fork()` + `setresuid()`:
- **throttled group**: N children with `euid = TARGET_UID` (60001) — subject to `MAX`
- **control group**: M children with `euid = CONTROL_UID` (60002) — no limit applied

Shared counters between processes use `mmap(MAP_SHARED|MAP_ANONYMOUS)` with `_Atomic` variables for lock-free coordination.

---

## Kernel Version Compatibility

| Kernel range | Dispatch path | Module behaviour |
|---|---|---|
| < 5.15 | `entry_SYSCALL_64 → sys_call_table[nr]` | Hook SCT only |
| ≥ 5.15, < 6.4 | `entry_SYSCALL_64 → x64_sys_call` | Hook SCT + patch `x64_sys_call` via `module_alloc` / `set_memory_x` |
| ≥ 6.4, < 6.15 | same as above | Same, but `execmem_alloc` / `execmem_free` / `set_memory_x` resolved at runtime via kprobe (no longer exported) |
| ≥ 6.15 | same as above | Same + `hrtimer_setup` API (replaces `hrtimer_init` + manual function assignment) |

---

## File Structure

```
throttle.h             — shared types, IOCTL definitions, extern declarations
throttle_main.c        — module init/exit, global state, drain protocol orchestration
throttle_mem.c         — page-table walk (sys_vtpmo), CR0/CR4 write-protect bypass
throttle_discovery.c   — sys_call_table finder (CR3 walk), x64_sys_call kprobe lookup, stub allocator
throttle_hook.c        — generic_sct_wrapper, install_hook / remove_hook, drain accounting
throttle_core.c        — throttle_check, hrtimer callback, process identification
throttle_dev.c         — character device driver, ioctl handler
throttleClient.c       — userspace configuration tool
throttleTest.c         — multi-threaded test: non-blocking syscall (getpid), auto PASS/FAIL
throttleTest2.c        — advanced test: blocking syscall (read) + UID-based throttling
```
