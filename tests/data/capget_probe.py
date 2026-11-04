#!/usr/bin/env python3
# capget(2) inside the box.
#
# node calls this 15 times at startup (seen in an strace -c on the host), and
# claybin's seccomp profile does not list it -- so it returns EPERM rather
# than succeeding. V8's startup assert is literally `Check failed: 12 ==
# errno`, i.e. "the call I just made should have set ENOMEM", so a syscall
# handing back the wrong errno is exactly the shape that trips it.
#
# Expected: EPERM in a box, 0 on the host.
import ctypes
import errno

l = ctypes.CDLL(None, use_errno=True)
hdr = ctypes.create_string_buffer(8)      # cap_user_header_t
data = ctypes.create_string_buffer(24)    # cap_user_data_t[2]
# version _LINUX_CAPABILITY_VERSION_3, pid 0 = self
ctypes.memmove(hdr, (0x20080522).to_bytes(4, "little") + b"\0\0\0\0", 8)

ctypes.set_errno(0)
rc = l.syscall(125, ctypes.byref(hdr), ctypes.byref(data))
e = ctypes.get_errno()
print("capget rc=", rc, "errno=", errno.errorcode.get(e, e) if rc < 0 else "-")
