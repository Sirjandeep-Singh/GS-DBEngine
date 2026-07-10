#include "key.h"

#include <cstring>
#include <stdexcept>

namespace {

void append_u16(std::vector<uint8_t>& buf, uint16_t v) {
    uint8_t bytes[2];
    std::memcpy(bytes, &v, sizeof(v));
    buf.insert(buf.end(), bytes, bytes + sizeof(v));
}

void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    uint8_t bytes[4];
    std::memcpy(bytes, &v, sizeof(v));
    buf.insert(buf.end(), bytes, bytes + sizeof(v));
}

uint16_t read_u16(const uint8_t*& ptr) {
    uint16_t v;
    std::memcpy(&v, ptr, sizeof(v));
    ptr += sizeof(v);
    return v;
}

uint32_t read_u32(const uint8_t*& ptr) {
    uint32_t v;
    std::memcpy(&v, ptr, sizeof(v));
    ptr += sizeof(v);
    return v;
}

} // namespace

std::vector<uint8_t> KeyCodec::encode(const Key& key) {
    std::vector<uint8_t> buf;
    append_u16(buf, static_cast<uint16_t>(key.size()));

    for (const Value& v : key) {
        if (is_null(v)) {
            buf.push_back(static_cast<uint8_t>(Tag::NUL));
        } else if (std::holds_alternative<int32_t>(v)) {
            buf.push_back(static_cast<uint8_t>(Tag::INT));
            int32_t iv = get_int(v);
            uint32_t raw;
            std::memcpy(&raw, &iv, sizeof(raw));
            append_u32(buf, raw);
        } else if (std::holds_alternative<float>(v)) {
            buf.push_back(static_cast<uint8_t>(Tag::FLOAT));
            float fv = get_float(v);
            uint32_t raw;
            std::memcpy(&raw, &fv, sizeof(raw));
            append_u32(buf, raw);
        } else if (std::holds_alternative<bool>(v)) {
            buf.push_back(static_cast<uint8_t>(Tag::BOOLEAN));
            buf.push_back(get_bool(v) ? 0x01 : 0x00);
        } else if (std::holds_alternative<std::string>(v)) {
            buf.push_back(static_cast<uint8_t>(Tag::VARCHAR));
            const std::string& s = get_string(v);
            append_u32(buf, static_cast<uint32_t>(s.size()));
            buf.insert(buf.end(), s.begin(), s.end());
        } else {
            throw std::runtime_error("KeyCodec::encode: unrecognized Value type");
        }
    }

    return buf;
}

Key KeyCodec::decode(const uint8_t*& ptr) {
    Key key;
    uint16_t num_elements = read_u16(ptr);
    key.reserve(num_elements);

    for (uint16_t i = 0; i < num_elements; i++) {
        Tag tag = static_cast<Tag>(*ptr);
        ptr += 1;

        switch (tag) {
            case Tag::NUL:
                key.emplace_back(std::monostate{});
                break;
            case Tag::INT: {
                uint32_t raw = read_u32(ptr);
                int32_t iv;
                std::memcpy(&iv, &raw, sizeof(iv));
                key.emplace_back(iv);
                break;
            }
            case Tag::FLOAT: {
                uint32_t raw = read_u32(ptr);
                float fv;
                std::memcpy(&fv, &raw, sizeof(fv));
                key.emplace_back(fv);
                break;
            }
            case Tag::BOOLEAN: {
                bool bv = (*ptr != 0x00);
                ptr += 1;
                key.emplace_back(bv);
                break;
            }
            case Tag::VARCHAR: {
                uint32_t len = read_u32(ptr);
                std::string s(reinterpret_cast<const char*>(ptr), len);
                ptr += len;
                key.emplace_back(std::move(s));
                break;
            }
            default:
                throw std::runtime_error("KeyCodec::decode: corrupt type tag");
        }
    }

    return key;
}
