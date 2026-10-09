// 用 DIA SDK 把崩溃 RVA 映射为 函数名 + 源码文件行号
#include <windows.h>
#include <diacreate.h>
#include <dia2.h>
#include <stdio.h>

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 3) { printf("usage: map_rva <pdb> <rva_hex>\n"); return 1; }
    const wchar_t* pdb = argv[1];
    unsigned __int64 rva = _wcstoui64(argv[2], nullptr, 16);

    IDiaDataSource* src = nullptr;
    HRESULT hr = NoRegCoCreate(L"msdia140.dll", CLSID_DiaSource,
                               IID_IDiaDataSource, (void**)&src);
    if (FAILED(hr) || !src) { printf("create DiaSource failed hr=%08x\n", (unsigned)hr); return 2; }
    if (FAILED(src->loadDataFromPdb(pdb))) { printf("loadDataFromPdb failed\n"); return 3; }
    IDiaSession* sess = nullptr;
    if (FAILED(src->openSession(&sess))) { printf("openSession failed\n"); return 3; }

    IDiaSymbol* fundata = nullptr;
    sess->findSymbolByRVA((DWORD)rva, SymTagFunction, &fundata);
    if (fundata) {
        BSTR n = nullptr; fundata->get_name(&n);
        DWORD rvaStart = 0; ULONGLONG len = 0;
        fundata->get_relativeVirtualAddress(&rvaStart);
        fundata->get_length(&len);
        printf("FUNCTION RVA=0x%x len=%llu name=%S\n\n", rvaStart, len, n ? n : L"?");
        if (n) SysFreeString(n);
        fundata->Release();
    } else {
        printf("no function symbol at RVA 0x%llx\n", rva);
    }

    IDiaEnumLineNumbers* lines = nullptr;
    if (SUCCEEDED(sess->findLinesByRVA((DWORD)rva, 1, &lines)) && lines) {
        IDiaLineNumber* ln = nullptr;
        DWORD celt = 0;
        while (SUCCEEDED(lines->Next(1, &ln, &celt)) && celt == 1 && ln) {
            DWORD lineNo = 0; ln->get_lineNumber(&lineNo);
            IDiaSourceFile* comp = nullptr; ln->get_sourceFile(&comp);
            BSTR fn = nullptr; if (comp) { comp->get_fileName(&fn); comp->Release(); }
            printf("LINE %u  file=%S\n", lineNo, fn ? fn : L"?");
            if (fn) SysFreeString(fn);
            ln->Release();
        }
        lines->Release();
    } else {
        printf("findLinesByRVA failed\n");
    }
    return 0;
}