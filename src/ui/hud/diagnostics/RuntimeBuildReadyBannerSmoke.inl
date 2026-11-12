// 就绪角标的本机像素验收：形状（上直角 / 下圆角）、水平居中、贴顶、
// 逐帧下滑、到期清理。
//
// 判据全部从"自己画出来的 DIB"里反推包围盒，不拿生产路径的 layout 函数
// 算期望值——否则 layout 算错时冒烟会跟着一起错。圆角那几条是按
// "角上应当露出背景"这一语义断言的。
bool runRuntimeBuildReadyBannerSmoke(
    const RuntimeConfig& config,
    std::string& error
) {
    std::cout << std::unitbuf;
    const auto fail = [&](std::string why) {
        error = "Build ready banner: " + std::move(why);
        return false;
    };
    constexpr std::uint32_t background = 0x203040U;
    constexpr std::uint32_t hemColour = 0x5CCCECU;
    constexpr std::uint64_t startMs = 10000;

    struct Canvas {
        RuntimePlayerDefeatNoticeSmokeSurface surface;
        int width = 0;
        int height = 0;
        Canvas(int w, int h)
            : surface(w, h), width(w), height(h) {}
    };

    const auto paint = [&](Canvas& canvas, RuntimeWindowState& state,
                           std::uint64_t nowMs) {
        std::fill(
            canvas.surface.pixels,
            canvas.surface.pixels
                + static_cast<std::size_t>(canvas.width) * canvas.height,
            background
        );
        const RECT client{0, 0, canvas.width, canvas.height};
        drawRuntimeBuildReadyBannerAt(canvas.surface.dc, client, state, nowMs);
        GdiFlush();
        return std::vector<std::uint32_t>(
            canvas.surface.pixels,
            canvas.surface.pixels
                + static_cast<std::size_t>(canvas.width) * canvas.height
        );
    };
    struct InkBounds {
        int left = 0, top = 0, right = 0, bottom = 0;
        std::size_t count = 0;
        bool valid() const { return count > 0; }
    };
    const auto inkBounds = [](const std::vector<std::uint32_t>& pixels,
                              int width, int height) {
        InkBounds bounds;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::uint32_t value =
                    pixels[static_cast<std::size_t>(y) * width + x] & 0xffffffU;
                if (value == background) continue;
                if (bounds.count == 0) {
                    bounds.left = bounds.right = x;
                    bounds.top = bounds.bottom = y;
                } else {
                    bounds.left = std::min(bounds.left, x);
                    bounds.right = std::max(bounds.right, x);
                    bounds.top = std::min(bounds.top, y);
                    bounds.bottom = std::max(bounds.bottom, y);
                }
                ++bounds.count;
            }
        }
        return bounds;
    };
    const auto pixelAt = [](const std::vector<std::uint32_t>& pixels, int width,
                            int x, int y) {
        return pixels[static_cast<std::size_t>(y) * width + x] & 0xffffffU;
    };

    RuntimeWindowState state;
    state.config = config;
    Canvas canvas(900, 600);
    if (!canvas.surface.valid()) return fail("real DIB surface unavailable");
    const RECT client{0, 0, canvas.width, canvas.height};
    const RECT viewport = runtimeMapViewport(client);
    const int viewportWidth = static_cast<int>(viewport.right - viewport.left);
    if (viewportWidth < 240 || viewport.bottom - viewport.top < 200) {
        return fail("map viewport too small for the fixture");
    }

    // 1) 未激活：一个像素都不许变。
    if (inkBounds(paint(canvas, state, 5000), canvas.width, canvas.height).valid()) {
        return fail("inactive banner painted pixels");
    }

    state.buildReadyBanner.start(startMs, "GAPIH");
    // 2) 起始帧：还完全藏在视口上方。
    if (inkBounds(paint(canvas, state, startMs), canvas.width, canvas.height).valid()) {
        return fail("first slide frame was already visible");
    }

    // 3) 下滑中途：顶边已进入视口，底边还没落到位。
    {
        const auto pixels = paint(canvas, state, startMs + 40);
        const InkBounds ink = inkBounds(pixels, canvas.width, canvas.height);
        if (!ink.valid()) return fail("mid-slide banner vanished");
        if (ink.top != static_cast<int>(viewport.top)) {
            return fail("mid-slide banner did not start at the viewport top");
        }
        if (ink.bottom - ink.top >= runtimeBuildReadyBannerHeight - 1) {
            return fail("mid-slide banner was already fully dropped");
        }
    }

    // 4) 停留段：形状与位置的全部硬判据（包围盒由像素反推）。
    const std::uint64_t holdMs =
        startMs + runtimeBuildReadyBannerSlideInMs + 400;
    const auto holdPixels = paint(canvas, state, holdMs);
    const InkBounds box = inkBounds(holdPixels, canvas.width, canvas.height);
    if (!box.valid()) return fail("held banner painted nothing");
    if (box.top != static_cast<int>(viewport.top)) {
        return fail("banner top edge is not flush with the viewport top");
    }
    if (box.bottom - box.top + 1 != runtimeBuildReadyBannerHeight) {
        return fail(
            "banner height is " + std::to_string(box.bottom - box.top + 1)
            + ", expected " + std::to_string(runtimeBuildReadyBannerHeight)
        );
    }
    if (box.left < viewport.left || box.right >= viewport.right) {
        return fail("banner left the map viewport horizontally");
    }
    const int centreDelta =
        (box.left + box.right) - static_cast<int>(viewport.left + viewport.right);
    if (centreDelta < -2 || centreDelta > 2) {
        return fail(
            "banner is not horizontally centred, delta="
            + std::to_string(centreDelta)
        );
    }
    // 上两角必须是直角：最上一行的两端都要有底色。
    if (pixelAt(holdPixels, canvas.width, box.left, box.top) == background
        || pixelAt(holdPixels, canvas.width, box.right, box.top) == background) {
        return fail("banner top corners were rounded");
    }
    // 下两角必须让出背景：圆角生效。
    if (pixelAt(holdPixels, canvas.width, box.left, box.bottom) != background
        || pixelAt(holdPixels, canvas.width, box.right, box.bottom)
            != background) {
        return fail("banner bottom corners were square");
    }
    // 底边中段必须填满，否则不是"收窄"而是整行没了。
    const int centreX = (box.left + box.right) / 2;
    if (pixelAt(holdPixels, canvas.width, centreX, box.bottom) == background) {
        return fail("banner bottom edge is empty at the centre");
    }
    // 圆角至少削掉 1px：贴着底边的左起第 1、2 列都要是背景。
    if (pixelAt(holdPixels, canvas.width, box.left + 1, box.bottom)
        != background) {
        return fail("bottom-left corner inset is smaller than one pixel");
    }
    // 底部高亮 hem 必须存在，并且在底边中段连续。
    int hemPixels = 0;
    for (int x = box.left; x <= box.right; ++x) {
        if (pixelAt(holdPixels, canvas.width, x, box.bottom) == hemColour) {
            ++hemPixels;
        }
    }
    if (hemPixels < 20) {
        return fail("bottom hem missing, pixels=" + std::to_string(hemPixels));
    }
    // 文字墨迹：白字必须真的画上去，并且左右居中。
    int brightPixels = 0;
    int inkLeft = -1;
    int inkRight = -1;
    int inkTop = -1;
    int inkBottom = -1;
    for (int y = box.top; y <= box.bottom; ++y) {
        for (int x = box.left; x <= box.right; ++x) {
            const std::uint32_t value = pixelAt(holdPixels, canvas.width, x, y);
            if ((value >> 16 & 0xff) < 200 || (value >> 8 & 0xff) < 200
                || (value & 0xff) < 200) {
                continue;
            }
            ++brightPixels;
            if (inkLeft < 0 || x < inkLeft) inkLeft = x;
            if (inkRight < 0 || x > inkRight) inkRight = x;
            if (inkTop < 0 || y < inkTop) inkTop = y;
            if (inkBottom < 0 || y > inkBottom) inkBottom = y;
        }
    }
    if (brightPixels < 20) {
        return fail(
            "ready text ink missing, bright pixels="
            + std::to_string(brightPixels)
        );
    }
    if (inkLeft - box.left < 8 || box.right - inkRight < 8) {
        return fail("ready text is not horizontally centred in the banner");
    }
    if (inkTop - box.top < 3 || box.bottom - inkBottom < 3) {
        return fail("ready text is not vertically centred in the banner");
    }

    // 5) 收尾：滑出之后完全消失，并且状态自己清干净。
    const std::uint64_t expiredMs = startMs + runtimeBuildReadyBannerTotalMs();
    if (inkBounds(paint(canvas, state, expiredMs), canvas.width, canvas.height)
        .valid()) {
        return fail("expired banner still painted");
    }
    if (state.buildReadyBanner.active) return fail("expired banner stayed active");

    // 6) 窄视口：按视口收窄，仍然居中，且不会压到视口外。
    {
        RuntimeWindowState narrowState;
        narrowState.config = config;
        Canvas narrowCanvas(240, 400);
        if (narrowCanvas.surface.valid()) {
            const RECT narrowClient{0, 0, narrowCanvas.width,
                narrowCanvas.height};
            const RECT narrowViewport = runtimeMapViewport(narrowClient);
            narrowState.buildReadyBanner.start(startMs, "GAPIH");
            const auto pixels = paint(narrowCanvas, narrowState,
                startMs + runtimeBuildReadyBannerSlideInMs + 100);
            const InkBounds narrow =
                inkBounds(pixels, narrowCanvas.width, narrowCanvas.height);
            if (narrow.valid()) {
                if (narrow.left < narrowViewport.left
                    || narrow.right >= narrowViewport.right) {
                    return fail("narrow viewport banner overflowed sideways");
                }
                const int narrowDelta = (narrow.left + narrow.right)
                    - static_cast<int>(
                        narrowViewport.left + narrowViewport.right);
                if (narrowDelta < -2 || narrowDelta > 2) {
                    return fail("narrow viewport banner is not centred");
                }
            }
        }
    }

    // 7) 证据位图：停留段与下滑段各一张。
    const char* outputEnv = std::getenv("CONCORD_BUILD_READY_BANNER_OUTPUT");
    const auto output = outputEnv && *outputEnv
        ? std::filesystem::path(outputEnv)
        : std::filesystem::path(config.localDataRootPath).parent_path()
            / "build/smoke/build-ready-banner";
    std::error_code directoryError;
    std::filesystem::create_directories(output, directoryError);
    if (directoryError) {
        return fail("output directory: " + directoryError.message());
    }
    if (!writeRuntimeSmokeBmp(
            pathString(output / "ready-banner-hold.bmp"),
            holdPixels.data(),
            canvas.width,
            canvas.height,
            error
        )) {
        return fail("hold BMP: " + error);
    }
    {
        state.buildReadyBanner.start(startMs, "GAPIH");
        const auto slidePixels = paint(canvas, state, startMs + 40);
        if (!writeRuntimeSmokeBmp(
                pathString(output / "ready-banner-slide.bmp"),
                slidePixels.data(),
                canvas.width,
                canvas.height,
                error
            )) {
            return fail("slide BMP: " + error);
        }
    }
    std::cout
        << "Build ready banner smoke passed: viewport=" << viewportWidth
        << "x" << (viewport.bottom - viewport.top)
        << " banner=" << (box.right - box.left + 1) << "x"
        << (box.bottom - box.top + 1)
        << " centred=yes topFlush=yes topCorners=square bottomCorners=rounded"
        << " textPixels=" << brightPixels << " hemPixels=" << hemPixels
        << " slideIn=" << runtimeBuildReadyBannerSlideInMs
        << "ms hold=" << runtimeBuildReadyBannerHoldMs
        << "ms slideOut=" << runtimeBuildReadyBannerSlideOutMs
        << "ms output=" << pathString(output) << " PASS\n";
    return true;
}
