#pragma once

// The binary form of a tag tree, both directions in one place: a payload is a type, a name
// and a body, and every length inside it is counted by the sender, so this is the only
// layer that has to care about hostile input.

#include "log/logger.hpp"
#include "tag/budget.hpp"
#include "tag/value.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace terila::tag {

    // Where a payload keeps the root name. Files start with it; the network form and a data
    // component value start straight at the type, so the difference has to be explicit.
    enum class Layout { Unnamed, Named };

    namespace codec {

        // Names are Java's "modified" UTF-8 — 16 bit units, so a character outside the BMP
        // travels as a surrogate pair — while everything else here speaks plain UTF-8.
        inline void append_utf8(std::string& out, std::uint32_t code_point) {
            if (code_point < 0x80) {
                out.push_back(static_cast<char>(code_point));
            }
            else if (code_point < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
                out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            }
            else if (code_point < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            }
            else {
                out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            }
        }

        inline std::string to_wire_text(std::string_view utf8) {
            std::string out;
            std::size_t i = 0;
            while (i < utf8.size()) {
                const auto lead = static_cast<std::uint8_t>(utf8[i]);
                std::uint32_t code_point = 0;
                std::size_t width = 1;
                if (lead < 0x80) {
                    code_point = lead;
                }
                else if ((lead & 0xE0) == 0xC0) {
                    code_point = lead & 0x1F;
                    width = 2;
                }
                else if ((lead & 0xF0) == 0xE0) {
                    code_point = lead & 0x0F;
                    width = 3;
                }
                else if ((lead & 0xF8) == 0xF0) {
                    code_point = lead & 0x07;
                    width = 4;
                }
                else {
                    // Not UTF-8 at all: keep the byte as itself instead of failing a write
                    // that only exists to describe a name.
                    out.push_back(static_cast<char>(lead));
                    ++i;
                    continue;
                }
                if (i + width > utf8.size()) {
                    throw BudgetExceeded("truncated UTF-8 in a tag name");
                }
                for (std::size_t k = 1; k < width; ++k) {
                    code_point = (code_point << 6) |
                        (static_cast<std::uint8_t>(utf8[i + k]) & 0x3F);
                }
                i += width;

                if (code_point >= 0x10000) {
                    const auto offset = code_point - 0x10000;
                    const std::uint16_t units[2] = {
                        static_cast<std::uint16_t>(0xD800 + (offset >> 10)),
                        static_cast<std::uint16_t>(0xDC00 + (offset & 0x3FF))
                    };
                    for (const std::uint16_t unit : units) {
                        out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
                    }
                }
                else if (code_point == 0) {
                    out.push_back(static_cast<char>(0xC0));
                    out.push_back(static_cast<char>(0x80));
                }
                else {
                    append_utf8(out, code_point);
                }
            }
            return out;
        }

        inline std::string from_wire_text(std::string_view wire) {
            std::string out;
            std::size_t i = 0;
            while (i < wire.size()) {
                const auto lead = static_cast<std::uint8_t>(wire[i]);
                if (lead < 0x80) {
                    out.push_back(static_cast<char>(lead));
                    ++i;
                    continue;
                }
                const std::size_t width = (lead & 0xE0) == 0xC0 ? 2 : 3;
                if (i + width > wire.size()) {
                    throw BudgetExceeded("truncated modified UTF-8 in a tag name");
                }
                std::uint32_t code_point = lead & (width == 2 ? 0x1F : 0x0F);
                for (std::size_t k = 1; k < width; ++k) {
                    code_point = (code_point << 6) |
                        (static_cast<std::uint8_t>(wire[i + k]) & 0x3F);
                }
                i += width;

                // A high surrogate is only half a character: the low half follows.
                if (code_point >= 0xD800 && code_point <= 0xDBFF && i + 3 <= wire.size()) {
                    const auto next = static_cast<std::uint8_t>(wire[i]);
                    if ((next & 0xF0) == 0xE0) {
                        std::uint32_t low = next & 0x0F;
                        low = (low << 6) | (static_cast<std::uint8_t>(wire[i + 1]) & 0x3F);
                        low = (low << 6) | (static_cast<std::uint8_t>(wire[i + 2]) & 0x3F);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                            i += 3;
                        }
                    }
                }
                append_utf8(out, code_point);
            }
            return out;
        }

    } // namespace codec

    // ---- reading -----------------------------------------------------------------

    class TagReader {
    public:
        explicit TagReader(std::span<const std::uint8_t> data, Layout layout = Layout::Unnamed,
            ReadBudget budget = ReadBudget::from_client())
            : data_(data), layout_(layout), budget_(budget) {}

        explicit TagReader(const std::vector<std::uint8_t>& data,
            Layout layout = Layout::Unnamed, ReadBudget budget = ReadBudget::from_client())
            : TagReader(std::span<const std::uint8_t>(data), layout, budget) {}

        // The whole payload, whatever tag it holds.
        std::optional<TagValue> root() {
            const auto remember = position_;
            try {
                auto value = layout_ == Layout::Unnamed ? one_tag() : read_named_body();
                if (value.has_value()) {
                    LOG_TRACE(Data, "tag read: {} bytes, root is {}", position_,
                        tag_label(value->tag()));
                }
                return value;
            }
            catch (const std::exception& error) {
                LOG_WARN(Data, "tag read failed at byte {}: {}", remember, error.what());
                position_ = remember;
                return std::nullopt;
            }
        }

        // ... when the caller needs a map, which is the only shape a command root may be.
        std::optional<TagMap> root_map() {
            const auto remember = position_;
            auto value = root();
            if (!value.has_value()) {
                return std::nullopt;
            }
            if (auto* map = value->get_if<TagMap>()) {
                return std::move(*map);
            }
            LOG_WARN(Data, "root tag is a {}, not a map", tag_label(value->tag()));
            position_ = remember;
            return std::nullopt;
        }

        // A single named child: type, name, value.
        std::optional<NamedTag> named() {
            const auto remember = position_;
            try {
                const auto kind = read_kind();
                if (kind == TagKind::End) {
                    return NamedTag{ TagKind::End, {}, TagValue(TagMap{}) };
                }
                NamedTag named;
                named.kind = kind;
                named.name = read_name();
                auto value = body(kind);
                if (!value.has_value()) {
                    position_ = remember;
                    return std::nullopt;
                }
                named.value = std::move(*value);
                return named;
            }
            catch (const std::exception& error) {
                LOG_WARN(Data, "tag read failed at byte {}: {}", remember, error.what());
                position_ = remember;
                return std::nullopt;
            }
        }

        std::size_t position() const noexcept { return position_; }
        bool finished() const noexcept { return position_ >= data_.size(); }
        const ReadBudget& budget() const noexcept { return budget_; }

    private:
        std::span<const std::uint8_t> data_;
        Layout layout_;
        ReadBudget budget_;
        std::size_t position_ = 0;

        void need(std::size_t count) const {
            if (data_.size() - position_ < count) {
                throw BudgetExceeded("tag payload ended early");
            }
        }

        std::uint8_t read_byte() {
            need(1);
            return data_[position_++];
        }

        template<class T>
        T read_number() {
            static_assert(std::is_integral_v<T>);
            need(sizeof(T));
            T value = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                value = static_cast<T>((value << 8) | data_[position_++]);
            }
            return value;
        }

        std::string read_name() {
            const auto length = read_number<std::uint16_t>();
            need(length);
            const std::string_view wire(
                reinterpret_cast<const char*>(data_.data() + position_), length);
            position_ += length;
            budget_.spend(36 + 2 * static_cast<std::int64_t>(length));
            return codec::from_wire_text(wire);
        }

        void skip_name() {
            const auto length = read_number<std::uint16_t>();
            need(length);
            position_ += length;
        }

        TagKind read_kind() {
            const auto raw = read_byte();
            if (raw > static_cast<std::uint8_t>(TagKind::Longs)) {
                throw BudgetExceeded("invalid tag id " + std::to_string(raw));
            }
            return static_cast<TagKind>(raw);
        }

        std::optional<TagValue> read_named_body() {
            const auto kind = read_kind();
            skip_name();
            if (kind == TagKind::End) {
                return TagValue(TagMap{});
            }
            return body(kind);
        }

        // A single tag with its type byte, no name: what the network form writes.
        std::optional<TagValue> one_tag() {
            const auto kind = read_kind();
            if (kind == TagKind::End) {
                return TagValue(TagMap{});
            }
            return body(kind);
        }

        std::optional<TagValue> body(TagKind kind) {
            budget_.spend(1);
            switch (kind) {
            case TagKind::End:
                throw BudgetExceeded("a tag body cannot be the end marker");
            case TagKind::Byte:
                return TagValue(static_cast<std::int8_t>(read_byte()));
            case TagKind::Short:
                return TagValue(read_number<std::int16_t>());
            case TagKind::Int:
                return TagValue(read_number<std::int32_t>());
            case TagKind::Long:
                return TagValue(read_number<std::int64_t>());
            case TagKind::Float: {
                const auto raw = read_number<std::uint32_t>();
                float value = 0.0f;
                std::memcpy(&value, &raw, sizeof(value));
                return TagValue(value);
            }
            case TagKind::Double: {
                const auto raw = read_number<std::uint64_t>();
                double value = 0.0;
                std::memcpy(&value, &raw, sizeof(value));
                return TagValue(value);
            }
            case TagKind::Text:
                return TagValue(read_name());
            case TagKind::Bytes: {
                const auto count = read_number<std::int32_t>();
                if (count < 0) {
                    throw BudgetExceeded("negative byte array length");
                }
                budget_.spend(count);
                need(static_cast<std::size_t>(count));
                TagBytes values(data_.begin() + static_cast<std::ptrdiff_t>(position_),
                    data_.begin() + static_cast<std::ptrdiff_t>(position_ + count));
                position_ += static_cast<std::size_t>(count);
                return TagValue(std::move(values));
            }
            case TagKind::Ints: {
                const auto count = read_number<std::int32_t>();
                if (count < 0 || static_cast<std::size_t>(count) > (data_.size() - position_) / 4) {
                    throw BudgetExceeded("int array length does not fit the payload");
                }
                budget_.spend(4 * static_cast<std::int64_t>(count));
                TagInts values;
                values.reserve(static_cast<std::size_t>(count));
                for (std::int32_t i = 0; i < count; ++i) {
                    values.push_back(read_number<std::int32_t>());
                }
                return TagValue(std::move(values));
            }
            case TagKind::Longs: {
                const auto count = read_number<std::int32_t>();
                if (count < 0 || static_cast<std::size_t>(count) > (data_.size() - position_) / 8) {
                    throw BudgetExceeded("long array length does not fit the payload");
                }
                budget_.spend(8 * static_cast<std::int64_t>(count));
                TagLongs values;
                values.reserve(static_cast<std::size_t>(count));
                for (std::int32_t i = 0; i < count; ++i) {
                    values.push_back(read_number<std::int64_t>());
                }
                return TagValue(std::move(values));
            }
            case TagKind::List: {
                const ReadBudget::Level level(budget_);
                if (!level.ok()) {
                    throw BudgetExceeded("tag tree is nested too deeply");
                }
                const auto element = read_kind();
                const auto count = read_number<std::int32_t>();
                if (count < 0) {
                    throw BudgetExceeded("negative list length");
                }
                if (element == TagKind::End && count != 0) {
                    throw BudgetExceeded("a list of end markers can only be empty");
                }
                if (static_cast<std::size_t>(count) > data_.size() - position_ + 1) {
                    throw BudgetExceeded("list length does not fit the payload");
                }
                budget_.spend(count);
                TagList values;
                values.reserve(static_cast<std::size_t>(count));
                for (std::int32_t i = 0; i < count; ++i) {
                    auto child = body(element);
                    if (!child.has_value()) {
                        throw BudgetExceeded("list element could not be read");
                    }
                    values.push_back(std::move(*child));
                }
                return TagValue(std::move(values));
            }
            case TagKind::Map: {
                const ReadBudget::Level level(budget_);
                if (!level.ok()) {
                    throw BudgetExceeded("tag tree is nested too deeply");
                }
                TagMap children;
                while (true) {
                    const auto child_kind = read_kind();
                    if (child_kind == TagKind::End) {
                        return TagValue(std::move(children));
                    }
                    auto name = read_name();
                    auto child = body(child_kind);
                    if (!child.has_value()) {
                        throw BudgetExceeded("map entry could not be read");
                    }
                    children.insert_or_assign(std::move(name), std::move(*child));
                }
            }
            }
            throw BudgetExceeded("unknown tag type");
        }
    };

    // ---- writing -----------------------------------------------------------------

    class TagWriter {
    public:
        explicit TagWriter(Layout layout = Layout::Unnamed) : layout_(layout) {}

        void open_map() {
            out_.push_back(static_cast<std::uint8_t>(TagKind::Map));
            if (layout_ == Layout::Named && depth_ == 0) {
                write_name("");
            }
            ++depth_;
        }

        void close_map() {
            if (depth_ != 0) {
                out_.push_back(0);
                --depth_;
            }
        }

        // A standalone value: type and body, no name in front of it.
        void put_root(const TagValue& value) {
            if (!writable(value)) {
                failed_ = true;
                LOG_WARN(Data, "tag cannot be written: a list must keep one element type");
                return;
            }
            out_.push_back(static_cast<std::uint8_t>(value.tag()));
            write_body(value);
        }

        void put(std::string_view name, const TagValue& value) {
            if (!writable(value)) {
                failed_ = true;
                LOG_WARN(Data, "tag '{}' cannot be written: a list must keep one element type", name);
                return;
            }
            out_.push_back(static_cast<std::uint8_t>(value.tag()));
            write_name(name);
            write_body(value);
        }

        void put_text(std::string_view name, std::string_view text) {
            out_.push_back(static_cast<std::uint8_t>(TagKind::Text));
            write_name(name);
            write_name(text);
        }

        void put_int(std::string_view name, std::int32_t value) {
            out_.push_back(static_cast<std::uint8_t>(TagKind::Int));
            write_name(name);
            write_number<std::int32_t>(value);
        }

        void put_boolean(std::string_view name, bool value) {
            out_.push_back(static_cast<std::uint8_t>(TagKind::Byte));
            write_name(name);
            out_.push_back(value ? 1 : 0);
        }

        std::vector<std::uint8_t> finish() {
            if (failed_) {
                LOG_DEBUG(Data, "tag write was refused, nothing was produced");
                return {};
            }
            LOG_TRACE(Data, "tag written: {} bytes at {} levels deep", out_.size(), depth_);
            return std::move(out_);
        }

    private:
        std::vector<std::uint8_t> out_;
        Layout layout_;
        std::size_t depth_ = 0;
        bool failed_ = false;

        template<class T>
        void write_number(T value) {
            static_assert(std::is_integral_v<T>);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                out_.push_back(static_cast<std::uint8_t>(value >> ((sizeof(T) - i - 1) * 8)));
            }
        }

        void write_real(float value) {
            std::uint32_t raw = 0;
            std::memcpy(&raw, &value, sizeof(raw));
            write_number<std::uint32_t>(raw);
        }

        void write_real(double value) {
            std::uint64_t raw = 0;
            std::memcpy(&raw, &value, sizeof(raw));
            write_number<std::uint64_t>(raw);
        }

        // A name that does not fit the 16 bit count is dropped to an empty name, which is
        // what the game does rather than refusing the whole payload.
        void write_name(std::string_view text) {
            const auto wire = codec::to_wire_text(text);
            if (wire.size() > 0xFFFF) {
                write_number<std::uint16_t>(0);
                return;
            }
            write_number<std::uint16_t>(static_cast<std::uint16_t>(wire.size()));
            out_.insert(out_.end(), wire.begin(), wire.end());
        }

        bool writable(const TagValue& value) const {
            if (const auto* text = value.get_if<std::string>()) {
                return codec::to_wire_text(*text).size() <= 0xFFFF;
            }
            if (const auto* list = value.get_if<TagList>()) {
                if (list->empty()) {
                    return true;
                }
                const auto kind = list->front().tag();
                for (const auto& child : *list) {
                    if (child.tag() != kind || !writable(child)) {
                        return false;
                    }
                }
                return true;
            }
            if (const auto* map = value.get_if<TagMap>()) {
                for (const auto& [name, child] : *map) {
                    if (codec::to_wire_text(name).size() > 0xFFFF || !writable(child)) {
                        return false;
                    }
                }
            }
            return true;
        }

        void write_body(const TagValue& value) {
            if (const auto* v = value.get_if<std::int8_t>()) {
                out_.push_back(static_cast<std::uint8_t>(*v));
            }
            else if (const auto* v = value.get_if<std::int16_t>()) {
                write_number<std::int16_t>(*v);
            }
            else if (const auto* v = value.get_if<std::int32_t>()) {
                write_number<std::int32_t>(*v);
            }
            else if (const auto* v = value.get_if<std::int64_t>()) {
                write_number<std::int64_t>(*v);
            }
            else if (const auto* v = value.get_if<float>()) {
                write_real(*v);
            }
            else if (const auto* v = value.get_if<double>()) {
                write_real(*v);
            }
            else if (const auto* v = value.get_if<std::string>()) {
                write_name(*v);
            }
            else if (const auto* v = value.get_if<TagBytes>()) {
                write_number<std::int32_t>(static_cast<std::int32_t>(v->size()));
                out_.insert(out_.end(), v->begin(), v->end());
            }
            else if (const auto* v = value.get_if<TagInts>()) {
                write_number<std::int32_t>(static_cast<std::int32_t>(v->size()));
                for (const auto item : *v) {
                    write_number<std::int32_t>(item);
                }
            }
            else if (const auto* v = value.get_if<TagLongs>()) {
                write_number<std::int32_t>(static_cast<std::int32_t>(v->size()));
                for (const auto item : *v) {
                    write_number<std::int64_t>(item);
                }
            }
            else if (const auto* v = value.get_if<TagList>()) {
                const auto kind = v->empty() ? TagKind::End : v->front().tag();
                out_.push_back(static_cast<std::uint8_t>(kind));
                write_number<std::int32_t>(static_cast<std::int32_t>(v->size()));
                for (const auto& child : *v) {
                    write_body(child);
                }
            }
            else if (const auto* v = value.get_if<TagMap>()) {
                for (const auto& [name, child] : *v) {
                    out_.push_back(static_cast<std::uint8_t>(child.tag()));
                    write_name(name);
                    write_body(child);
                }
                out_.push_back(0);
            }
        }
    };

    // Convenience: one value as a standalone payload, which is what a data component and a
    // chat component are written as.
    inline std::vector<std::uint8_t> encode_value(const TagValue& value) {
        TagWriter writer(Layout::Unnamed);
        writer.put_root(value);
        return writer.finish();
    }

} // namespace terila::tag
