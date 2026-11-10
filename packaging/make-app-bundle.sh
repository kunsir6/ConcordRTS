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

# ④ 图标：从单张 256px 源图生成 iconset，再交给 iconutil。
#    仓库里只存一张 120 KB 的 PNG，而不是 2 MB 的 icns：icns 是构建产物。
if [ -f "$ICON_SOURCE" ]; then
    # 目录名必须以 .iconset 结尾，且**不能**是隐藏文件（".iconset" 这种名字
    # iconutil 会当成"没有扩展名"，报 "The file extension must equal icns"）。
    ICONSET="$OUTPUT/concord-icon.iconset"
    mkdir -p "$ICONSET"
    for size in 16 32 64 128 256 512 1024; do
        sips -z "$size" "$size" "$ICON_SOURCE" --out "$ICONSET/icon_${size}.png" >/dev/null
    done
    cp "$ICONSET/icon_16.png"   "$ICONSET/icon_16x16.png"
    cp "$ICONSET/icon_32.png"   "$ICONSET/icon_16x16@2x.png"
    cp "$ICONSET/icon_32.png"   "$ICONSET/icon_32x32.png"
    cp "$ICONSET/icon_64.png"   "$ICONSET/icon_32x32@2x.png"
    cp "$ICONSET/icon_128.png"  "$ICONSET/icon_128x128.png"
    cp "$ICONSET/icon_256.png"  "$ICONSET/icon_128x128@2x.png"
    cp "$ICONSET/icon_256.png"  "$ICONSET/icon_256x256.png"
    cp "$ICONSET/icon_512.png"  "$ICONSET/icon_256x256@2x.png"
    cp "$ICONSET/icon_512.png"  "$ICONSET/icon_512x512.png"
    cp "$ICONSET/icon_1024.png" "$ICONSET/icon_512x512@2x.png"
    # iconutil 的 -o 也要求扩展名真的是 .icns（写 ".tmp" 会被拒），
    # 所以先在 build 目录产出 .icns，再 mv 进 Resources。
    iconutil -c icns "$ICONSET" -o "$OUTPUT/$APP_NAME.icns"
    mv "$OUTPUT/$APP_NAME.icns" "$RESOURCES_DIR/$APP_NAME.icns"
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
