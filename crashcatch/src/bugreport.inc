struct BugAsk {
    std::atomic<bool> busy{false}, done{false};
    std::wstring dir;
    std::wstring gameRoot, gameBin;
    HWND window = nullptr;
    std::string said;
    std::string top;
    std::string body;
    SYSTEMTIME at{};
};
BugAsk g_bug;

bool SaveShot(HWND w, const std::wstring& file) {
    RECT rc;
    if (!w || !GetClientRect(w, &rc)) return false;
    POINT tl{0, 0};
    ClientToScreen(w, &tl);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, cw, ch, screen, tl.x, tl.y, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(mem, old);
    }
    if (ok) {
        ok = false;
        IWICImagingFactory* f = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
            IWICBitmap* src = nullptr;
            IWICStream* st = nullptr;
            IWICBitmapEncoder* enc = nullptr;
            IWICBitmapFrameEncode* fr = nullptr;
            IPropertyBag2* bag = nullptr;
            if (SUCCEEDED(f->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppBGR, cw * 4, cw * ch * 4,
                                                    static_cast<BYTE*>(bits), &src)) &&
                SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
                SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &bag))) {
                PROPBAG2 opt{};
                opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_R4;
                v.fltVal = 0.88f;
                bag->Write(1, &opt, &v);
                WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                ok = SUCCEEDED(fr->Initialize(bag)) && SUCCEEDED(fr->SetSize(cw, ch)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
                     SUCCEEDED(fr->WriteSource(src, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
            }
            for (IUnknown* u : std::initializer_list<IUnknown*>{bag, fr, enc, st, src})
                if (u) u->Release();
            f->Release();
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

std::string PipeOf(const std::wstring& gameBin) {
    if (const char* e = getenv("TW3SE_PIPE")) return e;
    std::string cfg = ReadFileText(gameBin + L"\\tw3se\\tw3se.config");
    size_t at = 0;
    while (at < cfg.size()) {
        size_t nl = cfg.find('\n', at);
        std::string line = cfg.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? cfg.size() : nl + 1;
        size_t k = line.find_first_not_of(" \t");
        if (k == std::string::npos || line.compare(k, 4, "pipe") != 0) continue;
        size_t eq = line.find('=', k);
        if (eq == std::string::npos) continue;
        std::string v = line.substr(eq + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        while (!v.empty() && strchr(" \t\r\n", v.back())) v.pop_back();
        if (!v.empty()) return v;
    }
    return "tw3se";
}

std::string PipeAsk(const std::string& pipe, const std::string& line) {
    std::wstring name = L"\\\\.\\pipe\\" + Wide(pipe);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (ULONGLONG until = GetTickCount64() + 3000;;) {
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY || GetTickCount64() > until) return "";
        WaitNamedPipeW(name.c_str(), 500);
    }
    std::string out = line + "\n";
    DWORD n = 0;
    WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
    out.clear();
    char buf[4096];
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

const char* kCollect =
    "local R=TW3SE_REPORT or {} local o={} local n=0 "
    "local function ser(v,d) d=d or 0 if type(v)~='table' then return tostring(v) end if d>5 then return '{...}' end "
    "local p={} for k,x in pairs(v) do p[#p+1]=string.rep('  ',d+1)..tostring(k)..' = '..ser(x,d+1) end table.sort(p) "
    "return '{\\n'..table.concat(p,'\\n')..'\\n'..string.rep('  ',d)..'}' end "
    "for name,m in pairs(R) do n=n+1 o[#o+1]='=== '..tostring(name)..' '..tostring(type(m)=='table' and m.version or '') "
    "if type(m)=='table' and type(m.checks)=='function' then local ok,c=pcall(m.checks) "
    "if ok and type(c)=='table' then for _,x in ipairs(c) do o[#o+1]='CHECK|'..tostring(name)..'|'..(x.ok and 'ok' or 'FAIL')"
    "..'|'..tostring(x.name)..'|'..tostring((not x.ok) and x.why or '') end "
    "else o[#o+1]='CHECK|'..tostring(name)..'|FAIL|its checks() failed|'..tostring(c) end end "
    "if type(m)=='table' and type(m.state)=='function' then local ok,s=pcall(m.state) "
    "o[#o+1]=ok and ser(s) or ('its state() failed: '..tostring(s)) end end "
    "local f=w3.fs.folder('tw3se')..'\\\\out\\\\crashcatch_mods.txt' w3.fs.write(f,table.concat(o,'\\n')..'\\n') "
    "return 'mods '..n";

std::string Sha256(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<BYTE> buf(1 << 20);
        DWORD n = 0;
        while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n) BCryptHashData(hash, buf.data(), n, 0);
        BYTE d[32];
        if (BCryptFinishHash(hash, d, sizeof d, 0) == 0)
            for (BYTE b : d) out += Fmt("%02x", b);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

std::pair<std::string, std::string> InstallCheck(const std::wstring& root) {
    std::vector<std::wstring> lists;
    for (const wchar_t* base : {L"\\Mods", L"\\dlc", L"\\bin\\x64_dx12\\tw3se"}) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((root + base + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring l = root + base + L"\\" + fd.cFileName + L"\\crashcatch_files.txt";
            if (Exists(l)) lists.push_back(l);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (lists.empty()) return {"", "no mod of this game ships a crashcatch_files.txt\r\n"};
    std::string top, all;
    for (const auto& l : lists) {
        std::string text = ReadFileText(l), name = Utf8(BaseName(DirName(l)));
        size_t files = 0, okN = 0;
        std::vector<std::string> missing, changed;
        size_t at = 0;
        while (at < text.size()) {
            size_t nl = text.find('\n', at);
            std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? text.size() : nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            if (line[0] == '#') {
                if (files == 0) name = line.substr(line.find_first_not_of("# "));
                continue;
            }
            size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
            if (b == std::string::npos) continue;
            std::string sha = line.substr(0, a), rel = line.substr(b + 1);
            uint64_t size = _strtoui64(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
            ++files;
            std::wstring p = root + L"\\" + Wide(rel);
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) {
                missing.push_back(rel);
                continue;
            }
            uint64_t have = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            if (have != size) {
                changed.push_back(rel + Fmt(" (%llu bytes, shipped %llu)", static_cast<unsigned long long>(have),
                                            static_cast<unsigned long long>(size)));
                continue;
            }
            if (_stricmp(Sha256(p).c_str(), sha.c_str()) != 0) {
                changed.push_back(rel + " (same size, other content)");
                continue;
            }
            ++okN;
        }
        std::string head = Fmt("%s: %zu of %zu files as shipped", name.c_str(), okN, files);
        if (!missing.empty()) head += Fmt(", %zu MISSING", missing.size());
        if (!changed.empty()) head += Fmt(", %zu CHANGED", changed.size());
        all += head + "\r\n";
        for (size_t i = 0; i < missing.size(); ++i) all += (i < 40 ? "    missing  " + missing[i] + "\r\n" : "");
        for (size_t i = 0; i < changed.size(); ++i) all += (i < 40 ? "    changed  " + changed[i] + "\r\n" : "");
        if (missing.size() > 40 || changed.size() > 40) all += "    (the first 40 of each)\r\n";
        if (!missing.empty() || !changed.empty()) top += "  " + head + "\r\n";
    }
    return {top, all};
}

LRESULT CALLBACK AskProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND && LOWORD(wp) == 1) {
        HWND edit = GetDlgItem(w, 2);
        int n = GetWindowTextLengthW(edit);
        std::wstring t(static_cast<size_t>(n) + 1, L'\0');
        GetWindowTextW(edit, t.data(), n + 1);
        t.resize(static_cast<size_t>(n));
        g_bug.said = Utf8(t);
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_CLOSE) {
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void AskWhatIsWrong() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &AskProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"crashcatch_ask";
    RegisterClassW(&wc);
    int W = 560, H = 330;
    HWND w = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"crashcatch - what is wrong?",
                             WS_CAPTION | WS_SYSMENU | WS_VISIBLE, (GetSystemMetrics(SM_CXSCREEN) - W) / 2,
                             (GetSystemMetrics(SM_CYSCREEN) - H) / 2, W, H, nullptr, nullptr, inst, nullptr);
    if (!w) return;
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND lbl = CreateWindowW(L"STATIC",
                             L"crashcatch took a picture of the game and what the mods say about themselves.\n"
                             L"What is wrong? What did you see, what did you expect, what did you do just before?",
                             WS_CHILD | WS_VISIBLE, 14, 10, W - 40, 44, w, nullptr, inst, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 14, 60,
                                W - 44, H - 150, w, reinterpret_cast<HMENU>(2), inst, nullptr);
    HWND btn = CreateWindowW(L"BUTTON", L"Save the report", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, W - 200, H - 82, 170,
                             32, w, reinterpret_cast<HMENU>(1), inst, nullptr);
    for (HWND c : {lbl, edit, btn}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SetForegroundWindow(w);
    SetFocus(edit);
    FLASHWINFO fi{sizeof fi, w, FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0};
    FlashWindowEx(&fi);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &msg) || msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(font);
}

void CollectMods(std::string& top, std::string& body) {
    BugAsk& b = g_bug;
    std::string pipe = PipeOf(b.gameBin);
    std::string pong = PipeAsk(pipe, "ping");
    if (pong.empty()) {
        body += "\r\n[the mods]\r\nTW3SE did not answer on its pipe (" + pipe + "): no TW3SE in this game, or it hangs\r\n";
    } else {
        std::string rep = PipeAsk(pipe, "report");
        CopyIfThere(b.gameBin + L"\\tw3se\\out\\report.txt", b.dir + L"\\tw3se_report.txt", 4u << 20);
        std::string got = PipeAsk(pipe, std::string("lua ") + kCollect);
        std::wstring modsFile = b.gameBin + L"\\tw3se\\out\\crashcatch_mods.txt";
        std::string mods = ReadFileText(modsFile);
        WriteFileText(b.dir + L"\\mods_state.txt", mods);
        DeleteFileW(modsFile.c_str());
        body += "\r\n[the mods: TW3SE_REPORT]\r\nTW3SE: " + rep + "; the mods: " + got + "\r\n";
        std::string failed, okLines;
        size_t at = 0;
        while (at < mods.size()) {
            size_t nl = mods.find('\n', at);
            std::string line = mods.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? mods.size() : nl + 1;
            if (line.rfind("CHECK|", 0) != 0) continue;
            std::string rest = line.substr(6);
            std::vector<std::string> p;
            for (size_t s0 = 0;;) {
                size_t bar = rest.find('|', s0);
                p.push_back(rest.substr(s0, bar == std::string::npos ? std::string::npos : bar - s0));
                if (bar == std::string::npos || p.size() == 4) {
                    if (bar != std::string::npos) p.back() = rest.substr(s0);
                    break;
                }
                s0 = bar + 1;
            }
            while (p.size() < 4) p.push_back("");
            std::string txt = p[0] + ": " + p[2] + (p[3].empty() ? "" : " - " + p[3]);
            if (p[1] == "ok") okLines += "  ok    " + txt + "\r\n";
            else failed += "  FAIL  " + txt + "\r\n";
        }
        if (!failed.empty()) top += "Checks of the mods that failed:\r\n" + failed;
        body += failed + okLines + "(each mod's state: mods_state.txt; TW3SE's report: tw3se_report.txt)\r\n";
    }
    auto [inst, list] = InstallCheck(b.gameRoot);
    if (!inst.empty()) top += "Mod files not as shipped:\r\n" + inst;
    body += "\r\n[the mods' files against what they shipped (crashcatch_files.txt)]\r\n" + list;
}

void BugThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    BugAsk& b = g_bug;
    bool shot = SaveShot(b.window, b.dir + L"\\screenshot.jpg");
    std::string top, body = shot ? "screenshot.jpg: the game's window at the moment of the report\r\n"
                                 : "no screenshot (the game's window was not found or not on screen)\r\n";
    std::string mTop, mBody;
    std::thread mods(CollectMods, std::ref(mTop), std::ref(mBody));
    AskWhatIsWrong();
    mods.join();
    b.top = top + mTop;
    b.body = body + mBody;
    CoUninitialize();
    b.done = true;
}
