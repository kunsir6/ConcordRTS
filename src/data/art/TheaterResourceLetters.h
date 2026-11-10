// Q-03：NewTheater 剧场字母表的唯一实现（平台无关、无 IO、无依赖）。
//
// 为什么必须有这一份：同一段映射曾经复制在三个 TU 里（预览导出 / 产品资源解析 /
// 对象图候选），2026-11-10 的实测证明三份会各自漂移——Q-02 只修了预览那一份，
// 产品侧还留着 `.tem -> 't'`。从本轮起，三处都调这里，表只有一张。
//
// 关键事实（Q-04 用真实数据修正过一次，别再改回去）：
// **温带字符取决于建筑家族，不取决于用途**。
//   - 玩家建筑 GATECH（G/N/Y 前缀）温带用通用图 art/base/ggtech.shp，`gttech.shp` 不存在；
//   - 民用建筑 CAHOSP（C 前缀）温带用剧场图 art/temperat/cthosp.shp，`cghosp.shp` 不存在。
// 769 个 NewTheater=yes 段的交叉统计：两种变体都有 23 / 只有 g 120 / 只有 t 213 / 都没有 413。
// 所以**一个字母服务不了任何一边**，唯一正确的形状是序列：
//   [剧场字母, 通用 'G', ART 里字面写下的名字]
// 先试本剧场（民用建筑在这里命中），再退通用图（玩家建筑在这里命中），最后退回字面名。
// 首候选命中率：`[t, g]` = 242/769；只留 `[g]` = 149/769，且 213 个民用建筑会退到雪地图。
//
// 字母表的硬证据（按归档归属统计：art/<group> 是按源 mix 分的组）：
//   snow.mix -> A (492)   temperat.mix -> T (217)   urban.mix -> U (223)
//   urbann.mix -> N (262) desert.mix -> D (213)     lunar.mix -> L (209)
//   ra2md.mix -> G (170)  = 通用默认图，任何剧场找不到自己的变体都退到它
// 复算见 RUNLOG 2026-11-11 00:16 与 Q-02-剧场字符说明.md。
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace concord::data::art {

namespace theater_letters_detail {

struct TheaterLetterRow {
    const char* extension;
    char structureLetter;  // 建筑 SHP 用的剧场字母（温带 = T）
    char overlayLetter;    // 地表 overlay 用的剧场字母（目前与建筑一致）
    const char* theaterName;
};

// 顺序即"首选优先"。两张字母表只在温带上不同（G vs T），其余一致。
inline constexpr TheaterLetterRow kTheaterLetterTable[] = {
    {".tem", 'T', 'T', "temperate"},  // Q-04：温带剧场字母是 T；GG* 是通用兜底，不是温带字母
    {".sno", 'A', 'A', "snow"},
    {".urb", 'U', 'U', "urban"},
    {".ubn", 'N', 'N', "new urban"},
    {".des", 'D', 'D', "desert"},
    {".lun", 'L', 'L', "lunar"},
};

// 通用图字母：建筑在任何剧场找不到自己的变体时退到 gg*。
inline constexpr char kGenericTheaterLetter = 'G';

// 只有这几类前缀会被 NewTheater 换掉第二个字母：G 建筑、N 中子、C 民用、Y 尤里。
// 其它前缀（E1 / HTNK / AB* …）是单位名，剧场不参与。
inline bool isNewTheaterFirstLetter(char value) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    return upper == 'G' || upper == 'N' || upper == 'C' || upper == 'Y';
}

// 第二字母的合法取值：6 个剧场字母 + 通用 G。
// 把 G 算进来是 Q-02 的修正：`GGPOWR` / `NGYARD` 这类 art id 自己就写着通用形式，
// 旧门禁把它们判成"不支持新剧场"，于是雪地/沙漠剧场永远拿不到 GA*/CA*。
inline bool isKnownTheaterLetter(char value) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    if (upper == kGenericTheaterLetter) {
        return true;
    }
    for (const TheaterLetterRow& row : kTheaterLetterTable) {
        if (row.structureLetter == upper || row.overlayLetter == upper) {
            return true;
        }
    }
    return false;
}

inline std::string normalizeTileExtension(const std::string& tileExtension) {
    std::string value;
    value.reserve(tileExtension.size() + 1);
    for (const char raw : tileExtension) {
        if (raw == ' ' || raw == '\t') {
            continue;
        }
        value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw))));
    }
    if (!value.empty() && value.front() != '.') {
        value.insert(value.begin(), '.');
    }
    return value;
}

inline const TheaterLetterRow* findTheaterLetterRow(const std::string& tileExtension) {
    const std::string extension = normalizeTileExtension(tileExtension);
    if (extension.empty()) {
        return nullptr;
    }
    for (const TheaterLetterRow& row : kTheaterLetterTable) {
        if (extension == row.extension) {
            return &row;
        }
    }
    return nullptr;
}

inline std::string toLowerAsciiCopy(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char raw : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw))));
    }
    return result;
}

inline std::vector<char> letterCandidatesFor(char preferred) {
    std::vector<char> letters;
    if (preferred == '\0') {
        return letters;
    }
    letters.push_back(preferred);
    if (preferred != kGenericTheaterLetter) {
        letters.push_back(kGenericTheaterLetter);
    }
    return letters;
}

}  // namespace theater_letters_detail

// 建筑 SHP 的剧场字母；未知扩展名返回 '\0'。大小写与是否带点都容忍。
inline char theaterResourceLetter(const std::string& tileExtension) {
    const theater_letters_detail::TheaterLetterRow* row =
        theater_letters_detail::findTheaterLetterRow(tileExtension);
    return row == nullptr ? '\0' : row->structureLetter;
}

// 地表 overlay 的剧场字母；只有温带与建筑不同（'T' vs 'G'）。
inline char overlayTheaterResourceLetter(const std::string& tileExtension) {
    const theater_letters_detail::TheaterLetterRow* row =
        theater_letters_detail::findTheaterLetterRow(tileExtension);
    return row == nullptr ? '\0' : row->overlayLetter;
}

// 建筑候选字母序列：首选字母在前，通用 'G' 在后。温带时两者相同，只返回一个。
inline std::vector<char> theaterResourceLetterCandidates(const std::string& tileExtension) {
    return theater_letters_detail::letterCandidatesFor(theaterResourceLetter(tileExtension));
}

// 这个名字是否属于 NewTheater 的行列（第二字母可被替换）。
// 例：GAPOWR 是，GGPOWR 是，CAMORR 是，E1 / HTNK 不是。
inline bool isTheaterVariantResourceName(const std::string& baseName) {
    if (baseName.size() < 2) {
        return false;
    }
    return theater_letters_detail::isNewTheaterFirstLetter(baseName[0])
        && theater_letters_detail::isKnownTheaterLetter(baseName[1]);
}

// 生成建筑候选名（小写、不带扩展名），顺序即优先级：
//   1. 剧场字母替换后的名字（温带就是 gg*，不再是死名 gt*）
//   2. 通用字母替换后的名字（与 1 相同，仅在非温带剧场补上）
//   3. ART 段里字面写下的原名字
// 结果已去重，调用点自己决定是否追加 ".shp"。
inline std::vector<std::string> theaterResourceNameCandidates(
    const std::string& baseName,
    const std::string& tileExtension
) {
    std::vector<std::string> candidates;
    if (baseName.empty()) {
        return candidates;
    }

    const std::string lowered = theater_letters_detail::toLowerAsciiCopy(baseName);
    const auto addUnique = [&candidates](const std::string& candidate) {
        if (candidate.empty()
            || std::find(candidates.begin(), candidates.end(), candidate)
                != candidates.end()) {
            return;
        }
        candidates.push_back(candidate);
    };

    if (isTheaterVariantResourceName(baseName) && lowered.size() >= 2) {
        for (const char letter : theaterResourceLetterCandidates(tileExtension)) {
            std::string swapped = lowered;
            swapped[1] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(letter))
            );
            addUnique(swapped);
        }
    }

    addUnique(lowered);
    return candidates;
}

}  // namespace concord::data::art
