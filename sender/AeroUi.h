#pragma once

// Presentation only. AudioEngine and its existing event handlers remain in main.cpp.
namespace Aero {
struct Palette {
    Color bg, card, inset, border, text, muted, accent, soft, onAccent, danger;
};
struct FocusTarget { RECT rect; SliderControl* slider = nullptr; };
struct State {
    bool dark = false, visualizers = true, suppressCharacter = false;
    int page = 0, focus = -1;
    float scale = 1.0f;
    RECT nav[4]{}, light{}, darkButton{}, visuals{};
    std::vector<FocusTarget> targets;
} ui;

Palette Colors() {
    if (ui.dark) return {Color(255,15,25,26), Color(255,23,35,36), Color(255,30,45,46),
        Color(255,48,66,67), Color(255,237,247,246), Color(255,160,183,181),
        Color(255,103,224,194), Color(255,35,70,64), Color(255,11,43,35), Color(255,255,150,158)};
    return {Color(255,242,247,247), Color(255,255,255,255), Color(255,237,244,243),
        Color(255,218,230,228), Color(255,23,44,43), Color(255,91,116,113),
        Color(255,0,121,99), Color(255,220,245,237), Color(255,255,255,255), Color(255,180,48,65)};
}

std::wstring PreferencePath() {
    wchar_t path[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (!length || length >= MAX_PATH) return L"";
    std::wstring folder = std::wstring(path) + L"\\NullWire";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\appearance.ini";
}
void ApplyTheme(HWND hwnd, bool dark, bool save = true) {
    ui.dark = dark;
    BOOL enabled = dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
    if (save) {
        auto path = PreferencePath();
        if (!path.empty()) WritePrivateProfileStringW(L"Appearance", L"Dark", dark ? L"1" : L"0", path.c_str());
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}
void Initialize(HWND hwnd) {
    auto path = PreferencePath();
    ApplyTheme(hwnd, !path.empty() && GetPrivateProfileIntW(L"Appearance", L"Dark", 0, path.c_str()), false);
}
RectF Box(RECT r) { return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right-r.left), (REAL)(r.bottom-r.top)); }
RECT Hit(float x, float y, float w, float h) { return {(LONG)x,(LONG)y,(LONG)(x+w),(LONG)(y+h)}; }
POINT LogicalPoint(LPARAM param) {
    return {(LONG)(GET_X_LPARAM(param)/ui.scale), (LONG)(GET_Y_LPARAM(param)/ui.scale)};
}
void Round(Graphics& g, RectF r, Color fill, Color border, float radius = 16) {
    if (r.Width <= 0 || r.Height <= 0) return;
    radius = std::min(radius, std::min(r.Width,r.Height)/2);
    GraphicsPath path;
    float d = radius*2;
    path.AddArc(r.X,r.Y,d,d,180,90); path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90); path.AddArc(r.X,r.GetBottom()-d,d,d,90,90);
    path.CloseFigure();
    SolidBrush brush(fill); Pen pen(border,1);
    g.FillPath(&brush,&path); g.DrawPath(&pen,&path);
}
void Text(Graphics& g, const std::wstring& text, RectF r, float size, Color color,
          bool bold = false, StringAlignment align = StringAlignmentNear) {
    Font font(L"Segoe UI",size,bold ? FontStyleBold : FontStyleRegular,UnitPixel);
    SolidBrush brush(color); StringFormat format;
    format.SetAlignment(align); format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter); format.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(),-1,&font,r,&format,&brush);
}
void Paragraph(Graphics& g, const std::wstring& text, RectF r, Color color) {
    Font font(L"Segoe UI",14,FontStyleRegular,UnitPixel); SolidBrush brush(color);
    g.DrawString(text.c_str(),-1,&font,r,nullptr,&brush);
}
void Register(RECT r, SliderControl* slider = nullptr) { ui.targets.push_back({r,slider}); }
void Button(Graphics& g, RECT r, const std::wstring& label, bool selected = false, bool danger = false) {
    auto p=Colors();
    Round(g,Box(r),selected ? p.accent : p.inset,selected ? p.accent : p.border,10);
    Text(g,label,Box(r),14,selected ? p.onAccent : danger ? p.danger : p.text,true,StringAlignmentCenter);
    Register(r);
}
void Toggle(Graphics& g, RECT r, const std::wstring& label, bool enabled) {
    auto p=Colors(); auto b=Box(r);
    Text(g,label,RectF(b.X,b.Y,b.Width-52,b.Height),14,p.text);
    RectF pill(b.GetRight()-42,b.Y+(b.Height-22)/2,42,22);
    Round(g,pill,enabled ? p.accent : p.border,enabled ? p.accent : p.border,11);
    SolidBrush knob(enabled ? p.onAccent : p.card);
    g.FillEllipse(&knob,pill.X+(enabled ? 23.0f : 3.0f),pill.Y+3,16.0f,16.0f);
    Register(r);
}
void ThemeSwitch(Graphics& g, float x, float y) {
    ui.light=Hit(x,y,84,36); ui.darkButton=Hit(x+90,y,84,36);
    Button(g,ui.light,L"Light",!ui.dark); Button(g,ui.darkButton,L"Dark",ui.dark);
}
void ResetHitBoxes() {
    g_App.streamButtonRect={}; g_App.deviceDropdownRect={}; g_App.ipInputRect={};
    g_App.autoConnectRect={}; g_App.wifiToggleRect={};
    g_App.subBassSlider.rect={}; g_App.vocalPresenceSlider.rect={};
    g_App.trebleAirSlider.rect={}; g_App.surround3DSlider.rect={};
    for(auto& r:g_App.presetPillsRect) r={};
    ui.visuals={}; ui.targets.clear();
}
void Icon(Graphics& g, float x, float y, int kind, Color color) {
    Pen pen(color,1.8f);
    if(kind==0) { // Overview / home
        g.DrawLine(&pen,x,y+9,x+9,y); g.DrawLine(&pen,x+9,y,x+18,y+9);
        g.DrawRectangle(&pen,x+3,y+8,12.0f,11.0f);
    } else if(kind==1) {
        for(int i=0;i<4;++i) { float h=i%2 ? 18.0f:10.0f; g.DrawLine(&pen,x+i*5,y+(18-h)/2,x+i*5,y+(18+h)/2); }
    } else if(kind==2) {
        g.DrawRectangle(&pen,x+4,y,11.0f,19.0f); g.DrawLine(&pen,x+8,y+16,x+11,y+16);
    } else {
        g.DrawEllipse(&pen,x+1,y+1,17.0f,17.0f); g.DrawEllipse(&pen,x+6,y+6,7.0f,7.0f);
        for(int i=0;i<4;++i) { float a=i*1.5708f; g.DrawLine(&pen,x+9+8*std::cos(a),y+9+8*std::sin(a),x+9+12*std::cos(a),y+9+12*std::sin(a)); }
    }
}
void Route(Graphics& g, RectF r, bool detailed = false) {
    auto p=Colors(); bool live=g_App.engine.isStreaming.load();
    Round(g,r,p.card,p.border);
    float half=r.Width/2;
    Text(g,L"THIS COMPUTER",RectF(r.X+22,r.Y+14,half-36,20),11,p.muted,true);
    Text(g,g_App.hostPcName,RectF(r.X+22,r.Y+38,half-36,27),20,p.text,true);
    Text(g,g_App.hostIp,RectF(r.X+22,r.Y+68,half-36,20),12,p.muted);
    Text(g,L"RECEIVER",RectF(r.X+half+22,r.Y+14,half-44,20),11,p.muted,true);
    Text(g,g_App.isPhoneDiscovered ? g_App.targetDeviceName : L"Your Android phone",RectF(r.X+half+22,r.Y+38,half-44,27),20,p.text,true);
    Text(g,live ? L"Streaming  /  48 kHz stereo PCM" : g_App.isPhoneDiscovered ? L"Discovered · Ready to connect" : L"Open NullWire on your phone",RectF(r.X+half+22,r.Y+68,half-44,20),12,p.muted);
    Pen route(p.accent,2); route.SetDashStyle(DashStyleDot);
    g.DrawLine(&route,r.X+half-22,r.Y+48,r.X+half+6,r.Y+48);
    g_App.deviceDropdownRect=Hit(r.X+22,r.Y+102,half-44,38);
    std::wstring device=L"Select audio output";
    int index=g_App.engine.selectedDeviceIndex;
    if(index>=0 && index<(int)g_App.engine.devices.size()) device=g_App.engine.devices[index].name;
    Round(g,Box(g_App.deviceDropdownRect),p.inset,g_App.isDeviceHovered ? p.accent : p.border,8);
    Text(g,device,RectF(r.X+34,r.Y+102,half-80,38),13,p.text);
    Text(g,L"⌄",RectF(r.X+half-49,r.Y+102,20,38),16,p.muted);
    Register(g_App.deviceDropdownRect);
    g_App.ipInputRect=Hit(r.X+half+22,r.Y+102,half-44,38);
    Round(g,Box(g_App.ipInputRect),p.inset,g_App.isIpEditing ? p.accent : p.border,8);
    auto ip=g_App.targetIp.empty() ? L"Enter phone IP" : g_App.targetIp;
    if(g_App.isIpEditing) ip+=L" |";
    Text(g,ip,RectF(r.X+half+34,r.Y+102,half-68,38),13,p.text);
    Register(g_App.ipInputRect);
    if(detailed) {
        Paragraph(g,L"Choose the Windows output playing your audio. The receiver appears here when discovered on your local network. You can also enter its IP address manually.",RectF(r.X+22,r.Y+168,r.Width-44,68),p.muted);
        Text(g,L"Pairing and connection",RectF(r.X+22,r.Y+250,r.Width-44,28),18,p.text,true);
        Paragraph(g,L"Open the Android app and tap Start listening. Enter the pairing code when prompted by Windows. Latency, jitter and remote buffer health are not measured by this sender.",RectF(r.X+22,r.Y+290,r.Width-44,64),p.muted);
    }
}
void Visualizer(Graphics& g, RectF r) {
    auto p=Colors(); Round(g,r,p.card,p.border);
    Text(g,L"Live audio",RectF(r.X+22,r.Y+14,r.Width-44,28),18,p.text,true);
    Text(g,L"Stereo levels · Spectrum · Waveform",RectF(r.X+22,r.Y+44,r.Width-44,22),12,p.muted);
    if(!ui.visualizers) {
        Text(g,L"Visualizers paused",RectF(r.X+22,r.Y+100,r.Width-44,32),18,p.muted,true);
        Text(g,L"Enable them in Settings. Audio keeps playing.",RectF(r.X+22,r.Y+140,r.Width-44,24),12,p.muted);
        return;
    }
    float w=r.Width-44;
    for(int i=0;i<2;++i) {
        float y=r.Y+82+i*22;
        Text(g,i ? L"R" : L"L",RectF(r.X+22,y-4,18,20),11,p.muted,true);
        Round(g,RectF(r.X+44,y,w-22,7),p.inset,p.inset,3);
        float level=std::clamp(i ? g_App.vuRightLevel : g_App.vuLeftLevel,0.0f,1.0f);
        if(level>0.005f) Round(g,RectF(r.X+44,y,(w-22)*level,7),p.accent,p.accent,3);
    }
    float graphY=r.Y+132, graphH=std::max(50.0f,r.Height-230);
    Pen grid(p.border,1);
    for(int row=0;row<4;++row) g.DrawLine(&grid,r.X+22,graphY+graphH*row/3,r.GetRight()-22,graphY+graphH*row/3);
    const wchar_t* labels[]={L"60",L"150",L"400",L"1k",L"2.5k",L"6k",L"14k"};
    for(int i=0;i<7;++i) {
        float cell=w/7, x=r.X+22+i*cell;
        float amplitude=std::clamp(g_App.spectrumBands[i],0.0f,1.0f);
        SolidBrush bars(p.accent);
        // Each group represents one real engine band; no synthetic animation/data.
        for(int j=0;j<4;++j) {
            float bh=std::max(1.0f,graphH*amplitude);
            g.FillRectangle(&bars,x+3+j*(cell-6)/4,graphY+graphH-bh,(cell-6)/4-3,bh);
        }
        Text(g,labels[i],RectF(x,graphY+graphH+3,cell,18),10,p.muted,false,StringAlignmentCenter);
    }
    RectF wave(r.X+22,r.GetBottom()-57,w,34);
    g.DrawLine(&grid,wave.X,wave.Y+17,wave.GetRight(),wave.Y+17);
    if(g_App.waveformDisplay.size()>1) {
        std::vector<PointF> points;
        for(size_t i=0;i<g_App.waveformDisplay.size();++i)
            points.emplace_back(wave.X+wave.Width*i/(g_App.waveformDisplay.size()-1),wave.Y+17-std::clamp(g_App.waveformDisplay[i],-1.0f,1.0f)*16);
        Pen line(p.accent,1.6f); g.DrawLines(&line,points.data(),(INT)points.size());
    }
}
void Slider(Graphics& g, SliderControl& s, float x, float y, float w) {
    auto p=Colors(); s.rect=Hit(x,y,w,41); s.accentColor=p.accent;
    Text(g,s.label,RectF(x,y,w-80,22),13,p.text);
    float value=s.minValue+s.value*(s.maxValue-s.minValue);
    std::wstringstream number; if(value>0 && s.minValue<0) number<<L"+";
    number<<std::fixed<<std::setprecision(s.unit==L"%" ? 0 : 1)<<value<<L" "<<s.unit;
    Text(g,number.str(),RectF(x+w-80,y,80,22),13,p.accent,true,StringAlignmentFar);
    Round(g,RectF(x,y+34,w,5),p.inset,p.inset,2);
    float fill=w*s.value;
    if(fill>2) Round(g,RectF(x,y+34,fill,5),p.accent,p.accent,2);
    SolidBrush knob(p.card); Pen edge(p.accent,s.isHovered || s.isDragging ? 3.0f : 2.0f);
    g.FillEllipse(&knob,x+fill-7,y+29,14.0f,14.0f); g.DrawEllipse(&edge,x+fill-7,y+29,14.0f,14.0f);
    Register(s.rect,&s);
}
void Sound(Graphics& g, RectF r) {
    auto p=Colors(); Round(g,r,p.card,p.border);
    Text(g,L"Sound shaping",RectF(r.X+22,r.Y+14,r.Width-44,28),18,p.text,true);
    SliderControl* sliders[]={&g_App.subBassSlider,&g_App.vocalPresenceSlider,&g_App.trebleAirSlider,&g_App.surround3DSlider};
    float step=(r.Height-128)/4;
    for(int i=0;i<4;++i) Slider(g,*sliders[i],r.X+22,r.Y+58+i*step,r.Width-44);
    const wchar_t* presets[]={L"Pure",L"Studio",L"Cinema",L"Game"};
    float pw=(r.Width-62)/4;
    for(int i=0;i<4;++i) {
        g_App.presetPillsRect[i]=Hit(r.X+22+i*(pw+6),r.GetBottom()-52,pw,32);
        Button(g,g_App.presetPillsRect[i],presets[i],(int)g_App.engine.currentPreset.load()==i);
    }
}
void ConnectionOptions(Graphics& g, float x, float y, float w) {
    g_App.autoConnectRect=Hit(x,y,w,42);
    Toggle(g,g_App.autoConnectRect,L"Auto-connect",g_App.autoConnect);
    g_App.wifiToggleRect=Hit(x,y+54,w,42);
    Toggle(g,g_App.wifiToggleRect,L"Wi-Fi FastPath",g_App.engine.enableWifiAccel.load());
}
void Metrics(Graphics& g, RectF r) {
    auto p=Colors(); Round(g,r,p.card,p.border);
    std::wstringstream packets; packets<<g_App.engine.sendPacketCount.load();
    // The current sender simulates latency/jitter/buffer metrics. Do not present
    // those estimates as measurements in the redesigned interface.
    const std::wstring values[]={g_App.engine.isStreaming.load() ? L"48 kHz" : L"Ready",L"Stereo PCM",packets.str()};
    const wchar_t* labels[]={L"Audio stream",L"Lossless transport",L"Packets sent"};
    float step=r.Width/3;
    for(int i=0;i<3;++i) {
        Text(g,values[i],RectF(r.X+22+i*step,r.Y+13,step-40,26),20,p.text,true);
        Text(g,labels[i],RectF(r.X+22+i*step,r.Y+43,step-40,20),12,p.muted);
        if(i) { Pen line(p.border,1); g.DrawLine(&line,r.X+i*step,r.Y+20,r.X+i*step,r.GetBottom()-20); }
    }
}
void Render(HWND hwnd,HDC hdc) {
    RECT client{}; GetClientRect(hwnd,&client);
    int width=client.right,height=client.bottom;
    if(width<=0 || height<=0) return;
    ui.scale=std::min(width/1000.0f,height/700.0f);
    ui.scale=std::max(0.5f,std::min(ui.scale,2.0f));
    float w=width/ui.scale,h=height/ui.scale;
    HDC memory=CreateCompatibleDC(hdc); HBITMAP bitmap=CreateCompatibleBitmap(hdc,width,height);
    auto previous=SelectObject(memory,bitmap);
    {
        Graphics g(memory); g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit); g.ScaleTransform(ui.scale,ui.scale);
        auto p=Colors(); SolidBrush bg(p.bg); g.FillRectangle(&bg,0.0f,0.0f,w,h);
        SolidBrush sidebar(p.card); g.FillRectangle(&sidebar,0.0f,0.0f,172.0f,h);
        ResetHitBoxes();
        Pen brand(p.accent,4); PointF mark[]={{23,42},{30,24},{39,42},{47,24}};
        g.DrawLines(&brand,mark,4);
        Text(g,L"NullWire",RectF(58,20,106,30),20,p.text,true);
        const wchar_t* nav[]={L"Overview",L"Sound",L"Devices",L"Settings"};
        for(int i=0;i<4;++i) {
            ui.nav[i]=Hit(12,96+i*54,148,44);
            if(ui.page==i) Round(g,Box(ui.nav[i]),p.soft,p.soft,10);
            Icon(g,26,108+i*54,i,ui.page==i ? p.accent : p.muted);
            Text(g,nav[i],RectF(58,96+i*54,100,44),14,ui.page==i ? p.accent : p.muted,ui.page==i);
            Register(ui.nav[i]);
        }
        Text(g,L"WIRELESS AUDIO",RectF(22,h-64,144,20),10,p.muted,true);
        Text(g,L"Windows → Android",RectF(22,h-42,144,20),11,p.muted);
        float x=198,cw=w-x-26;
        const wchar_t* titles[]={L"Your sound. Everywhere.",L"Make it sound like you.",L"A simple connection.",L"Make yourself at home."};
        Text(g,titles[ui.page],RectF(x,22,cw-190,32),24,p.text,true);
        Text(g,g_App.engine.isStreaming.load() ? L"● Streaming audio" : L"● Ready when you are",RectF(x,58,cw-190,22),12,p.accent);
        ThemeSwitch(g,w-200,30);
        float bottom=h-106;
        if(ui.page==0) {
            Route(g,RectF(x,102,cw,158));
            float middle=bottom-18-278,left=(cw-16)*0.55f;
            Visualizer(g,RectF(x,278,left,middle));
            Sound(g,RectF(x+left+16,278,cw-left-16,middle));
            Metrics(g,RectF(x,bottom,cw-176,80));
            g_App.streamButtonRect=Hit(w-186,bottom,160,80);
            Button(g,g_App.streamButtonRect,g_App.engine.isStreaming.load() ? L"Disconnect" : L"Start stream",!g_App.engine.isStreaming.load(),g_App.engine.isStreaming.load());
        } else if(ui.page==1) {
            Sound(g,RectF(x,102,cw*0.52f,h-128));
            Visualizer(g,RectF(x+cw*0.52f+16,102,cw*0.48f-16,h-128));
        } else if(ui.page==2) {
            Route(g,RectF(x,102,cw,370),true);
            Round(g,RectF(x,490,cw,h-516),p.card,p.border);
            ConnectionOptions(g,x+22,504,cw-230);
            g_App.streamButtonRect=Hit(w-202,520,154,48);
            Button(g,g_App.streamButtonRect,g_App.engine.isStreaming.load() ? L"Disconnect" : L"Start stream",!g_App.engine.isStreaming.load(),g_App.engine.isStreaming.load());
        } else {
            Round(g,RectF(x,102,cw,142),p.card,p.border);
            Text(g,L"Appearance",RectF(x+24,120,cw-48,30),18,p.text,true);
            Paragraph(g,L"Choose Light or Dark above. Your preference is remembered on this computer.",RectF(x+24,158,cw-48,50),p.muted);
            Round(g,RectF(x,262,cw,186),p.card,p.border);
            Text(g,L"Connection preferences",RectF(x+24,278,cw-48,30),18,p.text,true);
            ConnectionOptions(g,x+24,326,cw-48);
            Round(g,RectF(x,466,cw,h-492),p.card,p.border);
            ui.visuals=Hit(x+24,480,cw-48,44);
            Toggle(g,ui.visuals,L"Live visualizers",ui.visualizers);
            Paragraph(g,L"Hide meters, spectrum and waveform when you prefer a quieter workspace. This only changes the display.",RectF(x+24,540,cw-48,60),p.muted);
        }
        if(ui.focus>=0 && ui.focus<(int)ui.targets.size()) {
            Pen focus(p.accent,2); focus.SetDashStyle(DashStyleDot);
            auto r=Box(ui.targets[ui.focus].rect); r.Inflate(3,3); g.DrawRectangle(&focus,r);
        }
        if(g_App.toast.active) {
            float elapsed=std::chrono::duration<float>(std::chrono::steady_clock::now()-g_App.toast.startTime).count();
            if(elapsed>=g_App.toast.durationSec) g_App.toast.active=false;
            else {
                RectF toast(x+24,h-102,cw-48,76); Round(g,toast,p.card,p.accent,12);
                Text(g,g_App.toast.title,RectF(toast.X+18,toast.Y+10,toast.Width-36,24),13,p.accent,true);
                Text(g,g_App.toast.message,RectF(toast.X+18,toast.Y+36,toast.Width-36,26),12,p.text);
            }
        }
    }
    BitBlt(hdc,0,0,width,height,memory,0,0,SRCCOPY);
    SelectObject(memory,previous); DeleteObject(bitmap); DeleteDC(memory);
}
bool Click(HWND hwnd,POINT point) {
    for(int i=0;i<4;++i) if(PtInRect(&ui.nav[i],point)) {
        ui.page=i; ui.focus=-1; g_App.isIpEditing=false;
        // Repaint now so keyboard activation cannot use the previous page's hit boxes.
        InvalidateRect(hwnd,nullptr,FALSE); UpdateWindow(hwnd); return true;
    }
    if(PtInRect(&ui.light,point) || PtInRect(&ui.darkButton,point)) {
        ApplyTheme(hwnd,PtInRect(&ui.darkButton,point)); return true;
    }
    if(PtInRect(&ui.visuals,point)) {
        ui.visualizers=!ui.visualizers; InvalidateRect(hwnd,nullptr,FALSE); return true;
    }
    return false;
}
bool Key(HWND hwnd,WPARAM key) {
    if(key==VK_TAB && !ui.targets.empty()) {
        int direction=(GetKeyState(VK_SHIFT)&0x8000) ? -1 : 1;
        int count=(int)ui.targets.size();
        ui.focus=ui.focus<0 ? (direction>0 ? 0 : count-1) : (ui.focus+direction+count)%count;
        g_App.isIpEditing=false; InvalidateRect(hwnd,nullptr,FALSE); return true;
    }
    if(key==VK_ESCAPE) { g_App.isIpEditing=false; ui.focus=-1; InvalidateRect(hwnd,nullptr,FALSE); return true; }
    if(g_App.isIpEditing || ui.focus<0 || ui.focus>=(int)ui.targets.size()) return false;
    auto target=ui.targets[ui.focus];
    float x=(target.rect.left+target.rect.right)/2.0f,y=(target.rect.top+target.rect.bottom)/2.0f;
    if(target.slider && (key==VK_LEFT || key==VK_RIGHT || key==VK_HOME || key==VK_END)) {
        float value=target.slider->value;
        value=key==VK_HOME ? 0 : key==VK_END ? 1 : std::clamp(value+(key==VK_RIGHT ? 0.02f : -0.02f),0.0f,1.0f);
        x=target.rect.left+(target.rect.right-target.rect.left)*value;
        // Keep the hit point inside the control at the endpoint; dragging can reach 100%.
        LPARAM down=MAKELPARAM((int)(std::min(x,(float)target.rect.right-1)*ui.scale),(int)(y*ui.scale));
        SendMessage(hwnd,WM_LBUTTONDOWN,0,down);
        SendMessage(hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM((int)(x*ui.scale),(int)(y*ui.scale)));
        SendMessage(hwnd,WM_LBUTTONUP,0,down); InvalidateRect(hwnd,nullptr,FALSE); return true;
    }
    if((key==VK_RETURN || key==VK_SPACE) && !target.slider) {
        ui.suppressCharacter=true;
        LPARAM pos=MAKELPARAM((int)(x*ui.scale),(int)(y*ui.scale));
        SendMessage(hwnd,WM_LBUTTONDOWN,0,pos); SendMessage(hwnd,WM_LBUTTONUP,0,pos); return true;
    }
    return false;
}
} // namespace Aero

void RenderFrame(HWND hwnd,HDC hdc) { Aero::Render(hwnd,hdc); }
