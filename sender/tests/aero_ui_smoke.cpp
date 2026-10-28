// Compile as a console executable. This harness never starts audio or discovery.
// Optional first argument: an existing directory in which to save rendered PNGs.
#define WinMain NullWireProductionEntry
#include "../main.cpp"
#undef WinMain
#include <cassert>

void SavePreview(HBITMAP image, const std::wstring& path) {
    UINT count=0,bytes=0; GetImageEncodersSize(&count,&bytes);
    std::vector<BYTE> storage(bytes);
    auto encoders=reinterpret_cast<ImageCodecInfo*>(storage.data());
    GetImageEncoders(count,bytes,encoders);
    for(UINT i=0;i<count;++i) if(wcscmp(encoders[i].MimeType,L"image/png")==0) {
        Bitmap bitmap(image,nullptr);
        assert(bitmap.Save(path.c_str(),&encoders[i].Clsid,nullptr)==Ok);
        return;
    }
    assert(false);
}

int main(int argc,char** argv) {
    GdiplusStartupInput startup; ULONG_PTR token;
    assert(GdiplusStartup(&token,&startup,nullptr)==Ok);
    WNDCLASSW testClass{};
    testClass.hInstance=GetModuleHandle(nullptr);
    testClass.lpszClassName=L"NullWireUiTest";
    testClass.lpfnWndProc=[](HWND hwnd,UINT message,WPARAM wp,LPARAM lp)->LRESULT {
        // Exercise the real UI event wiring without discovery, timers or audio startup.
        if(message==WM_CREATE || message==WM_DESTROY || message==WM_TIMER) return 0;
        return WndProc(hwnd,message,wp,lp);
    };
    assert(RegisterClassW(&testClass));
    HWND window=CreateWindowW(testClass.lpszClassName,L"NullWire UI smoke test",WS_POPUP,0,0,1180,800,
        nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    assert(window);
    HDC dc=GetDC(window),memory=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,1180,800);
    auto old=SelectObject(memory,bitmap);
    // Rendering fixtures only. Do not start audio, enumerate devices or broadcast.
    const float bass=g_App.engine.bassBoostDb.load();
    for(int dark=0;dark<2;++dark) {
        Aero::ui.dark=dark;
        for(int page=0;page<4;++page) {
            Aero::ui.page=page;
            RenderFrame(window,memory);
            assert(!Aero::ui.targets.empty());
            for(auto& target:Aero::ui.targets) {
                assert(target.rect.left>=0 && target.rect.top>=0);
                assert(target.rect.right>target.rect.left && target.rect.bottom>target.rect.top);
                assert(target.rect.right*Aero::ui.scale<=1181 && target.rect.bottom*Aero::ui.scale<=801);
            }
            if(page==3) {
                auto r=Aero::ui.visuals;
                assert(Aero::Click(window,{r.left+2,r.top+2}));
                assert(!Aero::ui.visualizers);
                Aero::Click(window,{r.left+2,r.top+2});
                assert(Aero::ui.visualizers);
            }
            if(page>0) assert(IsRectEmpty(&g_App.presetPillsRect[0]) == (page!=1));
            if(argc>1) {
                std::string arg=argv[1]; std::wstring dir(arg.begin(),arg.end());
                SavePreview(bitmap,dir+L"/windows-"+(dark ? L"dark-" : L"light-")+std::to_wstring(page)+L".png");
            }
        }
    }
    assert(!g_App.engine.isStreaming.load());
    assert(g_App.engine.bassBoostDb.load()==bass);
    assert(!g_App.autoConnect || !g_App.isPhoneDiscovered);
    Aero::ui.page=0; RenderFrame(window,memory);
    for(int i=0;i<(int)Aero::ui.targets.size();++i)
        if(EqualRect(&Aero::ui.targets[i].rect,&g_App.ipInputRect)) Aero::ui.focus=i;
    assert(Aero::Key(window,VK_RETURN));
    SendMessageW(window,WM_CHAR,VK_RETURN,0);
    assert(g_App.isIpEditing); // Activation's WM_CHAR must not immediately close editing.
    SendMessageW(window,WM_CHAR,L'1',0);
    assert(g_App.targetIp==L"1");
    SendMessageW(window,WM_CHAR,VK_RETURN,0);
    assert(!g_App.isIpEditing);
    g_App.targetIp.clear();
    SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window,dc); DestroyWindow(window);
    GdiplusShutdown(token);
    std::cout<<"8 themed pages rendered; controls in bounds; view changes preserved audio state; keyboard IP editing passed.\n";
}
