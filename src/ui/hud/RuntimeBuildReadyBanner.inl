// 建造/防御就绪角标：挂在视口顶边、水平居中的标签。
// 顶边与视口顶边齐平（上两角是直角），只有底部两角做圆角。
// 逐行填充而不是 RoundRect：RoundRect 会把四个角一起削掉，而这里需要
// "上直角、下圆角"，唯一稳的写法是按行收窄。
void drawRuntimeBuildReadyBannerAt(
    HDC dc,
    const RECT& clientRect,
    RuntimeWindowState& state,
    std::uint64_t nowMs
) {
    RuntimeBuildReadyBannerState& banner = state.buildReadyBanner;
    if (!banner.active) return;
    // 到期判定按时刻算，不按 reveal 算：reveal 在 startedMs 那一刻正好是 0，
    // 若拿它判"过期"会把刚点亮的角标在首帧就清掉。
    if (nowMs >= banner.startedMs
        && nowMs - banner.startedMs >= runtimeBuildReadyBannerTotalMs()) {
        banner.clear();
        return;
    }
    const double reveal = runtimeBuildReadyBannerReveal(nowMs, banner);
    if (reveal <= 0.0) return;
    const RECT viewport = runtimeMapViewport(clientRect);
    if (viewport.right <= viewport.left || viewport.bottom <= viewport.top) {
        return;
    }
    const std::wstring text = L"就绪";
    const int saved = SaveDC(dc);
    if (saved == 0) return;
    // 角标比普通提示更"标题感"一点：比 HUD 常规字号大 2px 并加粗。
    // 字体族沿用 HUD 的 Microsoft YaHei，避免和侧栏/提示条两套字形。
    const HFONT bannerFont = CreateFontW(
        -21, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Microsoft YaHei"
    );
    if (bannerFont == nullptr) {
        RestoreDC(dc, saved);
        return;
    }
    int textWidth = 0;
    if (const HGDIOBJ previous = SelectObject(dc, bannerFont)) {
        const auto extent = concord::ui::HudSystemFontWin32::measure(dc, text);
        if (extent.measured) textWidth = static_cast<int>(extent.width);
        SelectObject(dc, previous);
    }
    const RuntimeBuildReadyBannerBounds bounds = runtimeBuildReadyBannerBounds(
        static_cast<int>(viewport.left),
        static_cast<int>(viewport.top),
        static_cast<int>(viewport.right),
        static_cast<int>(viewport.bottom),
        textWidth,
        reveal
    );
    if (!bounds.valid) {
        RestoreDC(dc, saved);
        return;
    }
    IntersectClipRect(
        dc,
        viewport.left,
        viewport.top,
        viewport.right,
        viewport.bottom
    );

    const int height = bounds.height();
    // 深色渐变 + 底部一道高亮 hem，读法和侧栏的 busy/border 色系一致。
    constexpr int topRed = 14, topGreen = 33, topBlue = 44;
    constexpr int bottomRed = 24, bottomGreen = 58, bottomBlue = 78;
    constexpr int hemRed = 92, hemGreen = 204, hemBlue = 236;
    for (int row = 0; row < height; ++row) {
        const int y = bounds.top + row;
        if (y < viewport.top || y >= viewport.bottom) continue;
        int red = 0, green = 0, blue = 0;
        if (row == height - 1) {
            red = hemRed;
            green = hemGreen;
            blue = hemBlue;
        } else {
            const int mix = height > 1 ? row : 0;
            const int span = std::max(1, height - 2);
            red = topRed + (bottomRed - topRed) * mix / span;
            green = topGreen + (bottomGreen - topGreen) * mix / span;
            blue = topBlue + (bottomBlue - topBlue) * mix / span;
        }
        const int inset = runtimeBuildReadyBannerRowInset(
            height - 1 - row,
            bounds.radius
        );
        const RECT line{
            bounds.left + inset,
            y,
            bounds.right - inset,
            y + 1
        };
        if (line.right <= line.left) continue;
        HBRUSH brush = CreateSolidBrush(RGB(red, green, blue));
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }

    // 顶部内缝：和视口顶边齐平时给一道细线，避免标签"糊"在地图边缘上。
    // 下面再压一行更暗的线，做出一点倒角（bevel）的层次。
    {
        const RECT seam{bounds.left, bounds.top, bounds.right, bounds.top + 1};
        if (seam.top >= viewport.top && seam.bottom <= viewport.bottom
            && seam.right > seam.left) {
            HBRUSH brush = CreateSolidBrush(RGB(74, 142, 176));
            FillRect(dc, &seam, brush);
            DeleteObject(brush);
        }
        const RECT bevel{
            bounds.left,
            bounds.top + 1,
            bounds.right,
            bounds.top + 2
        };
        if (bevel.top >= viewport.top && bevel.bottom <= viewport.bottom
            && bevel.right > bevel.left) {
            HBRUSH brush = CreateSolidBrush(RGB(9, 22, 30));
            FillRect(dc, &bevel, brush);
            DeleteObject(brush);
        }
    }

    SelectObject(dc, bannerFont);
    SetBkMode(dc, TRANSPARENT);
    constexpr UINT flags =
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    const auto request = [&](int topOffset, COLORREF color) {
        SetTextColor(dc, color);
        const concord::ui::HudSystemWideTextRequest message{
            text.c_str(),
            -1,
            {bounds.left, bounds.top + topOffset, bounds.right,
                bounds.bottom + topOffset},
            flags
        };
        RECT textRect{
            message.rect.left,
            message.rect.top,
            message.rect.right,
            message.rect.bottom
        };
        DrawTextW(dc, message.text, message.count, &textRect, message.format);
    };
    request(1, RGB(0, 0, 0));
    request(0, RGB(236, 250, 255));
    RestoreDC(dc, saved);
    DeleteObject(bannerFont);
}

void drawRuntimeBuildReadyBanner(
    HDC dc,
    const RECT& clientRect,
    RuntimeWindowState& state
) {
    if (!state.buildReadyBanner.active) return;
    drawRuntimeBuildReadyBannerAt(dc, clientRect, state, runtimeUiClockNow(state));
}
