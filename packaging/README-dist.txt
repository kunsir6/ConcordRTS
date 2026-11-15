尤里红警 · macOS 通用产物（Apple Silicon + Intel）
================================================

    dist/尤里红警.app                      ← 双击即玩（universal，ad-hoc 签名，macOS 12.0+）

一个包里同时有 arm64 与 x86_64 两个切片：Apple Silicon 上原生跑，Intel 上原生跑，
不需要选。为什么不是两份 thin 包：判据（金标准哈希、像素基线）按构建目录组织，
两种架构各编一次、最后 lipo 合成，源码只有一份（见 PKG-01）。

**分发包（dmg）不常驻 `dist/`**：只有本轮真改了产品代码才用
`bash ra2mac_docs/scripts/make-dmg.sh` 现出一份，名字带当时的 hash。
`dist/` 里没有 dmg = 本轮只是文档轮，`.app` 仍是最新，直接双击那个就行。
（R59 起：`dist/` 里曾长期躺着一份挂 `b4672b5` 的旧 dmg，而旁边的 .app 已到 `96ed2ee`
——它的身份自证是真的，所以「包是哪一版」查得出来，「该用哪个」反而更迷惑。）

这个包是哪一版？
--------------
包内自带身份，不必回头看构建机上的文件：

    ./尤里红警.app/Contents/MacOS/concord_runtime --version

    ConcordRTS runtime 0.1.0
    arch: arm64                                   ← 当前实际跑的切片
    compiled: Nov 15 2026 19:25:24                ← 这半边的编译时刻
    package: <hash> 0.1.0 <打包时刻> arch=x86_64+arm64
             └ git 短 hash   └ 打包时刻      └ 包含的切片

`package:` 行来自包内 `Contents/Resources/BUILD-INFO.txt`，打包脚本会断言
「二进制自己报的 package 行 == 包内文件」——所以它能证明包里的二进制与标签是同一版，
而不是「标签换了、里面没换」。

数据目录（第一次启动最容易卡这里）
--------------------------------
游戏数据（1.6GB 的 .mix/.shp/.wav）**不在包里**。包里的
`Contents/Resources/data-root` 只是一个**文本指针**，写着打包时选定的数据目录：

    /Users/zhenglk/AiProjects/ra2mac/ConcordRTS-GameData

搜索顺序（`RuntimeConfigPaths.cpp` 的 ⓪①②③）：包内指针 → `CONCORD_DATA_ROOT`
环境变量 → 可执行文件自身/当前目录 → **兄弟目录**。
注意第一条：包内指针能解析成功时，环境变量不会再生效（R55 实测；该处代码注释
曾与代码相反，`b4672b5` 已改）。

所以把包发给别人时，三选一：

    1. 把数据树放成 .app 的**同级目录**（最省事，不用改包、不用重签）。
       目录名不限——兄弟目录是按「同时含 local_data/ 与 RedAlert2/」判定的，
       叫 ConcordRTS-GameData 或 GameData 都行
    2. 让对方 export CONCORD_DATA_ROOT=/path/to/ConcordRTS-GameData
       **并且**让包内指针指向的路径不存在（否则指针优先，环境变量被忽略）
    3. 改包内 data-root 指针文件——改完必须重签：codesign --force -s - <app>

**当前缺口（EVO-07 ③ 未做）**：找不到数据根时**没有对话框**，只在终端打印一行诊断；
双击启动的人看不到。这一条已登记在案，没有假装已修。

全屏
----
绿灯是系统全屏（Ctrl+Cmd+F 同效）。全屏的语义是**视口变大**：单位、建筑、侧边栏的
像素大小不变，一屏能容下更多地图——不是把 800×600 拉伸放大（见 U-14）。
窗口尺寸变化时客户区尺寸会交给游戏，由游戏按新尺寸重绘，鼠标坐标按表面/view 比例反算。
仍不做 Retina 原生分辨率重绘（EVO-14 未做），所以超大屏上单位仍是原来的点阵大小。

鼠标与指针
----------
· 指针在游戏窗口内时系统箭头隐藏（只留游戏自绘的那个），移出窗口立刻恢复；
· 全屏时窗口就是屏幕，指针天然出不去，边缘滚动和 Windows 上一样；
· 窗口模式下想「把指针关在游戏里」（VMware 那种手感）：

      Ctrl + Cmd + G   抓住指针（硬件指针冻结，游戏光标随鼠标走）
      Ctrl + Cmd       放开指针（硬件指针接回游戏光标处，可以移出窗口）
      鼠标点回窗口      自动再抓住

  默认不抓——窗口模式常用来边玩边切应用，一进游戏就抓住容易让人以为鼠标坏了。
  万一捕获中进程异常退出，关联会自动恢复（atexit 兜底），鼠标不会真的卡死。

一个应用，两个入口
------------------
《红色警戒 2》与《尤里的复仇》是同一个应用里的两个入口，不是两个 app：

    · 战役选择器按版本分组，名字后面带「（红色警戒 2）」/「（尤里的复仇）」后缀；
    · 遭遇战/地图列表共用同一套数据树，按所选内容区分。

外层只有一个 app 名（尤里红警），内层靠战役/内容入口区分——
与零售版两个快捷方式（ra2.exe / ra2md.exe）是两种做法，这里是单一进程内的入口。

首次启动（Gatekeeper）
----------------------
ad-hoc 签名，Gatekeeper 会拦一次：右键 → 打开，或 系统设置 → 隐私与安全性 → 仍要打开。
如果提示"已损坏"，执行：

    xattr -dr com.apple.quarantine <app 路径>
    codesign --verify --deep --strict <app 路径>

从 dmg 拷贝时请用 Finder 拖拽（内部走 ditto，保留签名 seal）；用 `cp -R` 手工拷贝
在某些情况下会丢扩展属性，验签会失败。

重新打包
--------
不要手工往 .app 里 cp 二进制——身份（名字 / 图标 / LaunchServices 登记）是一次整体动作。
完整链路（顺序不能变，漏掉第 2 步会让 arm64 那半边停在旧代码上）：

    ninja -C ConcordRTS/build-product-arm
    ninja -C ConcordRTS/build-product-x86_64
    bash ra2mac_docs/scripts/lipo-universal.sh      # 备份 + lipo + 重签 + 逐切片 minos 校验
    bash ra2mac_docs/scripts/pack-dist.sh --bin ConcordRTS/build-universal/concord_runtime
    bash ra2mac_docs/scripts/make-dmg.sh            # 需要分发包时才跑，挂载后逐条验

pack-dist 会：归档并注销旧包 → 组装（圆角图标 + plist + data-root + 签名 + BUILD-INFO）→
向 IconServices 求证图标真的被采用 → 校验两切片 minos 与 plist 一致 → 跑 `--smoke-test`
→ 断言 `--version` 的 package 行与 BUILD-INFO 一致 → 覆盖写本说明书与 BUILD-STAMP。
验证截图落在 dist/.verify/icon.png。

make-dmg 会：ditto 进临时卷（保 seal）→ hdiutil 压缩 → verify CRC → 挂载 →
在挂载卷里重验 FAT/两切片 minos/验签/版本身份/`--smoke-test`，全部通过才算成功。

换图标：把新图放到 ConcordRTS/packaging/icon-source.png（建议 1024×1024），
或给 pack-dist.sh 传 --icon-source。图不需要自己切圆角——make-icon.py 会按
macOS 的 824/1024 网格套圆角、加透明边距与阴影。源图自带 alpha 时可用
make-icon.py --no-mask 跳过套壳。

回归现状与已知缺口
------------------
`bash ra2mac_docs/scripts/regress.sh` 共 **15 条：13 绿 / 2 known-red / 0 新红**（约 35 秒）。
两个数字都会随树变，以报告头的 `-- 汇总 --` 行为准，别拿这份说明书当门禁。

· known-red ①：command 链尾「奴隶矿工满载不交付」（`digging=1 carrying=0 cargo=4 credits=0`）。
  也就是说**尤里的奴隶矿工目前不产生收入**——真实缺口，不假装已修（C-02 主线判 blocked）。
· known-red ②：`vehiclepass` —— 载具被友军步兵堵在终点前过不去（M-25，用户实测确认）。
  判据 `RuntimeVehicleInfantryPassThroughSmoke.inl` 自第 58 轮起**已在 HEAD 且已是第 15 条门禁**
  （`782e194` + `96ed2ee`），所以这条缺陷每轮都会被照出来、不会静默退化；
  但缺陷本身仍在（三次修复尝试均负结果，主线判 blocked）。
· 数据根缺失没有可读对话框（EVO-07 ③）、About 面板未做（EVO-07 ②）。
· 战役：尤里的复仇战役尚未接入战役数据库，只有基础版 ra2 的战役可选（EVO-13）。
· Bink 视频不做解码，过场为静帧（产品决策）。

已修的两类"看起来卡死"
----------------------
U-01 大厅不处理事件：从主菜单进遭遇战大厅后，AppKit 事件队列无人排空 →
     画面停在第一帧、点不动，点一下就是彩虹圈。
U-02 世界与加载页同样不排空：进世界后鼠标键盘进不来、彩虹圈一直挂着。

两者都修在 compat 层（MsgWaitForMultipleObjectsEx / WaitMessage / PeekMessageW），
共享代码零改动。

---
最后核对：第 59 轮（`96ed2ee`）。这份文件里**可被机器验证**的三处事实
（回归条数 / M-25 判据是否在门禁里 / 数据根搜索顺序）逐条对过；
下次改代码后请重核这三处，其余描述性段落没有廉价判据，靠人读。
