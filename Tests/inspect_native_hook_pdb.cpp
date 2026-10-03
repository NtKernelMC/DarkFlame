#include <Windows.h>
#include <dia2.h>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if(argc != 3) return 1;
    CoInitialize(nullptr);
    HMODULE module = LoadLibraryW(argv[1]);
    if(!module) return 2;
    const auto factory = reinterpret_cast<HRESULT (WINAPI*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(module, "DllGetClassObject"));
    if(!factory) return 3;
    IClassFactory* classFactory{};
    if(FAILED(factory(__uuidof(DiaSource), IID_IClassFactory,
        reinterpret_cast<void**>(&classFactory)))) return 4;
    IDiaDataSource* source{};
    if(FAILED(classFactory->CreateInstance(nullptr, __uuidof(IDiaDataSource),
        reinterpret_cast<void**>(&source)))) return 5;
    classFactory->Release();
    if(FAILED(source->loadDataFromPdb(argv[2]))) return 6;
    IDiaSession* session{};
    if(FAILED(source->openSession(&session))) return 7;
    IDiaSymbol* global{};
    session->get_globalScope(&global);
    IDiaEnumSymbols* symbols{};
    if(FAILED(global->findChildren(SymTagFunction, nullptr, nsNone, &symbols))) return 8;
    IDiaSymbol* symbol{};
    ULONG count{};
    while(SUCCEEDED(symbols->Next(1, &symbol, &count)) && count)
    {
        BSTR name{};
        symbol->get_name(&name);
        if(name && (wcsstr(name, L"HookIsNameAllowed") || wcsstr(name, L"HookOnPreFunction")))
        {
            DWORD rva{};
            ULONGLONG length{};
            symbol->get_relativeVirtualAddress(&rva);
            symbol->get_length(&length);
            std::wprintf(L"%ls RVA=0x%08lX length=0x%llX\n", name, rva, length);
        }
        SysFreeString(name);
        symbol->Release();
    }
    symbols->Release();
    global->Release();
    session->Release();
    source->Release();
    CoUninitialize();
    return 0;
}
