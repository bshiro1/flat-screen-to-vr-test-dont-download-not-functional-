// Small offline DIA SDK utility used by Outlast Trials Phase 0 discovery.
//
//   vrc_pdb_dump.exe <TOTClient-Win64-Shipping.pdb> [filter] [filter ...]
//
// With no filters it prints a useful default set of Unreal symbols (ProcessEvent,
// UObject, UWorld, FName, StaticConstructObject, ...). With filters it prints
// every public symbol whose mangled name contains any filter substring.
//
// Output format is intentionally machine-friendly:
//   0x1A16300 ?ProcessEvent@UObject@@...
//
// The RVA is relative to the module image base and can be added to the base
// address of TOTClient-Win64-Shipping.exe at runtime.

#include <windows.h>

#include <dia2.h>
#include <diacreate.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "diaguids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

void print_help() {
    std::printf(
        "Usage: vrc_pdb_dump.exe <pdb> [filter ...]\n"
        "Default filters: ProcessEvent UObject UWorld FName StaticConstructObject\n");
}

std::wstring to_wide(const char* text) {
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), len);
    out.pop_back();
    return out;
}

bool contains_ci(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [](wchar_t a, wchar_t b) {
                           return std::tolower(static_cast<unsigned short>(a)) ==
                                  std::tolower(static_cast<unsigned short>(b));
                       }) != haystack.end();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::vector<std::wstring> filters;
    for (int i = 2; i < argc; ++i) filters.emplace_back(argv[i]);
    if (filters.empty()) {
        filters = {L"ProcessEvent", L"UObject", L"UWorld", L"FName",
                   L"StaticConstructObject"};
    }

    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::fprintf(stderr, "CoInitialize failed: 0x%08lx\n", hr);
        return 1;
    }

    IDiaDataSource* source = nullptr;
    hr = NoRegCoCreate(L"msdia140.dll", CLSID_DiaSource, IID_IDiaDataSource,
                       reinterpret_cast<void**>(&source));
    if (FAILED(hr) || !source) {
        std::fprintf(stderr, "DIA load failed: 0x%08lx (is msdia140.dll next to this exe?)\n", hr);
        return 1;
    }

    hr = source->loadDataFromPdb(argv[1]);
    if (FAILED(hr)) {
        std::fprintf(stderr, "loadDataFromPdb failed: 0x%08lx\n", hr);
        source->Release();
        return 1;
    }

    IDiaSession* session = nullptr;
    hr = source->openSession(&session);
    if (FAILED(hr) || !session) {
        std::fprintf(stderr, "openSession failed: 0x%08lx\n", hr);
        source->Release();
        return 1;
    }

    IDiaSymbol* global = nullptr;
    hr = session->get_globalScope(&global);
    if (FAILED(hr) || !global) {
        std::fprintf(stderr, "get_globalScope failed: 0x%08lx\n", hr);
        session->Release();
        source->Release();
        return 1;
    }

    IDiaEnumSymbols* symbols = nullptr;
    hr = global->findChildren(SymTagPublicSymbol, nullptr, nsfCaseInsensitive, &symbols);
    if (FAILED(hr) || !symbols) {
        std::fprintf(stderr, "findChildren failed: 0x%08lx\n", hr);
        global->Release();
        session->Release();
        source->Release();
        return 1;
    }

    unsigned long matches = 0;
    ULONG fetched = 0;
    IDiaSymbol* symbol = nullptr;
    while (symbols->Next(1, &symbol, &fetched) == S_OK && fetched == 1) {
        BSTR name_bstr = nullptr;
        DWORD rva = 0;
        if (SUCCEEDED(symbol->get_name(&name_bstr)) && name_bstr &&
            SUCCEEDED(symbol->get_relativeVirtualAddress(&rva))) {
            std::wstring name(name_bstr, SysStringLen(name_bstr));
            bool keep = false;
            for (const auto& filter : filters) {
                if (contains_ci(name, filter)) {
                    keep = true;
                    break;
                }
            }
            if (keep && rva != 0) {
                std::printf("0x%08lX ", rva);
                std::wprintf(L"%ls\n", name.c_str());
                ++matches;
            }
        }
        if (name_bstr) SysFreeString(name_bstr);
        symbol->Release();
        symbol = nullptr;
    }

    std::printf("matches=%lu\n", matches);

    symbols->Release();
    global->Release();
    session->Release();
    source->Release();
    CoUninitialize();
    return 0;
}
