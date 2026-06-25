#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: injector.exe <process_name> <dll_path>\n");
        printf("  process_name: e.g. ReadyOrNotSteam-Win64-Shipping.exe\n");
        printf("  dll_path: full path to vr_converter.dll\n");
        return 1;
    }

    const char* target = argv[1];
    const char* dll_path = argv[2];

    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        printf("Failed to create process snapshot\n");
        return 1;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, target, -1, nullptr, 0);
    wchar_t* wtarget = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wtarget, wlen);

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, wtarget) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    free(wtarget);

    if (!pid) {
        printf("Process '%s' not found\n", target);
        return 1;
    }

    printf("Found PID: %lu\n", pid);

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        printf("OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    size_t path_len = strlen(dll_path) + 1;
    void* remote_mem = VirtualAllocEx(proc, nullptr, path_len,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem) {
        printf("VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }

    WriteProcessMemory(proc, remote_mem, dll_path, path_len, nullptr);

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC load_lib = GetProcAddress(kernel32, "LoadLibraryA");

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)load_lib, remote_mem, 0, nullptr);
    if (!thread) {
        printf("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    printf("DLL injected. Waiting for LoadLibrary to complete...\n");
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);

    printf("Injection complete. DllMain will initialize in ~200ms.\n");
    return 0;
}