// Injects a DLL into a running process with CreateRemoteThread + LoadLibraryW.
//
// Usage: vrc_injector.exe <process_name> <dll_path>
//
// Exits with 0 only when LoadLibraryW succeeded in the target. The companion
// app checks the exit status and shows stdout (read as UTF-8) on failure.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

namespace {

constexpr DWORD kRemoteThreadTimeoutMs = 10000;

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

DWORD find_process(const wchar_t* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exe_name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

// The remote thread's exit code is only the low 32 bits of the HMODULE, which
// can be 0 for a module mapped on a 4 GiB boundary. Look the module up to tell
// that case apart from a failed LoadLibraryW.
bool module_loaded(DWORD pid, const std::wstring& dll_path) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExePath, dll_path.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 3) {
        printf("Usage: vrc_injector.exe <process_name> <dll_path>\n");
        printf("  process_name: e.g. ReadyOrNotSteam-Win64-Shipping.exe\n");
        printf("  dll_path: path to vr_converter.dll\n");
        return 1;
    }
    const std::wstring target = argv[1];

    // The target resolves a relative path against its own directory and DLL
    // search path, so make it absolute here.
    const DWORD needed = GetFullPathNameW(argv[2], 0, nullptr, nullptr);
    std::wstring dll_path(needed, L'\0');
    const DWORD len = needed ? GetFullPathNameW(argv[2], needed, dll_path.data(), nullptr) : 0;
    if (len == 0 || len >= needed) {
        printf("Invalid DLL path '%s' (error %lu)\n", to_utf8(argv[2]).c_str(), GetLastError());
        return 1;
    }
    dll_path.resize(len);

    const DWORD attrs = GetFileAttributesW(dll_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("DLL not found: %s\n", to_utf8(dll_path).c_str());
        return 1;
    }

    const DWORD pid = find_process(target.c_str());
    if (!pid) {
        printf("Process '%s' not found\n", to_utf8(target).c_str());
        return 1;
    }
    printf("Found PID: %lu\n", pid);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        printf("OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    // LoadLibraryW is taken from our own kernel32, which is mapped at the same
    // address only in processes of the same architecture.
    BOOL self_wow64 = FALSE;
    BOOL target_wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &self_wow64) &&
        IsWow64Process(proc, &target_wow64) && self_wow64 != target_wow64) {
        printf("Architecture mismatch: the injector and '%s' must both be 32-bit or both 64-bit\n",
               to_utf8(target).c_str());
        CloseHandle(proc);
        return 1;
    }

    const SIZE_T path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(proc, nullptr, path_bytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        printf("VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }
    if (!WriteProcessMemory(proc, remote_path, dll_path.c_str(), path_bytes, nullptr)) {
        printf("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = load_library
        ? CreateRemoteThread(proc, nullptr, 0, load_library, remote_path, 0, nullptr)
        : nullptr;
    if (!thread) {
        printf("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    printf("Waiting for LoadLibraryW in the target...\n");
    if (WaitForSingleObject(thread, kRemoteThreadTimeoutMs) != WAIT_OBJECT_0) {
        // The remote thread may still read the path, so the buffer is
        // deliberately left allocated in the target.
        printf("Timed out after %lu ms waiting for LoadLibraryW in the target; "
               "the DLL may or may not load\n", kRemoteThreadTimeoutMs);
        CloseHandle(thread);
        CloseHandle(proc);
        return 2;
    }

    DWORD exit_code = 0;
    const BOOL got_exit_code = GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);

    // Exit code = low 32 bits of the HMODULE returned by LoadLibraryW.
    const bool loaded = got_exit_code && (exit_code != 0 || module_loaded(pid, dll_path));
    CloseHandle(proc);
    if (!loaded) {
        printf("LoadLibraryW failed in the target process for %s "
               "(missing dependency, blocked by anti-cheat, or wrong architecture)\n",
               to_utf8(dll_path).c_str());
        return 1;
    }

    printf("Injected %s into PID %lu\n", to_utf8(dll_path).c_str(), pid);
    return 0;
}
