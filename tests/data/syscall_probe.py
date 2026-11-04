#!/usr/bin/env python3
# Which dangerous syscalls does agentty's real posture actually deny?
#
# Each of these is an escape primitive, or close to one:
#   io_uring_*        submits file IO through a ring the seccomp filter never
#                     sees -- the classic "filter bypass" family
#   userfaultfd       page-fault handling in userspace: a TOCTOU lever
#   process_vm_readv  read another process's memory without ptrace
#   pidfd_open/getfd  steal a descriptor from another process
#   open_by_handle_at needs CAP_DAC_READ_SEARCH, but reaches any inode by
#                     handle -- the mount tree does not contain it
#   memfd_create      harmless alone; the W^X path's usual first step
import ctypes, errno

l = ctypes.CDLL(None, use_errno=True)
l.syscall.restype = ctypes.c_long

CASES = [
    ("io_uring_setup",    425, (8, 0)),
    ("io_uring_enter",    426, (0, 0, 0, 0, 0)),
    ("io_uring_register", 427, (0, 0, 0, 0)),
    ("userfaultfd",       323, (0,)),
    ("process_vm_readv",  310, (0, 0, 0, 0, 0, 0)),
    ("process_vm_writev", 311, (0, 0, 0, 0, 0, 0)),
    ("pidfd_open",        434, (1, 0)),
    ("pidfd_getfd",       438, (0, 0, 0)),
    ("open_by_handle_at", 304, (0, 0, 0)),
    ("memfd_create",      319, (0, 0)),
]

for name, nr, args in CASES:
    ctypes.set_errno(0)
    rc = l.syscall(nr, *args)
    e = ctypes.get_errno()
    verdict = "ALLOWED(reached kernel)" if rc >= 0 else errno.errorcode.get(e, str(e))
    print(f"{name:20s} rc={rc:<4} {verdict}")
