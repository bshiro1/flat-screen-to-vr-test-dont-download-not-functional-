#!/usr/bin/env python3
"""
VR Game Converter - DLL Injector

Injects the vr_converter.dll into a target game process for runtime
VR conversion. Uses SetWindowsHookEx for anti-cheat bypass.

Usage:
    python injector.py --target game.exe --dll vr_converter.dll
    python injector.py --pid 1234 --dll vr_converter.dll
    python injector.py --launch "C:\\Games\\game.exe" --dll vr_converter.dll
"""

import ctypes
import ctypes.wintypes
import sys
import time
import argparse
import subprocess
from pathlib import Path

# ─── ctypes type setup ──────────────────────────────────────────────────────
# On 64-bit, ctypes defaults to 32-bit int for return values, which truncates
# HANDLE/LPVOID pointers.  We must set .restype / .argtypes explicitly.

kernel32 = ctypes.windll.kernel32
user32 = ctypes.windll.user32
ntdll = ctypes.windll.ntdll

# kernel32 types
kernel32.OpenProcess.restype = ctypes.wintypes.HANDLE
kernel32.OpenProcess.argtypes = (ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.wintypes.DWORD)

kernel32.VirtualAllocEx.restype = ctypes.wintypes.LPVOID
kernel32.VirtualAllocEx.argtypes = (ctypes.wintypes.HANDLE, ctypes.wintypes.LPVOID,
                                     ctypes.c_size_t, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD)

kernel32.WriteProcessMemory.restype = ctypes.wintypes.BOOL
kernel32.WriteProcessMemory.argtypes = (ctypes.wintypes.HANDLE, ctypes.wintypes.LPVOID,
                                         ctypes.wintypes.LPCVOID, ctypes.c_size_t,
                                         ctypes.POINTER(ctypes.c_size_t))

kernel32.CreateRemoteThread.restype = ctypes.wintypes.HANDLE
kernel32.CreateRemoteThread.argtypes = (ctypes.wintypes.HANDLE,
                                         ctypes.wintypes.LPVOID,  # LPSECURITY_ATTRIBUTES
                                         ctypes.c_size_t,
                                         ctypes.wintypes.LPVOID,  # LPTHREAD_START_ROUTINE
                                         ctypes.wintypes.LPVOID,
                                         ctypes.wintypes.DWORD,
                                         ctypes.POINTER(ctypes.wintypes.DWORD))

kernel32.WaitForSingleObject.restype = ctypes.wintypes.DWORD
kernel32.WaitForSingleObject.argtypes = (ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD)

kernel32.CloseHandle.restype = ctypes.wintypes.BOOL
kernel32.CloseHandle.argtypes = (ctypes.wintypes.HANDLE,)

kernel32.VirtualFreeEx.restype = ctypes.wintypes.BOOL
kernel32.VirtualFreeEx.argtypes = (ctypes.wintypes.HANDLE, ctypes.wintypes.LPVOID,
                                    ctypes.c_size_t, ctypes.wintypes.DWORD)

kernel32.GetProcAddress.restype = ctypes.wintypes.LPVOID
kernel32.GetProcAddress.argtypes = (ctypes.wintypes.HMODULE, ctypes.wintypes.LPCSTR)

kernel32.GetModuleHandleA.restype = ctypes.wintypes.HMODULE
kernel32.GetModuleHandleA.argtypes = (ctypes.wintypes.LPCSTR,)

kernel32.LoadLibraryW.restype = ctypes.wintypes.HMODULE
kernel32.LoadLibraryW.argtypes = (ctypes.wintypes.LPCWSTR,)

kernel32.GetModuleFileNameW.restype = ctypes.wintypes.DWORD
kernel32.GetModuleFileNameW.argtypes = (ctypes.wintypes.HMODULE, ctypes.wintypes.LPWSTR,
                                         ctypes.wintypes.DWORD)

kernel32.GetModuleHandleW.restype = ctypes.wintypes.HMODULE
kernel32.GetModuleHandleW.argtypes = (ctypes.wintypes.LPCWSTR,)

kernel32.FreeLibrary.restype = ctypes.wintypes.BOOL
kernel32.FreeLibrary.argtypes = (ctypes.wintypes.HMODULE,)

kernel32.GetLastError.restype = ctypes.wintypes.DWORD
kernel32.GetLastError.argtypes = ()

# ntdll types
ntdll.NtCreateThreadEx.restype = ctypes.wintypes.LONG
ntdll.NtCreateThreadEx.argtypes = (
    ctypes.POINTER(ctypes.wintypes.HANDLE),  # ThreadHandle
    ctypes.wintypes.DWORD,                   # DesiredAccess
    ctypes.wintypes.LPVOID,                  # ObjectAttributes (NULL)
    ctypes.wintypes.HANDLE,                  # ProcessHandle
    ctypes.wintypes.LPVOID,                  # StartRoutine
    ctypes.wintypes.LPVOID,                  # Argument
    ctypes.wintypes.DWORD,                   # CreateFlags
    ctypes.c_size_t,                         # ZeroBits
    ctypes.c_size_t,                         # StackSize
    ctypes.c_size_t,                         # MaximumStackSize
    ctypes.wintypes.LPVOID,                  # AttributeList (NULL)
)

# user32 types — HOOKPROC is just a void* (function pointer address)
HOOKPROC = ctypes.c_void_p
user32.SetWindowsHookExW.restype = ctypes.wintypes.HHOOK
user32.SetWindowsHookExW.argtypes = (ctypes.c_int, HOOKPROC,
                                      ctypes.wintypes.HINSTANCE, ctypes.wintypes.DWORD)

user32.PostThreadMessageW.restype = ctypes.wintypes.BOOL
user32.PostThreadMessageW.argtypes = (ctypes.wintypes.DWORD, ctypes.wintypes.UINT,
                                       ctypes.wintypes.WPARAM, ctypes.wintypes.LPARAM)

user32.UnhookWindowsHookEx.restype = ctypes.wintypes.BOOL
user32.UnhookWindowsHookEx.argtypes = (ctypes.wintypes.HHOOK,)

# ─── Constants ──────────────────────────────────────────────────────────────
PROCESS_CREATE_THREAD = 0x0002
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_SUSPEND_RESUME = 0x0800
PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x00001000
MEM_RESERVE = 0x00002000
PAGE_READWRITE = 0x04

# Minimum rights needed for VirtualAllocEx + WriteProcessMemory + CreateRemoteThread
INJECT_RIGHTS = (PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                 PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION)

WH_CBT = 5
WH_KEYBOARD = 2

# NtCreateThreadEx flags
THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER = 0x00000004
THREAD_ALL_ACCESS = 0x1F03FF
NTSTATUS_SUCCESS = 0


def get_process_id_by_name(name):
    """Find a process by executable name, return first match."""
    import psutil
    for proc in psutil.process_iter(['pid', 'name', 'threads']):
        if proc.info['name'].lower() == name.lower():
            return proc.info['pid']
    raise RuntimeError(f"Process '{name}' not found")


def get_thread_ids_by_pid(pid):
    """Get all thread IDs for a given process PID."""
    import psutil
    try:
        proc = psutil.Process(pid)
        return [t.id for t in proc.threads()]
    except psutil.NoSuchProcess:
        raise RuntimeError(f"Process {pid} not found")


def get_export_rva_from_file(dll_path, export_name):
    """Read a DLL's export table directly from file to find an export's RVA."""
    import struct
    name_bytes = export_name.encode()
    with open(dll_path, 'rb') as f:
        dos = f.read(64)
        e_lfanew = struct.unpack('<I', dos[0x3C:0x40])[0]
        f.seek(e_lfanew + 4)  # skip signature
        fh = f.read(20)
        num_sections = struct.unpack('<H', fh[2:4])[0]
        opt_hdr_size = struct.unpack('<H', fh[16:18])[0]
        # NumberOfRvaAndSizes at offset 108 within optional header
        f.seek(e_lfanew + 4 + 20 + 108)
        num_rvas = struct.unpack('<I', f.read(4))[0]
        # Data directories at offset 112 within optional header
        f.seek(e_lfanew + 4 + 20 + 112)
        export_rva = struct.unpack('<I', f.read(4))[0]  # first entry = export
        # Read section headers
        f.seek(e_lfanew + 4 + 20 + opt_hdr_size)
        sections = []
        for _ in range(num_sections):
            s = f.read(40)
            name = s[:8].rstrip(b'\x00')
            va = struct.unpack('<I', s[12:16])[0]
            vsize = struct.unpack('<I', s[8:12])[0]
            roff = struct.unpack('<I', s[20:24])[0]
            sections.append((name, va, vsize, roff))
        def rva_to_offset(rva):
            for name, va, vsize, roff in sections:
                if va <= rva < va + vsize:
                    return roff + (rva - va)
            return None
        # Parse export directory
        off = rva_to_offset(export_rva)
        if off is None:
            raise RuntimeError(f"Export dir RVA {export_rva:#x} not in any section")
        f.seek(off)
        edata = f.read(40)
        n_names = struct.unpack('<I', edata[24:28])[0]
        n_funcs = struct.unpack('<I', edata[20:24])[0]
        addr_names = struct.unpack('<I', edata[32:36])[0]
        addr_ordinals = struct.unpack('<I', edata[36:40])[0]
        addr_funcs = struct.unpack('<I', edata[28:32])[0]
        # Read name pointers
        off_names = rva_to_offset(addr_names)
        f.seek(off_names)
        name_ptrs = struct.unpack(f'<{n_names}I', f.read(n_names * 4))
        # Read ordinals
        off_ords = rva_to_offset(addr_ordinals)
        f.seek(off_ords)
        ords = struct.unpack(f'<{n_names}H', f.read(n_names * 2))
        # Read function RVAs
        off_funcs = rva_to_offset(addr_funcs)
        f.seek(off_funcs)
        func_rvas = struct.unpack(f'<{n_funcs}I', f.read(n_funcs * 4))
        # Search for export name
        for i, np in enumerate(name_ptrs):
            no = rva_to_offset(np)
            f.seek(no)
            n = b''
            while True:
                b = f.read(1)
                if b == b'\x00' or not b: break
                n += b
            if n == name_bytes:
                return func_rvas[ords[i]]
    raise RuntimeError(f"Export '{export_name}' not found in {dll_path}")


def inject_setwindowsHookEx(pid, dll_path):
    """
    Inject DLL using SetWindowsHookEx.

    Windows loads the DLL into the target process when the hooked
    message is processed. This bypasses anti-cheat that blocks
    CreateRemoteThread + VirtualAllocEx.

    Our DLL exports VRCbtProc which calls StartVRConverter on hook fire.
    """
    dll_path_str = str(Path(dll_path).resolve())

    # Load the DLL locally to get its module handle
    local_dll = kernel32.LoadLibraryW(dll_path_str)
    if not local_dll:
        err = ctypes.GetLastError()
        raise RuntimeError(f"Failed to load DLL locally: error {err}")

    try:
        # Verify the path Windows has for this module
        buf = ctypes.create_unicode_buffer(260)
        kernel32.GetModuleFileNameW(local_dll, buf, 260)
        print(f"  Module path: {buf.value}")

        # Also try getting a handle by just the DLL filename
        name_dll = kernel32.GetModuleHandleW("vr_converter.dll")
        if name_dll:
            print(f"  Handle by name: {name_dll:#010x}")
            local_dll = name_dll  # Use this handle instead

        # Calculate VRCbtProc address: local base + RVA from file
        cbt_rva = get_export_rva_from_file(dll_path_str, "VRCbtProc")
        cbt_proc = local_dll + cbt_rva
        print(f"  VRCbtProc at local address: {cbt_proc:#010x} (RVA: {cbt_rva:#06x})")

        # Get a thread ID from the target process
        thread_ids = get_thread_ids_by_pid(pid)
        if not thread_ids:
            raise RuntimeError("No threads found in target process")
        thread_id = thread_ids[0]
        print(f"  Target thread ID: {thread_id}")

        # Set the CBT hook — Windows injects our DLL into the target process
        print("  Installing CBT hook...")
        hook = user32.SetWindowsHookExW(WH_CBT, cbt_proc, local_dll, thread_id)
        if not hook:
            err = ctypes.GetLastError()
            raise RuntimeError(f"SetWindowsHookEx failed: error {err} ({err:#x})")

        print("  Hook installed, triggering callback...")

        # Trigger the hook by sending a message to the target thread's queue.
        # CBT hook fires on window messages. PostThreadMessage wakes the queue.
        user32.PostThreadMessageW(thread_id, 0x100, 0, 0)  # WM_KEYDOWN

        # Give the hook time to fire and init to run
        time.sleep(2)

        # Cleanup
        user32.UnhookWindowsHookEx(hook)
        print("  Hook removed")

    finally:
        kernel32.FreeLibrary(local_dll)

    return True


def inject_remotethread(pid, dll_path):
    """
    Fallback: classic CreateRemoteThread + LoadLibraryA injection.
    Tries with minimal rights first, then falls back to PROCESS_ALL_ACCESS.
    """
    dll_path_str = str(dll_path)
    dll_path_bytes = dll_path_str.encode('utf-8') + b'\x00'

    for rights in [INJECT_RIGHTS, PROCESS_ALL_ACCESS]:
        handle = kernel32.OpenProcess(rights, False, pid)
        if handle:
            break
    if not handle:
        err = ctypes.GetLastError()
        raise RuntimeError(f"OpenProcess failed for PID {pid}: error {err}")

    try:
        dll_path_addr = kernel32.VirtualAllocEx(
            handle, None, len(dll_path_bytes) + 1,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        )
        if not dll_path_addr:
            err = ctypes.GetLastError()
            raise RuntimeError(
                f"VirtualAllocEx failed (error {err}) — game likely has anti-cheat "
                f"blocking DLL injection. Try a game without Denuvo/EAC/BattlEye."
            )

        written = ctypes.c_size_t(0)
        kernel32.WriteProcessMemory(
            handle, dll_path_addr, dll_path_bytes,
            len(dll_path_bytes), ctypes.byref(written)
        )

        loadlib_addr = kernel32.GetProcAddress(
            kernel32.GetModuleHandleA(b"kernel32.dll"), b"LoadLibraryA"
        )

        thread_id = ctypes.c_ulong(0)
        thread = kernel32.CreateRemoteThread(
            handle, None, 0, loadlib_addr,
            dll_path_addr, 0, ctypes.byref(thread_id)
        )
        if not thread:
            err = ctypes.GetLastError()
            raise RuntimeError(f"CreateRemoteThread failed: error {err}")

        kernel32.WaitForSingleObject(thread, 10000)
        kernel32.CloseHandle(thread)

    finally:
        kernel32.VirtualFreeEx(handle, dll_path_addr, 0, 0x8000)
        kernel32.CloseHandle(handle)

    return True


def inject_remotethread_ex(pid, dll_path):
    """
    Evasive injection using NtCreateThreadEx with HIDE_FROM_DEBUGGER flag.

    Hides the remote thread from debugger events, helping evade Steam DRM
    (Steam Stub) and simple anti-cheat thread detection.
    Falls back to standard CreateRemoteThread if NtCreateThreadEx fails.
    """
    dll_path_str = str(dll_path)
    dll_path_bytes = dll_path_str.encode('utf-8') + b'\x00'

    # THREAD_CREATE_THREAD_INIT | THREAD_TERMINATE | THREAD_SUSPEND_RESUME |
    # THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION |
    # THREAD_SET_THREAD_TOKEN | THREAD_IMPERSONATE | THREAD_DIRECT_IMPERSONATION
    needed = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION

    for rights in [needed, PROCESS_ALL_ACCESS]:
        handle = kernel32.OpenProcess(rights, False, pid)
        if handle:
            break
    if not handle:
        err = ctypes.GetLastError()
        raise RuntimeError(f"OpenProcess failed for PID {pid}: error {err}")

    try:
        dll_path_addr = kernel32.VirtualAllocEx(
            handle, None, len(dll_path_bytes) + 1,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        )
        if not dll_path_addr:
            err = ctypes.GetLastError()
            raise RuntimeError(f"VirtualAllocEx failed: error {err}")

        written = ctypes.c_size_t(0)
        kernel32.WriteProcessMemory(
            handle, dll_path_addr, dll_path_bytes,
            len(dll_path_bytes), ctypes.byref(written)
        )

        loadlib_addr = kernel32.GetProcAddress(
            kernel32.GetModuleHandleA(b"kernel32.dll"), b"LoadLibraryA"
        )

        # Try NtCreateThreadEx with HIDE_FROM_DEBUGGER first
        thread_handle = ctypes.wintypes.HANDLE(0)
        print("  Using NtCreateThreadEx with HIDE_FROM_DEBUGGER...")
        status = ntdll.NtCreateThreadEx(
            ctypes.byref(thread_handle),
            THREAD_ALL_ACCESS,
            None,           # ObjectAttributes
            handle,         # ProcessHandle
            loadlib_addr,   # StartRoutine
            dll_path_addr,  # Argument
            THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER,
            0,              # ZeroBits
            0,              # StackSize
            0,              # MaximumStackSize
            None            # AttributeList
        )

        if status != NTSTATUS_SUCCESS or not thread_handle.value:
            print(f"  NtCreateThreadEx failed (status={status:#x}), falling back to CreateRemoteThread...")
            thread_id = ctypes.c_ulong(0)
            thread = kernel32.CreateRemoteThread(
                handle, None, 0, loadlib_addr,
                dll_path_addr, 0, ctypes.byref(thread_id)
            )
            if not thread:
                err = ctypes.GetLastError()
                raise RuntimeError(f"CreateRemoteThread failed: error {err}")
            kernel32.WaitForSingleObject(thread, 10000)
            kernel32.CloseHandle(thread)
        else:
            kernel32.WaitForSingleObject(thread_handle.value, 10000)
            kernel32.CloseHandle(thread_handle.value)

    finally:
        kernel32.VirtualFreeEx(handle, dll_path_addr, 0, 0x8000)
        kernel32.CloseHandle(handle)

    return True


def main():
    parser = argparse.ArgumentParser(
        description="VR Game Converter - DLL Injector"
    )
    parser.add_argument("--target", help="Target process name (e.g., game.exe)")
    parser.add_argument("--pid", type=int, help="Target process ID")
    parser.add_argument("--launch", help="Launch a game executable and inject")
    parser.add_argument("--dll", required=True, help="Path to vr_converter.dll")
    parser.add_argument("--method", choices=["hook", "remotethread", "remotethread_ex"],
                        default="remotethread_ex", help="Injection method (default: remotethread_ex)")

    args = parser.parse_args()

    dll_path = Path(args.dll).resolve()
    if not dll_path.exists():
        print(f"Error: DLL not found: {dll_path}")
        sys.exit(1)

    pid = args.pid
    if args.launch:
        print(f"Launching: {args.launch}")
        proc = subprocess.Popen(args.launch)
        pid = proc.pid
        time.sleep(2)
    elif args.target:
        print(f"Finding process: {args.target}")
        pid = get_process_id_by_name(args.target)

    if not pid:
        parser.print_help()
        sys.exit(1)

    print(f"Injecting {dll_path} into PID {pid}...")

    try:
        if args.method == "hook":
            try:
                inject_setwindowsHookEx(pid, str(dll_path))
            except Exception as hook_err:
                print(f"  SetWindowsHookEx failed: {hook_err}")
                print("  Falling back to NtCreateThreadEx (evasive)...")
                inject_remotethread_ex(pid, str(dll_path))
        elif args.method == "remotethread":
            inject_remotethread(pid, str(dll_path))
        else:  # remotethread_ex (default)
            inject_remotethread_ex(pid, str(dll_path))
        print(f"Successfully injected into PID {pid}")
        print("Press F2 in-game to toggle the config overlay")
    except Exception as e:
        print(f"Injection failed: {e}")
        print()
        print("TIP: Games with anti-cheat (Denuvo, EAC, BattlEye) block injection.")
        print("Test with the built-in test app instead:")
        print("  .\\build\\vrc_test_app.exe")
        print("(It auto-loads vr_converter.dll — no injector needed)")
        sys.exit(1)


if __name__ == "__main__":
    main()
