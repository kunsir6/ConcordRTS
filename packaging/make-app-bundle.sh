#!/bin/sh
# ON-1-03: 把已链接的 concord_runtime 组装成 ConcordRTS.app。
#
# 为什么是脚本而不是 cmake 的 MACOSX_BUNDLE：
#   MACOSX_BUNDLE 会把产物搬到 ConcordRTS.app/Contents/MacOS/ 下面，而仓库里
#   十几套 smoke 验证与 RUNLOG 里的每一条命令都写着 build-product-mac/concord_runtime。
#   改产物路径等于把既有验证全打断。所以：可执行文件原地不动，这个脚本**复制**一份
#   进 bundle —— bundle 是交付形态，build 目录里的二进制仍是开发形态。
#
# 数据根策略：写指针文件 Contents/Resources/data-root（不是 symlink）。
#   理由：1.7 GB 的数据目录在仓库外，symlink 会跟着 bundle 一起被搬走并在拷贝时
#   变成悬空链接；指针文件是可读的一行字，用户能自己改，也能被 ON-1-01 的探测器
#   当成显式声明（source=app-resources）。
#
# 幂等：所有写入都走 <目标>.tmp + mv 原子替换，从不删除既有文件
# （铁律二）。重复运行只会覆盖同一个文件集合。
set -eu

usage() {
    cat <<'EOF'
usage: make-app-bundle.sh --binary <path> --output <dir> --data-root <dir>
                          [--version <semver>] [--bundle-id <id>]
                          [--app-name <name>] [--icon-source <png>]
                          [--min-macos <ver>]
EOF
    exit 2
}

BINARY=""
OUTPUT=""
DATA_ROOT=""
VERSION="0.1.0"
BUNDLE_ID="org.concord.rts"
APP_NAME="ConcordRTS"
ICON_SOURCE=""
MIN_MACOS="11.0"

while [ $# -gt 0 ]; do
    case "$1" in
        --binary)      BINARY="$2"; shift 2 ;;
        --output)      OUTPUT="$2"; shift 2 ;;
        --data-root)   DATA_ROOT="$2"; shift 2 ;;
        --version)     VERSION="$2"; shift 2 ;;
        --bundle-id)   BUNDLE_ID="$2"; shift 2 ;;
        --app-name)    APP_NAME="$2"; shift 2 ;;
        --icon-source) ICON_SOURCE="$2"; shift 2 ;;
        --min-macos)   MIN_MACOS="$2"; shift 2 ;;
        -h|--help)     usage ;;
        *) echo "make-app-bundle: unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "$BINARY" ] || { echo "make-app-bundle: --binary is required" >&2; exit 2; }
[ -n "$OUTPUT" ] || { echo "make-app-bundle: --output is required" >&2; exit 2; }
[ -f "$BINARY" ] || { echo "make-app-bundle: binary not found: $BINARY" >&2; exit 2; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
[ -n "$ICON_SOURCE" ] || ICON_SOURCE="$SCRIPT_DIR/icon-source.png"

APP="$OUTPUT/$APP_NAME.app"
CONTENTS="$APP/Contents"
MACOS_DIR="$CONTENTS/MacOS"
RESOURCES_DIR="$CONTENTS/Resources"

mkdir -p "$MACOS_DIR" "$RESOURCES_DIR"

# ① 主程序：cp 到 .tmp 再 mv，避免复制到一半的二进制被 codesign 或用户读到。
cp "$BINARY" "$MACOS_DIR/concord_runtime.tmp"
chmod 755 "$MACOS_DIR/concord_runtime.tmp"
mv "$MACOS_DIR/concord_runtime.tmp" "$MACOS_DIR/concord_runtime"

# ② PkgInfo：老 Finder 用它判断"这是应用"，两行字，成本为零。
printf 'APPL????' > "$CONTENTS/PkgInfo.tmp"
mv "$CONTENTS/PkgInfo.tmp" "$CONTENTS/PkgInfo"

# ③ Info.plist：占位符替换。分隔符用 |，因为值里会有 /（bundle id、路径）。
sed -e "s|@CONCORD_APP_NAME@|$APP_NAME|g" \
    -e "s|@CONCORD_BUNDLE_ID@|$BUNDLE_ID|g" \
    -e "s|@CONCORD_APP_VERSION@|$VERSION|g" \
    -e "s|@CONCORD_MIN_MACOS@|$MIN_MACOS|g" \
    "$SCRIPT_DIR/Info.plist.in" > "$CONTENTS/Info.plist.tmp"
mv "$CONTENTS/Info.plist.tmp" "$CONTENTS/Info.plist"

# ④ 图标：交给 make-icon.py 生成（1024 画布 + Big Sur 圆角 + 透明边距），再 iconutil 出 icns。
#    这里**不能**只做「sips 缩 10 个尺寸 + iconutil」：
#      · 源图通常是纯 RGB 方块（游戏封面），没有 alpha、也不是苹果的圆角形体；
#      · 这样出来的 icns 系统不报错也不理它，Finder/Dock 显示通用空白图标；
#      · 实测纯 10 帧的 iconset 会被 iconutil 直接判 "Invalid Iconset"。
#    为什么不是"在 icns 里存 alpha 就够了"：Dock 里系统图标都按 824/1024 网格排，
#    满幅方图在旁边一眼就偏大偏方，所以形体和留白一起做。
if [ -f "$ICON_SOURCE" ]; then
    # 中间产物（10 帧 iconset）放临时目录：它只是 iconutil 的输入，
    # 留在 build/ 或 dist/ 里只会变成"这个目录里哪些文件是产物"的噪声。
    ICON_WORK="$(mktemp -d "${TMPDIR:-/tmp}/concord-icon.XXXXXX")"
    python3 "$SCRIPT_DIR/make-icon.py" \
        --source "$ICON_SOURCE" \
        --out-dir "$ICON_WORK" \
        --name "$APP_NAME" \
        --canvas 1024 --content 824 --radius 185
    mv "$ICON_WORK/$APP_NAME.icns" "$RESOURCES_DIR/$APP_NAME.icns"
    rm -rf "$ICON_WORK"
else
    echo "make-app-bundle: warning: icon source not found: $ICON_SOURCE" >&2
fi

# ⑤ 数据根指针。空值不写：没有 data-root 时探测器会退回兄弟目录/祖先扫描，
#    写一个空文件反而会误导人以为"打包时指定过数据根"。
if [ -n "$DATA_ROOT" ]; then
    printf '# ON-1-03: path to the game data tree (must contain local_data/ and RedAlert2/).\n%s\n' \
        "$DATA_ROOT" > "$RESOURCES_DIR/data-root.tmp"
    mv "$RESOURCES_DIR/data-root.tmp" "$RESOURCES_DIR/data-root"
else
    echo "make-app-bundle: warning: --data-root empty; bundle will fall back to sibling scan" >&2
fi

# ⑥ ad-hoc 签名。--force 覆盖上一次的签名；--deep 连内层可执行文件一起签。
#    没有签名时 Gatekeeper 对本机构建也会拦（"无法验证开发者"），ad-hoc 身份
#    足以让本机双击放行——它不是分发签名，别指望它在别人机器上免提示。
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || {
    echo "make-app-bundle: codesign failed" >&2
    exit 1
}

echo "bundle:    $APP"
echo "binary:    $(/usr/bin/stat -f %z "$MACOS_DIR/concord_runtime") bytes"
echo "data-root: ${DATA_ROOT:-(unset)}"
echo "signature: $(codesign -dv "$APP" 2>&1 | sed -n 's/^Signature=//p')"
