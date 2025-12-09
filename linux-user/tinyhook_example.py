#!/usr/bin/env python3
"""
TinyHook Example Script

This script demonstrates how to use the tinyhook API to hook syscalls
in QEMU linux-user mode.

Usage:
    qemu-mipsel -tinyhook tinyhook_example.py ./your_binary

The tinyhook module provides:
    - tinyhook.register_pre_hook(syscall_num, callback)
    - tinyhook.register_post_hook(syscall_num, callback)
    - tinyhook.unregister_pre_hook(syscall_num)
    - tinyhook.unregister_post_hook(syscall_num)
    - tinyhook.read_memory(addr, size) -> bytes
    - tinyhook.write_memory(addr, data)
    - tinyhook.read_string(addr) -> str
    - tinyhook.CONTINUE  (action: call original syscall)
    - tinyhook.SKIP      (action: skip original syscall)
"""

import tinyhook

# Syscall numbers (MIPS as example, check target arch)
SYS_READ = 4003
SYS_WRITE = 4004
SYS_OPEN = 4005
SYS_CLOSE = 4006
SYS_OPENAT = 4288


def on_write_pre(syscall_num, fd, buf, count, arg4, arg5, arg6, arg7, arg8):
    """
    Pre-hook for write() syscall.
    
    Called before the syscall is executed.
    
    Args:
        syscall_num: The syscall number
        fd: File descriptor
        buf: Buffer address (guest memory)
        count: Number of bytes to write
        arg4-arg8: Remaining arguments (unused for write)
    
    Returns:
        A dict with optional keys:
        - "action": tinyhook.CONTINUE or tinyhook.SKIP
        - "args": tuple of 8 arguments (for modified args)
        - "ret": return value (only used if action == SKIP)
    """
    try:
        data = tinyhook.read_memory(buf, min(count, 256))
        print(f"[tinyhook] write(fd={fd}, buf={buf:#x}, count={count})")
        print(f"[tinyhook]   data preview: {data[:64]!r}...")
    except Exception as e:
        print(f"[tinyhook] write hook error: {e}")
    
    # Continue with original syscall
    return {"action": tinyhook.CONTINUE}


def on_write_post(syscall_num, ret, fd, buf, count, arg4, arg5, arg6, arg7, arg8):
    """
    Post-hook for write() syscall.
    
    Called after the syscall is executed.
    
    Args:
        syscall_num: The syscall number
        ret: The original return value
        fd-arg8: The arguments that were passed to the syscall
    
    Returns:
        The (possibly modified) return value.
    """
    print(f"[tinyhook] write returned: {ret}")
    return ret


def on_open_pre(syscall_num, filename_ptr, flags, mode, arg4, arg5, arg6, arg7, arg8):
    """
    Pre-hook for open() syscall - demonstrates reading strings and modifying args.
    """
    try:
        filename = tinyhook.read_string(filename_ptr)
        print(f"[tinyhook] open(filename={filename!r}, flags={flags:#x}, mode={mode:#o})")
        
        # Example: Block opening /etc/passwd
        if filename == "/etc/passwd":
            print("[tinyhook] BLOCKED: /etc/passwd access denied")
            # Return -EACCES (13) and skip the original syscall
            return {
                "action": tinyhook.SKIP,
                "ret": -13  # -EACCES
            }
    except Exception as e:
        print(f"[tinyhook] open hook error: {e}")
    
    return {"action": tinyhook.CONTINUE}


def on_openat_pre(syscall_num, dirfd, filename_ptr, flags, mode, arg5, arg6, arg7, arg8):
    """
    Pre-hook for openat() syscall.
    """
    try:
        filename = tinyhook.read_string(filename_ptr)
        print(f"[tinyhook] openat(dirfd={dirfd}, filename={filename!r}, flags={flags:#x})")
    except Exception as e:
        print(f"[tinyhook] openat hook error: {e}")
    
    return {"action": tinyhook.CONTINUE}


def on_read_post(syscall_num, ret, fd, buf, count, arg4, arg5, arg6, arg7, arg8):
    """
    Post-hook for read() syscall - demonstrates modifying data after syscall.
    """
    if ret > 0:
        print(f"[tinyhook] read(fd={fd}) returned {ret} bytes")
        
        # Example: Log first few bytes read
        try:
            data = tinyhook.read_memory(buf, min(ret, 64))
            print(f"[tinyhook]   data preview: {data[:32]!r}...")
        except:
            pass
    
    return ret


# Register hooks
print("[tinyhook] Registering syscall hooks...")

# Hook write syscall (both pre and post)
tinyhook.register_pre_hook(SYS_WRITE, on_write_pre)
tinyhook.register_post_hook(SYS_WRITE, on_write_post)

# Hook open syscall (pre only - to demonstrate blocking)
tinyhook.register_pre_hook(SYS_OPEN, on_open_pre)

# Hook openat syscall (pre only)
tinyhook.register_pre_hook(SYS_OPENAT, on_openat_pre)

# Hook read syscall (post only - to inspect data)
tinyhook.register_post_hook(SYS_READ, on_read_post)

print("[tinyhook] Hooks registered. Ready to intercept syscalls.")
print("[tinyhook] Note: Syscall numbers are architecture-specific!")
print()
