# The su audit log

`kernel/sulog/` records the root grants the module hands out and streams them to one
privileged userspace reader over a file that has no path. Three call sites produce
records, a bounded queue in RAM holds at most 256 of them, and a daemon drains that queue
to disk. Nothing here writes to disk itself, and nothing here blocks a producer: once the
queue is full every further record is thrown away, and the loss is reported to the reader
as a synthetic record rather than hidden.

Read [`event.c`](event.c) first - it snapshots task identity and the [`execve`][execve-2]
arguments, and every design decision in this directory shows up in it. [`fd.c`](fd.c) is
the readable file a daemon polls, and [`../feature/sulog.c`](../feature/sulog.c) is the
on/off switch wired into the feature registry. The transport underneath is generic and
lives in [`../infra/event_queue.c`](../infra/event_queue.c)
([`../infra/README.md`](../infra/README.md)); sulog is its only consumer in the tree.
Persistence belongs to `ksud sulogd`
([`../../userspace/ksud/src/sulog.rs`](../../userspace/ksud/src/sulog.rs)).

- [Who emits, and why capture and emit are two calls](#who-emits-and-why-capture-and-emit-are-two-calls)
- [Record and frame layout](#record-and-frame-layout)
- [What a full buffer does](#what-a-full-buffer-does)
- [The file handed to the reader](#the-file-handed-to-the-reader)
- [The switch and the daemon behind it](#the-switch-and-the-daemon-behind-it)
- [Startup and teardown order](#startup-and-teardown-order)
- [Known limits](#known-limits)

Two headers carry everything the directory exports inward:

| Header | Declares |
| --- | --- |
| [`event.h`](event.h) | the capture and emit calls, `ksu_sulog_get_queue()`, `ksu_sulog_events_init()`/`_exit()` |
| [`fd.h`](fd.h) | `ksu_install_sulog_fd()`, and the fd layer's own init and exit |

## Who emits, and why capture and emit are two calls

### The three producers

Three call sites produce records, and only three, but two of them now stand behind a pair
of syscalls.

| Producer | Event type | Lives in |
| --- | --- | --- |
| `ksu_hook_execve_common()` | `ROOT_EXECVE` | [`../hook/syscall_event_bridge.c`](../hook/syscall_event_bridge.c) |
| `ksu_handle_execve_sucompat_common()` | `SUCOMPAT` | [`../feature/sucompat.c`](../feature/sucompat.c) |
| `do_grant_root()` | `IOCTL_GRANT_ROOT` | [`../supercall/dispatch.c`](../supercall/dispatch.c) |

`ksu_hook_execve_common()` captures a `ROOT_EXECVE` whenever `current_euid().val == 0`.
`ksu_hook_execve()` and `ksu_hook_execveat()` are one-line wrappers over it, and both are
registered in [`../hook/syscall_hook_manager.c`](../hook/syscall_hook_manager.c)
([`../hook/README.md`](../hook/README.md)), because a recent bionic implements `execve()`
on top of [`execveat`][execveat-2]`(AT_FDCWD, path, argv, envp, 0)` and a hook on
`__NR_execve` alone would watch a syscall the C library has stopped issuing.

The `execveat` flag threaded through the common function picks the execveat variant of
each handler that has one - the ksud bootstrap hook and the adb-root path, both described
in [`../hook/README.md`](../hook/README.md), and sucompat, described in
[`../feature/README.md`](../feature/README.md). For capture the flag does nothing except
move the argument window: the pathname pointer is `PT_REGS_PARM1` for `execve` and
`PT_REGS_PARM2` for `execveat`, with argv one register further along in each case, and
capture needs both pointers before the address space is replaced.

`ksu_handle_execve_sucompat_common()` captures a `SUCOMPAT` after installing the `O_PATH`
descriptor for the su target and before rewriting the syscall registers, reached the same
way from `ksu_handle_execve_sucompat()` and `ksu_handle_execveat_sucompat()`. The target
is whatever `su_target_path()` answers: `/data/adb/ksud` when `is_ksud_exists()`, and
`/system/bin/sh` when it does not and the caller is the shell uid on an image patched with
`allow_shell`.

`do_grant_root()` calls `ksu_sulog_emit_grant_root()` with the uid and euid it snapshotted
before `escape_with_root_profile()` ran.

### An execveat is narrowed before it is logged

Interception works by overwriting the syscall's registers into
`execveat(target_fd, "", argv, envp, AT_EMPTY_PATH)`, where `target_fd` is the `O_PATH`
descriptor on ksud or on the `sh` fallback. That cannot be layered on top of a caller that
already meant something by the dirfd or the flags, so the handler jumps to
`do_orig_execve` unless
`(int)PT_REGS_PARM1(regs)` is `AT_FDCWD` and `(int)PT_REGS_PARM5(regs)` is zero. An `su`
invoked as `execveat(fd, "", ..., AT_EMPTY_PATH)` therefore passes through untouched and
yields no `SUCOMPAT` record, though a caller already at euid 0 still produces the
`ROOT_EXECVE` one.

### Two calls, because the record is readable at two instants

The execve sites call `ksu_sulog_capture_root_execve()` / `ksu_sulog_capture_sucompat()`,
which return an opaque `struct ksu_sulog_pending_event *`, and later
`ksu_sulog_emit_pending()` with the result. The two halves of a record are readable at
different instants: before the syscall, `filename_user` and `argv_user` point into the
caller's address space but the outcome is unknown; after a successful `execve` that
address space has been replaced, so the same pointers name the new image's memory. One
shot afterwards logs garbage for exactly the events that matter most; one shot beforehand
gives every record `retval = 0`.

The cost of the split is that `retval` has to be read through `event_type`. That is the
one thing a consumer must get right, because nothing on `retval` itself says which reading
applies.

| `event_type` | `retval` holds | Stamped |
| --- | --- | --- |
| `ROOT_EXECVE` | the real syscall's return value | after the syscall returns |
| `SUCOMPAT` | the return value of `escape_with_root_profile()` | before `ksu_syscall_table[__NR_execveat]()` runs, so zero means the escalation succeeded, not that the program did |
| `IOCTL_GRANT_ROOT` | the return value of `escape_with_root_profile()` | after that call, inside `do_grant_root()` |

One syscall can yield two records, but only when the sucompat handler also accepts the
caller. A process already at euid 0 that execs `/system/bin/su` trips the euid test and
then falls into the sucompat branch, which gates on `ksu_is_allow_uid_for_current()` over
the *real* uid. `__ksu_is_allow_uid_for_current()` in
[`../policy/allowlist.c`](../policy/allowlist.c) answers `is_ksu_domain()` for a real uid
of 0, so a root daemon outside `u:r:ksu:s0` leaves only the `ROOT_EXECVE` record; an
allowlisted app that had already reached euid 0 gets both. The branch is reached at all
only while `ksu_su_compat_enabled` is set and the adb-root branch has not claimed the call
first, which it does for any task other than pid 1 whose cred is init's.

### Reading user memory without faulting

Capture reads user memory with `strncpy_from_user_nofault()` rather than
`strncpy_from_user()`, which runs with page faults disabled, so a bad or non-resident
pointer fails immediately instead of dragging the fault handler and `mmap_lock` into a
best-effort log path. Atomic context is not the reason:
`ksu_handle_execve_sucompat_common()` calls plain `strncpy_from_user()` on the same
filename pointer a few lines earlier, and both execve producers pass `GFP_KERNEL`.

The filename and each argument string also pass through `untagged_addr()` to strip the
arm64 [top-byte-ignore][tagged-address-abi] / MTE tag; the argv vector itself is read with
`get_user()` on the pointer exactly as the caller supplied it.

The argv walker mirrors `fs/exec.c`'s `struct user_arg_ptr` union, but its compat branch
is unreachable, and so is the rest of the capture path for a 32-bit caller.
`ksu_sys_enter_handler()` in
[`../hook/syscall_hook_manager.c`](../hook/syscall_hook_manager.c) - the `sys_enter`
handler that redirects hooked syscalls into the dispatcher - returns before the redirect
for `in_compat_syscall()` on x86_64 and `is_compat_task()` on arm64. With no redirect,
`ksu_hook_execve()` never runs: a 32-bit caller's execs produce no record at all, not a
record with a blank command line.

## Record and frame layout

The payload is `struct ksu_sulog_event` in [`../../uapi/sulog.h`](../../uapi/sulog.h):

```c
struct ksu_sulog_event {
    __u16 version;
    __u16 event_type;
    __s32 retval;
    __u32 pid;
    __u32 tgid;
    __u32 ppid;
    __u32 uid;
    __u32 euid;
    char comm[TASK_COMM_LEN];
    __u32 filename_len;
    __u32 argv_len;
} __packed;
```

That is 52 bytes, and the filename and the flattened argv follow it, each NUL-terminated.

The two lengths are not measured the same way. `argv_len` is the flattened line plus its
NUL. `filename_len` is one byte larger than that: `strncpy_from_user_nofault()` already
counts the NUL, and `ksu_sulog_copy_filename()` adds another, so an untruncated filename
of L bytes is reported as L + 2 and drags one spare zero byte out of the `kzalloc`'d
payload. Only the two edge cases are exact - a faulted or empty read reports 1, and a
filename that fills the 256-byte window reports 256. A consumer must therefore stop at the
first NUL rather than trust the length, which is what `parse_c_string()` in
[`sulog.rs`](../../userspace/ksud/src/sulog.rs) does.

`__packed` buys nothing today: every member is fixed-width, none needs more than four-byte
alignment, and the natural layout already has no padding on a 32- or a 64-bit build. It is
there to pin the layout against whatever field gets added later, which matters because
`ksud` reparses the bytes as a hand-written `#[repr(C, packed)] SulogEventHeader` rather
than as bindgen output. `comm` must stay exactly `TASK_COMM_LEN`, since `get_task_comm()`
carries a `BUILD_BUG_ON` on the destination size. `KSU_SULOG_EVENT_VERSION` is stamped
into every record and is currently 1.

The frame is `struct ksu_event_record_hdr` in
[`../infra/event_queue.h`](../infra/event_queue.h): 24 bytes of
`{type, flags, len, seq, ts_ns}` emitted verbatim ahead of every payload. `seq` starts at
1 and is assigned under the queue lock, so it counts events produced, including those
lost; `ts_ns` is
`ktime_get_ns()`, i.e. CLOCK_MONOTONIC. Type `0xFFFF` (`KSU_EVENT_QUEUE_TYPE_DROPPED`) is
reserved for the loss marker below.

Capture puts the header, filename and argv in one 2048-byte allocation, so the
variable-length part has a single lifetime; the only other allocation is the small
`struct ksu_sulog_pending_event` that carries it, and `ksu_sulog_free_pending()` releases
both. Only the bytes used are pushed, so a grant-root record carries a 54-byte payload, 78
bytes once the frame header in front of it is counted.

### Capture limits

Every bound on a record is a `#define` at the top of [`event.c`](event.c):

| Constant | Value | Bounds |
| --- | --- | --- |
| `KSU_SULOG_MAX_QUEUED` | 256 | records held before a new one is dropped |
| `KSU_SULOG_MAX_PAYLOAD_LEN` | 2048 | the one allocation holding header, filename and argv |
| `KSU_SULOG_MAX_FILENAME_LEN` | 256 | the filename window |
| `KSU_SULOG_MAX_ARG_CHUNK` | 256 | the `arg` stack buffer each argv string is copied through |
| `KSU_SULOG_MAX_ARG_STRINGS` | `0x7FFFFFFF` | argv entries walked, i.e. the walk stops at the first NULL pointer, not at a count |

The flattened command line takes whatever is left of the payload after the header and the
filename.

## What a full buffer does

### A bounded list, not a ring

Despite the "ring buffer" reflex, [`../infra/event_queue.c`](../infra/event_queue.c) holds
a bounded linked list: a `struct list_head pending` of kmalloc'd nodes, appended at the
tail and removed from the head. When it is full the *new* event is dropped and the old
ones are kept; there is no overwrite-oldest path. `ksu_event_queue_push()` builds the node
before taking `queue->lock`, so the spinlock covers only the insertion and the sequence
bump. If the allocation failed, or `queued >= max_queued` (256), it calls
`ksu_event_queue_note_drop_locked(queue, seq)` instead of linking and returns `-ENOMEM` or
`-ENOSPC`; `ksu_sulog_capture()`'s `out_drop` label calls `ksu_event_queue_drop()`
directly, so a capture-time failure is accounted the same way.

Producers never wait for the reader: an audit log that could stall an `execve` would be
worse than one that loses records. They can still sleep in the allocator, since both
execve sites pass `GFP_KERNEL`, but nothing in the path blocks on a slow or absent
consumer.

### Losses are reported, not hidden

`dropped_pending`, `dropped_first_seq` and `dropped_last_seq` accumulate, and the reader's
next [`read()`][read-2] gets a synthetic record of type `0xFFFF` with
`KSU_EVENT_RECORD_FLAG_INTERNAL` set, carrying
`struct ksu_event_queue_dropped_info { dropped, first_seq, last_seq }`. Naming the range
lets a reader say which records are missing, not merely that some are.

`ksu_event_queue_read_drop()` dances around one hazard: the counters must be cleared under
the spinlock, since a producer can bump them at any instant, but `copy_to_user()` may
fault and sleep and cannot run under one. It moves the values into `dropped_inflight*` and
zeroes the live ones before dropping the lock; on a faulting copy `out_restore` folds them
back, keeping the earlier `first_seq`. `ksu_event_queue_has_data_locked()` counts
`dropped_inflight` too, so [`poll()`][poll-2] still reports readable while a drop record
is owed. Without that staging, one faulting copy would erase the evidence that anything
was lost.

The record path has the mirror-image constraint: `ksu_event_queue_read_node()` reads the
head node's length under the lock, releases it, copies the bytes out, and only then
unlinks with `list_del()` and frees. Touching an unlocked node that way is safe because
`ksu_event_queue_read()` holds the `read_lock` mutex across the whole call, so a second
thread that shares or dup'd the descriptor cannot reach the same head node. A faulting
copy leaves the node at the head and returns `-EFAULT` with nothing lost.

## The file handed to the reader

### Why an anonymous inode

`ksu_install_sulog_fd()` calls `anon_inode_getfile("[ksu_sulog]", &ksu_sulog_fops, NULL,
O_RDONLY | O_CLOEXEC)` and `fd_install()`s the result. An anonymous inode is a
`struct file` with custom `file_operations` and no filesystem entry, the construction
behind [`eventfd`][eventfd-2] and [`perf_event_open`][perf-event-open-2]; it surfaces only
as `anon_inode:[ksu_sulog]` in the holder's `/proc/<pid>/fd`. That is the point. A `/proc`
node would be a named, enumerable object every process can stat, and would need an
[SELinux][selinux] `genfscon` label plus policy to be reachable from `u:r:ksu:s0` - one
more visible artefact for a project that spends its other half hiding them. A netlink
socket would need a globally registered family number and would still appear in
`/proc/net/netlink`. The module already owns an ioctl control plane on the `[ksu_driver]`
anon inode ([`../supercall/supercall.c`](../supercall/supercall.c),
[`../supercall/README.md`](../supercall/README.md)), so a second anon-inode fd costs
nothing new.

### What the descriptor allows

`ksu_sulog_fops` exposes `read`, `poll`, `release` and `noop_llseek`, and no
`unlocked_ioctl`: a leaked sulog descriptor grants the ability to watch the audit stream,
not to command the module. `.owner = THIS_MODULE` pins the module while a descriptor is
open, keeping `ksu_event_queue_destroy()` from freeing nodes under an in-flight `read()`.

At most one descriptor is handed out at a time - `ksu_sulog_fd_active` under
`ksu_sulog_fd_lock` fails a second `GET_SULOG_FD` with `-EBUSY` until the holder's last
close reaches `.release` - because reads are destructive and two readers would split the
stream. The flag guards the handout, not the descriptor: a holder that `dup()`s or forks
hands a second thread a way in, which is why the read path still serialises on
`read_lock`.

### The GET_SULOG_FD contract

Userspace asks with `KSU_IOCTL_GET_SULOG_FD`, declared in
[`../../uapi/supercall.h`](../../uapi/supercall.h) as
`_IOW('K', 20, struct ksu_get_sulog_fd_cmd)`. The struct is one reserved `flags` word.

| Outcome | Result |
| --- | --- |
| Success | the fd number, as the ioctl's return value |
| Caller is not uid 0 | `-EPERM`, from the dispatcher's `perm_check` before the handler runs |
| `flags` is non-zero | `-EINVAL` |
| A descriptor is already outstanding | `-EBUSY` |
| The queue has been closed by teardown | `-EPIPE` |

Rejecting a non-zero `flags` word makes a future variant addable without a new ioctl
number. Returning the fd number as the ioctl's return value rather than as a struct field
means no second `copy_to_user()` can fault after installation and leak it. The dispatch
table pins the command to `only_root` ([`../supercall/perm.c`](../supercall/perm.c)), not
`manager_or_root`: while the feature is on, the stream carries every root exec the hooks
catch, command lines included.

## The switch and the daemon behind it

[`../feature/sulog.c`](../feature/sulog.c) registers `sulog_handler` with
[`../policy/feature.c`](../policy/feature.c) as `KSU_FEATURE_SULOG` (id 2 in
[`../../uapi/feature.h`](../../uapi/feature.h)), backed by one
`static bool ksu_sulog_enabled __read_mostly` defaulting to false. The test lives inside
`ksu_sulog_capture()`, not at the hook sites, so a disabled log costs one bool read per
root `execve`. There is no Kconfig symbol; `sulog/event.o`, `sulog/fd.o`,
`feature/sulog.o` and `infra/event_queue.o` are all listed unconditionally in
[`../Kbuild`](../Kbuild).

Flipping the bit is not enough, because something has to drain the queue.
`set_kernel_feature()` in
[`../../userspace/ksud/src/feature.rs`](../../userspace/ksud/src/feature.rs) calls
`sulog::ensure_sulogd_running()` whenever the feature is set non-zero, which double-forks
and re-execs `/proc/self/exe sulogd`. That daemon takes an exclusive [`flock`][flock-2] on
`/data/adb/ksu/sulogd.lock` and exits quietly if another instance holds it, so the spawn
is idempotent. `init_features()` replays `/data/adb/ksu/.feature_config` at post-fs-data,
which is how the daemon returns after a reboot.

That file stores each setting under the numeric feature id rather than the name, which
makes the id an ABI of its own: renumbering `KSU_FEATURE_SULOG` would make a saved sulog
bit come back as some other feature's. Upstream allocates ids upward from zero and once
took 5 for `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT`, which is why this fork's own features now
start at 16; sulog kept id 2 across that, and `parse_feature_id()` in
[`../../userspace/ksud/src/feature.rs`](../../userspace/ksud/src/feature.rs) still
resolves both `sulog` and `2` onto `FeatureId::Sulog`.

`run_sulog_session()` opens the stream through `open_sulog_fd()`, which calls
`ksucalls::get_sulog_fd()`
([`../../userspace/ksud/src/ksucalls.rs`](../../userspace/ksud/src/ksucalls.rs)) and
switches the descriptor to `O_NONBLOCK` with an [`F_GETFL`/`F_SETFL`][fcntl-2] pair -
which is what selects `-EAGAIN` over sleeping, since `ksu_sulog_read()` passes
`file->f_flags` through. The session then watches it with [`epoll`][epoll-7]. Its
8192-byte buffer exceeds the 2072-byte maximum frame, and that matters: a buffer smaller
than the head record makes `ksu_event_queue_read_node()` return `-EMSGSIZE` forever.
Frames are decoded by testing for `0xFFFF` first and otherwise checking
`size_of::<SulogEventHeader>() + filename_len + argv_len == payload.len()`, so a layout
drift becomes a skipped record, not an out-of-bounds slice.

Lines land in `/data/adb/ksu/log/sulog-YYYY-MM-DD[-N].log`, mode 0600 in a 0700 directory,
as `key=value` text flushed per line, with backslash, quote and control-character escaping
on every quoted field. `argv` is attacker-controlled, and an unescaped newline would let a
logged process forge log lines. Retention and rotation come from the module-config store
under module id `internal.ksud.sulogd`: `log.retention.days` (default 3) and
`log.max_file_size` (default 10 MiB).

The manager never opens the kernel fd - there is no JNI binding for it. It flips the
feature with `execKsud("feature set sulog ...")` in
[`SettingsRepositoryImpl.kt`](../../manager/app/src/main/java/me/weishu/kernelsu/data/repository/SettingsRepositoryImpl.kt)
and tails the rotated files through libsu's `SuFile` and `SuFileInputStream` in
[`SulogHelper.kt`](../../manager/app/src/main/java/me/weishu/kernelsu/ui/util/SulogHelper.kt).

## Startup and teardown order

`ksu_sulog_init()` runs from [`../core/init.c`](../core/init.c) immediately after
`ksu_feature_init()`, because the handler table has to exist, and before
`ksu_supercalls_init()`, because `GET_SULOG_FD` must not be reachable before the queue is
initialised. `ksu_sulog_exit()` runs in phase 2 of `kernelsu_exit()`, after the syscall
hooks and the supercall driver are gone and after [`synchronize_rcu()`][whatisrcu]:
producers must be silenced before the list they append to is dismantled.

Teardown is itself ordered. `ksu_sulog_fd_exit()` clears the single-instance flag and
calls `ksu_event_queue_close()`, which sets `closed` under the lock and wakes every waiter
with `EPOLLHUP`; only then does `ksu_sulog_events_exit()` reach
`ksu_event_queue_destroy()`, which takes `read_lock` before freeing nodes so it cannot
race a reader mid-`copy_to_user()`. Setting `closed` first turns a blocked
`wait_event_interruptible` into a clean EOF instead of leaving a task parked on a wait
queue about to disappear.

To the daemon that is a zero-length read; the session ends and `run_sulogd()` retries
three seconds later. The retry cannot succeed: `ksu_install_sulog_fd()` tests
`queue->closed` before it allocates anything and answers `-EPIPE`, and the queue never
reopens. Only a fresh module load starts a new stream.

## Known limits

The stream is best-effort by construction. These gaps are known and unaddressed:

- Records produced during teardown vanish without a marker: `ksu_event_queue_push()` on a
  closed queue returns `-EPIPE` before consuming a sequence number or noting a drop.
- Disabling the feature neither drains nor closes the queue, and does not stop `sulogd`.
  There is no flush, reset or resize.
- Nothing enforces that the holder of `[ksu_sulog]` is ksud, so any root process can take
  it first and starve the daemon.
- `KSU_SULOG_EVENT_VERSION` is stamped into every record but no consumer branches on it.
- Sequence numbers restart at 1 on module reload rather than on reboot, so the daemon's
  `boot_id` marker is the only fence. `write_session_marker()` in
  [`../../userspace/ksud/src/sulog.rs`](../../userspace/ksud/src/sulog.rs) emits it, as a
  `type=daemon_start boot_id=...` line on a first session and
  `type=daemon_restart boot_id=... restart=N` on every later one.

Reading user memory without faulting has a cost the record does not admit to. A string
that is swapped out or not yet faulted in makes `strncpy_from_user_nofault()` fail, and
both `ksu_sulog_copy_filename()` and `ksu_sulog_flatten_argv()` fall back to
`ksu_sulog_copy_empty_string()` when it does, so the record goes out with a blank `file`
or a blank `argv` and nothing marks the difference between a program invoked with no
arguments and one whose arguments could not be read. Truncation is silent in the same way,
at each of the three windows in [Capture limits](#capture-limits): the 256-byte filename,
the 256-byte `arg` chunk every argument is copied through, and whatever is left of the
2048-byte payload for the flattened command line.

The `execveat` capture adds a third source of blanks that is nobody's fault: a caller that
names its program by descriptor passes an empty pathname with `AT_EMPTY_PATH`, so the
record copies a legitimately empty string and prints the same `file=""` as a read that
faulted. A relative pathname under a real dirfd loses its anchor for the same reason:
`execveat(dirfd, "prog", argv, envp, 0)` from a euid-0 caller logs `file="prog"` and the
record has no field for the descriptor the name is relative to.

Nothing else in the record names the program either: `comm` is filled during capture and
still holds the caller's name, not the image about to replace it. The syscall goes unnamed
as well: with bionic routing `execve()` through `execveat` and both numbers hooked,
`ROOT_EXECVE` records now mix the two, and separating them would take a new field and a
`KSU_SULOG_EVENT_VERSION` bump that no consumer reads yet.

## See also

- [`../README.md`](../README.md) - the kernel module: build modes, init order, layer map
- [`../infra/README.md`](../infra/README.md) - the event queue this area is built on
- [`../hook/README.md`](../hook/README.md) - the syscall hooks the execve records come from
- [`../supercall/README.md`](../supercall/README.md) - the ioctl that hands out the fd
- [`../feature/README.md`](../feature/README.md) - the toggle and its sibling features
- [`../policy/README.md`](../policy/README.md) - the allowlist predicate the gate uses
- [`../../uapi/README.md`](../../uapi/README.md) - the kernel/userspace ABI contract
- [`../../userspace/ksud/README.md`](../../userspace/ksud/README.md) - the `sulogd` daemon
- [`../../docs/architecture.md`](../../docs/architecture.md) - where this sits in the tree

<!-- reference links: kernel documentation and man pages -->
[epoll-7]: https://man7.org/linux/man-pages/man7/epoll.7.html
[eventfd-2]: https://man7.org/linux/man-pages/man2/eventfd.2.html
[execve-2]: https://man7.org/linux/man-pages/man2/execve.2.html
[execveat-2]: https://man7.org/linux/man-pages/man2/execveat.2.html
[fcntl-2]: https://man7.org/linux/man-pages/man2/fcntl.2.html
[flock-2]: https://man7.org/linux/man-pages/man2/flock.2.html
[perf-event-open-2]: https://man7.org/linux/man-pages/man2/perf_event_open.2.html
[poll-2]: https://man7.org/linux/man-pages/man2/poll.2.html
[read-2]: https://man7.org/linux/man-pages/man2/read.2.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[tagged-address-abi]: https://docs.kernel.org/arch/arm64/tagged-address-abi.html
[whatisrcu]: https://docs.kernel.org/RCU/whatisRCU.html
