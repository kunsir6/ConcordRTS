// 战斗内 ESC 菜单是**模态**的（用户 2026-11-16 报「战斗显示跟菜单显示包括声音有冲突」）。
// 模态在这里有三条含义，缺一条用户就能感知到：
//   ① 模拟不走拍（主循环 tickAccumulator=0，R 更早已有）
//   ② 整屏都被菜单盖住 —— contentRect 底部留了 32px 动作条余量，而地图视口是
//      「客户区高度 - 侧栏宽」，那一条在菜单打开时还画着战斗画面
//   ③ 已经在响的战斗音效 / EVA 语音要停 —— 它们不归 tick 管，暂停不会让它们安静
//
// 判据刻意用「哨兵像素」而不是「跟参考图比」：先整屏刷一个绝不会出现在菜单美术里的
// 颜色，再画菜单，然后数剩下多少哨兵像素。这样任何「某块区域没被盖住」都会以一个
// 确定的数字暴露出来，而不需要一张可遇不可求的对照图。
namespace RuntimeMenuModalPauseSmokeDetail {

struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    bool create(int w, int h) {
        width = w; height = h;
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* storage = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
        if (!dc || !bitmap || !storage) return false;
        pixels = static_cast<std::uint32_t*>(storage);
        previous = SelectObject(dc, bitmap);
        return previous && previous != HGDI_ERROR;
    }
    ~Surface() {
        if (dc && previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    void fill(std::uint32_t rgb) {
        if (pixels == nullptr) return;
        std::fill(pixels, pixels + static_cast<std::size_t>(width) * height, rgb);
    }
    // 统计 [l,t,r,b) 里等于 sentinel 的像素数（GDI 的 alpha 通道不可信，只比 RGB）。
    int countRgb(int l, int t, int r, int b, std::uint32_t rgb) const {
        GdiFlush();
        int hits = 0;
        for (int y = std::max(0, t); y < std::min(height, b); ++y) {
            for (int x = std::max(0, l); x < std::min(width, r); ++x) {
                const std::uint32_t pixel =
                    pixels[static_cast<std::size_t>(y) * width + x] & 0x00ffffffU;
                if (pixel == rgb) ++hits;
            }
        }
        return hits;
    }
};

constexpr std::uint32_t kSentinel = 0x00ff00ffU; // 品红：菜单美术里不会出现

} // namespace RuntimeMenuModalPauseSmokeDetail

bool runRuntimeMenuModalPauseSmokeProbe(
    RuntimeWindowState& state,
    std::ostream& output
) {
    using namespace RuntimeMenuModalPauseSmokeDetail;
    const auto fail = [&output](const std::string& why) {
        output << "Menu modal pause failed: " << why << "\n";
        return false;
    };

    state.runtimePreferencePersistenceEnabled = false;
    state.runtimeSoundsEnabled = false;
    state.runtimeMusicEnabled = false;
    state.runtimeSoundEffectsEnabled = false;
    state.runtimeMenuOpen = false;

    // ① 先造「正在响」的现场：两个 SFX 通道 + EVA 通道都在播。
    //    这里注入的是通道簿记（不依赖真的有一块声卡），断言的是「打开菜单必须把它们清掉」
    //    这条产品规则——设备侧是否真出声由苹果音频底座自己负责，本冒烟不声称测到声音。
    state.runtimeSfxMciChannelActive.fill(false);
    state.runtimeSfxBusyUntilTick.fill(0);
    state.runtimeSfxSpatialRecords = {};
    state.runtimeSfxMciChannelActive[0] = true;
    state.runtimeSfxMciChannelActive[3] = true;
    state.runtimeSfxBusyUntilTick[0] = state.tick + 500;
    state.runtimeSfxSpatialRecords[0].intent = "BaseUnderAttackSiren";
    state.runtimeEvaMciChannelActive = true;
    state.runtimeEvaBusyUntilTick = state.tick + 500;
    state.runtimeEvaActivePriority = 3;

    Surface surface;
    if (!surface.create(800, 600)) return fail("offscreen 800x600 surface");
    surface.fill(kSentinel);

    // ② 走产品入口打开菜单（顺带验证音频侧）。
    setRuntimeMenuOpen(state, true);
    if (!state.runtimeMenuOpen) return fail("menu did not open");

    int liveSfxChannels = 0;
    for (std::size_t channel = 0; channel < state.runtimeSfxMciChannelActive.size(); ++channel) {
        if (state.runtimeSfxMciChannelActive[channel]) ++liveSfxChannels;
    }
    const bool sfxStopped = liveSfxChannels == 0
        && state.runtimeSfxSpatialRecords[0].intent != "BaseUnderAttackSiren";
    const bool evaStopped =
        !state.runtimeEvaMciChannelActive && state.runtimeEvaBusyUntilTick == 0;

    // ③ 画菜单，数哨兵。两条判据都先算完再统一报，失败时把两组读数一起打出来——
    //    否则一个红点会把另一个的读数吞掉，下一次就又得猜。
    const RECT client{0, 0, 800, 600};
    drawRuntimeMenuOverlay(surface.dc, client, state);

    // 菜单背景 632x568 只盖到 y=568（contentRect 底部留了 32px 动作条余量），
    // 而地图视口是「客户区高度 - 侧栏宽」——底部这一条以前一直在画战斗画面。
    const int bottomStripLeak = surface.countRgb(0, 568, 632, 600, kSentinel);
    const int wholeClientLeak = surface.countRgb(0, 0, 800, 600, kSentinel);
    std::string bmpError;
    const std::string bmpPath = runtimeSmokeArtifactPath("ingame_menu_800x600.bmp");
    (void)writeRuntimeSmokeBmp(bmpPath, surface.pixels, surface.width, surface.height, bmpError);

    output << "Menu modal pause: sfxChannelsStopped=" << formatBool(sfxStopped)
           << " liveSfxChannels=" << liveSfxChannels
           << " evaStopped=" << formatBool(evaStopped)
           << " bottomStripLeak=" << bottomStripLeak
           << " wholeClientLeak=" << wholeClientLeak
           << "/" << (800 * 600)
           << " frame=" << bmpPath << "\n";
    if (!sfxStopped) {
        return fail("battle sfx still live after menu open: channels="
            + std::to_string(liveSfxChannels));
    }
    if (!evaStopped) {
        return fail("EVA voice channel still live after menu open");
    }
    if (bottomStripLeak != 0) {
        return fail("bottom 32px strip still shows battle pixels: "
            + std::to_string(bottomStripLeak));
    }
    if (wholeClientLeak != 0) {
        return fail("menu frame leaves battle pixels visible: "
            + std::to_string(wholeClientLeak));
    }

    // ④ 关掉菜单不该把音频簿记重新点亮（音效由模拟在下一拍自然重新发起）。
    setRuntimeMenuOpen(state, false);
    if (state.runtimeSfxMciChannelActive[0] || state.runtimeEvaMciChannelActive) {
        return fail("closing the menu re-armed audio channels");
    }
    output << "Menu modal pause: closeRestoresNothing=yes\n";
    return true;
}
