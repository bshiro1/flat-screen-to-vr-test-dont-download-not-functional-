#include <windows.h>

DWORD WINAPI delayed_start(LPVOID) {
    Sleep(500);
    HANDLE f = CreateFileA("C:\\Users\\shiro\\Desktop\\minimal_test.txt", FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const char* msg = "Minimal DLL loaded\r\n";
        DWORD written;
        WriteFile(f, msg, (DWORD)strlen(msg), &written, nullptr);
        CloseHandle(f);
    }
    return 0;
}

extern "C" __declspec(dllexport) void StartMinimal() {
    delayed_start(nullptr);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, delayed_start, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
