#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// S-20：对战两条 wire 路径共用一份大端编解码器。
//
// 背景：`snapshots/RuntimeMultiplayerSnapshotTransfer.cpp` 与
// `protocol/RuntimeMultiplayerMatchSetup.cpp` 各自在匿名命名空间里定义了一套
// ByteWriter / ByteReader。第 36 轮逐行 diff 的结论是**分叉而不是复制品**：
//   · 错误文案前缀不同（"snapshot " / "match setup "）
//   · `require()` 只有一侧带下溢守卫（`offset_ <= size && count <= size - offset_`）
//   · 方法集不同（一侧有 writeBool/readBool，另一侧有 writeByteVector/readArray）
// 两条路径对敲同一批字节流，任一侧改了编码另一侧不认——这一类缺陷不会有编译错误，
// 只会在联机时以「对面拒收」的形态出现。
//
// 合并口径：**保留两侧全部方法**（超集），错误文案前缀改由构造参数 `label` 提供，
// 于是每条既有错误字符串逐字节不变（`"snapshot chunk payload is truncated"` 等），
// 既有断言文案不需要改一个字。`require()` 统一取带守卫的那一版。
namespace concord::network::detail {

class BigEndianWriter {
public:
    void writeU8(std::uint8_t value) {
        bytes_.push_back(value);
    }

    void writeU16(std::uint16_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8U));
        bytes_.push_back(static_cast<std::uint8_t>(value));
    }

    void writeU32(std::uint32_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value >> 24U));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 16U));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8U));
        bytes_.push_back(static_cast<std::uint8_t>(value));
    }

    void writeU64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::uint8_t>(
                value >> static_cast<unsigned int>(shift)
            ));
        }
    }

    void writeBool(bool value) {
        writeU8(value ? 1U : 0U);
    }

    void writeString(const std::string& value) {
        writeU16(static_cast<std::uint16_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void writeByteVector(const std::vector<std::uint8_t>& value) {
        writeU32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    template <std::size_t Size>
    void writeArray(const std::array<std::uint8_t, Size>& value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    const std::vector<std::uint8_t>& bytes() const {
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

class BigEndianReader {
public:
    // label 是错误文案前缀，原样拼在「字段名 / payload」之前。
    // snapshot 侧传 "snapshot "、match setup 侧传 "match setup "。
    BigEndianReader(std::span<const std::uint8_t> bytes, const char* label)
        : bytes_(bytes), label_(label == nullptr ? "" : label) {}

    bool readU8(std::uint8_t& value) {
        if (!require(1U)) return false;
        value = bytes_[offset_++];
        return true;
    }

    bool readU16(std::uint16_t& value) {
        if (!require(2U)) return false;
        value = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes_[offset_]) << 8U)
            | static_cast<std::uint16_t>(bytes_[offset_ + 1U])
        );
        offset_ += 2U;
        return true;
    }

    bool readU32(std::uint32_t& value) {
        if (!require(4U)) return false;
        value =
            (static_cast<std::uint32_t>(bytes_[offset_]) << 24U)
            | (static_cast<std::uint32_t>(bytes_[offset_ + 1U]) << 16U)
            | (static_cast<std::uint32_t>(bytes_[offset_ + 2U]) << 8U)
            | static_cast<std::uint32_t>(bytes_[offset_ + 3U]);
        offset_ += 4U;
        return true;
    }

    bool readU64(std::uint64_t& value) {
        if (!require(8U)) return false;
        value = 0U;
        for (std::size_t index = 0; index < 8U; ++index) {
            value = (value << 8U)
                | static_cast<std::uint64_t>(bytes_[offset_ + index]);
        }
        offset_ += 8U;
        return true;
    }

    bool readBool(bool& value, const char* fieldName) {
        std::uint8_t encoded = 0U;
        if (!readU8(encoded)) return false;
        if (encoded > 1U) {
            return fail(std::string(label_) + fieldName
                        + " boolean is invalid");
        }
        value = encoded != 0U;
        return true;
    }

    bool readString(
        std::size_t maximumBytes,
        std::string& value,
        const char* fieldName
    ) {
        std::uint16_t length = 0U;
        if (!readU16(length)) return false;
        if (static_cast<std::size_t>(length) > maximumBytes) {
            return fail(std::string(label_) + fieldName
                        + " exceeds the size limit");
        }
        if (!require(static_cast<std::size_t>(length))) return false;
        value.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_),
            static_cast<std::size_t>(length)
        );
        offset_ += static_cast<std::size_t>(length);
        return true;
    }

    bool readByteVector(
        std::size_t maximumBytes,
        std::vector<std::uint8_t>& value,
        const char* fieldName
    ) {
        std::uint32_t length = 0U;
        if (!readU32(length)) return false;
        if (static_cast<std::size_t>(length) > maximumBytes) {
            return fail(std::string(label_) + fieldName
                        + " exceeds the size limit");
        }
        if (!require(static_cast<std::size_t>(length))) return false;
        value.assign(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(
                offset_ + static_cast<std::size_t>(length)
            )
        );
        offset_ += static_cast<std::size_t>(length);
        return true;
    }

    template <std::size_t Size>
    bool readArray(std::array<std::uint8_t, Size>& value) {
        if (!require(Size)) return false;
        std::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            Size,
            value.begin()
        );
        offset_ += Size;
        return true;
    }

    bool finished() const {
        return offset_ == bytes_.size();
    }

    bool fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return false;
    }

    const std::string& error() const {
        return error_;
    }

private:
    // 守卫两件都要：`offset_ <= size` 挡住 offset_ 已越界时
    // `size - offset_` 的无符号下溢（原 match setup 那一版只有后半句）。
    bool require(std::size_t count) {
        if (offset_ <= bytes_.size()
            && count <= bytes_.size() - offset_) {
            return true;
        }
        return fail(std::string(label_) + "payload is truncated");
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
    std::string error_;
    std::string label_;
};

} // namespace concord::network::detail
