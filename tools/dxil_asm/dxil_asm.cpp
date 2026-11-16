// dxil_asm: LLVM IR text (from `dxc -dumpbin`) -> a signed DXIL container (.cso).
//
//     dxil_asm <in.ll> <out.cso>
//
// The IR goes through IDxcAssembler (dxcompiler.dll), which builds the container parts (signatures, PSV) from the
// IR's metadata, then through IDxcValidator (dxil.dll), which checks it and writes the hash D3D12 asks for. Both
// DLLs come from the Windows SDK (bin\<version>\x64) and must sit next to this program or on the PATH.
#include <windows.h>
#include <dxcapi.h>

#include <cstdio>
#include <string>
#include <vector>

static std::vector<char> ReadAll(const char* path) {
    std::vector<char> data;
    FILE* f = fopen(path, "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    data.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(data.data(), 1, data.size(), f);
    fclose(f);
    return data;
}

static void PrintBlob(IDxcBlob* b) {
    if (b && b->GetBufferSize()) fprintf(stderr, "%.*s\n", (int)b->GetBufferSize(), (const char*)b->GetBufferPointer());
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "dxil_asm <in.ll> <out.cso>\n");
        return 2;
    }
    std::vector<char> ir = ReadAll(argv[1]);
    if (ir.empty()) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    HMODULE comp = LoadLibraryA("dxcompiler.dll");
    HMODULE val = LoadLibraryA("dxil.dll");
    if (!comp || !val) {
        fprintf(stderr, "dxcompiler.dll and dxil.dll are needed\n");
        return 1;
    }
    auto createComp = (DxcCreateInstanceProc)GetProcAddress(comp, "DxcCreateInstance");
    auto createVal = (DxcCreateInstanceProc)GetProcAddress(val, "DxcCreateInstance");

    IDxcLibrary* lib = nullptr;
    IDxcAssembler* asmb = nullptr;
    IDxcValidator* validator = nullptr;
    if (FAILED(createComp(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void**)&lib)) ||
        FAILED(createComp(CLSID_DxcAssembler, __uuidof(IDxcAssembler), (void**)&asmb)) ||
        FAILED(createVal(CLSID_DxcValidator, __uuidof(IDxcValidator), (void**)&validator))) {
        fprintf(stderr, "DxcCreateInstance failed\n");
        return 1;
    }
    IDxcBlobEncoding* src = nullptr;
    lib->CreateBlobWithEncodingFromPinned(ir.data(), (UINT32)ir.size(), CP_UTF8, &src);

    IDxcOperationResult* res = nullptr;
    HRESULT hr = asmb->AssembleToContainer(src, &res);
    HRESULT status = E_FAIL;
    if (res) res->GetStatus(&status);
    if (FAILED(hr) || FAILED(status)) {
        fprintf(stderr, "assemble failed (0x%08lx)\n", (unsigned long)(FAILED(hr) ? hr : status));
        IDxcBlobEncoding* err = nullptr;
        if (res && SUCCEEDED(res->GetErrorBuffer(&err))) PrintBlob(err);
        return 1;
    }
    IDxcBlob* container = nullptr;
    res->GetResult(&container);

    IDxcOperationResult* vres = nullptr;
    hr = validator->Validate(container, DxcValidatorFlags_InPlaceEdit, &vres);
    status = E_FAIL;
    if (vres) vres->GetStatus(&status);
    if (FAILED(hr) || FAILED(status)) {
        fprintf(stderr, "validation failed (0x%08lx)\n", (unsigned long)(FAILED(hr) ? hr : status));
        IDxcBlobEncoding* err = nullptr;
        if (vres && SUCCEEDED(vres->GetErrorBuffer(&err))) PrintBlob(err);
        return 1;
    }
    FILE* f = fopen(argv[2], "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite(container->GetBufferPointer(), 1, container->GetBufferSize(), f);
    fclose(f);
    printf("%s: %zu bytes\n", argv[2], (size_t)container->GetBufferSize());
    return 0;
}
