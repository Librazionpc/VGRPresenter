#include "modules/vgr/VgrFormat.hpp"

#include <cstring>
#include <format>

namespace bps::vgr {

namespace {

// CRC32 (IEEE) — integrity per section + whole-file hash.
uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Crc32(const std::vector<uint8_t>& v) {
    return Crc32(v.data(), v.size());
}

std::string ToHex(uint32_t v) {
    return std::format("{:08x}", v);
}

// Minimal RLE for asset payloads: [count u32][value byte] runs.
std::vector<uint8_t> RleEncode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < in.size()) {
        uint8_t v = in[i];
        size_t run = 1;
        while (i + run < in.size() && in[i + run] == v && run < 0xFFFFFF) ++run;
        out.push_back(static_cast<uint8_t>(run & 0xFF));
        out.push_back(static_cast<uint8_t>((run >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((run >> 16) & 0xFF));
        out.push_back(v);
        i += run;
    }
    return out;
}

std::vector<uint8_t> RleDecode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i + 4 <= in.size()) {
        uint32_t run = static_cast<uint32_t>(in[i]) | (static_cast<uint32_t>(in[i + 1]) << 8) |
                       (static_cast<uint32_t>(in[i + 2]) << 16);
        out.insert(out.end(), run, in[i + 3]);
        i += 4;
    }
    return out;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

uint32_t GetU32(const std::vector<uint8_t>& b, size_t& pos) {
    if (pos + 4 > b.size()) return 0;
    uint32_t v = static_cast<uint32_t>(b[pos]) | (static_cast<uint32_t>(b[pos + 1]) << 8) |
                 (static_cast<uint32_t>(b[pos + 2]) << 16) |
                 (static_cast<uint32_t>(b[pos + 3]) << 24);
    pos += 4;
    return v;
}

void PutString(std::vector<uint8_t>& out, std::string_view s) {
    PutU32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

std::string GetString(const std::vector<uint8_t>& b, size_t& pos) {
    uint32_t len = GetU32(b, pos);
    if (pos + len > b.size()) return {};
    std::string s(reinterpret_cast<const char*>(b.data() + pos), len);
    pos += len;
    return s;
}

std::string HeaderToJson(const VgrHeader& h) {
    json::Value::Object root;
    root["formatVersion"] = json::Value::Number(static_cast<double>(h.formatVersion));
    root["engineVersion"] = json::Value::String(h.engineVersion);
    root["type"] = json::Value::Number(static_cast<double>(static_cast<int>(h.type)));
    root["uuid"] = json::Value::String(h.uuid);
    root["name"] = json::Value::String(h.name);
    root["createdMs"] = json::Value::Number(static_cast<double>(h.createdMs));
    root["modifiedMs"] = json::Value::Number(static_cast<double>(h.modifiedMs));
    root["author"] = json::Value::String(h.author);
    root["compression"] = json::Value::String(h.compression);
    root["integrityHash"] = json::Value::String(h.integrityHash);
    json::Value::Array deps;
    for (const auto& d : h.dependencyIds) deps.push_back(json::Value::String(d));
    root["dependencies"] = json::Value(std::move(deps));
    json::Value::Array refs;
    for (const auto& r : h.externalRefs) refs.push_back(json::Value::String(r));
    root["externalRefs"] = json::Value(std::move(refs));
    json::Value::Object search;
    for (const auto& [k, v] : h.searchMetadata) search[k] = json::Value::String(v);
    root["searchMetadata"] = json::Value(std::move(search));
    json::Value::Object custom;
    for (const auto& [k, v] : h.customMetadata) custom[k] = json::Value::String(v);
    root["customMetadata"] = json::Value(std::move(custom));
    return json::Value(std::move(root)).ToString();
}

VgrHeader HeaderFromJson(std::string_view json) {
    VgrHeader h;
    auto parsed = json::Parse(json);
    if (!parsed.ok()) return h;
    const json::Value& root = parsed.value();
    h.formatVersion = root.Find("formatVersion") ? static_cast<uint32_t>(root.Find("formatVersion")->asInt()) : 1;
    h.engineVersion = std::string(root.Find("engineVersion") ? root.Find("engineVersion")->asString() : "");
    h.type = static_cast<DocumentType>(root.Find("type") ? root.Find("type")->asInt() : 0);
    h.uuid = std::string(root.Find("uuid") ? root.Find("uuid")->asString() : "");
    h.name = std::string(root.Find("name") ? root.Find("name")->asString() : "");
    h.createdMs = root.Find("createdMs") ? static_cast<int64_t>(root.Find("createdMs")->asInt()) : 0;
    h.modifiedMs = root.Find("modifiedMs") ? static_cast<int64_t>(root.Find("modifiedMs")->asInt()) : 0;
    h.author = std::string(root.Find("author") ? root.Find("author")->asString() : "");
    h.compression = std::string(root.Find("compression") ? root.Find("compression")->asString() : "stored");
    h.integrityHash = std::string(root.Find("integrityHash") ? root.Find("integrityHash")->asString() : "");
    if (const auto* deps = root.Find("dependencies") ? root.Find("dependencies")->asArray() : nullptr)
        for (const auto& d : *deps) h.dependencyIds.push_back(std::string(d.asString()));
    if (const auto* refs = root.Find("externalRefs") ? root.Find("externalRefs")->asArray() : nullptr)
        for (const auto& r : *refs) h.externalRefs.push_back(std::string(r.asString()));
    if (const auto* search = root.Find("searchMetadata") ? root.Find("searchMetadata")->asObject() : nullptr)
        for (const auto& [k, v] : *search) h.searchMetadata[std::string(k)] = std::string(v.asString());
    if (const auto* custom = root.Find("customMetadata") ? root.Find("customMetadata")->asObject() : nullptr)
        for (const auto& [k, v] : *custom) h.customMetadata[std::string(k)] = std::string(v.asString());
    return h;
}

} // namespace

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------
uint32_t VgrSerializer::PeekVersion(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 12) return 0;
    if (std::memcmp(bytes.data(), kMagic, 8) != 0) return 0;
    size_t pos = 8;
    return GetU32(bytes, pos);
}

Result<std::vector<uint8_t>> VgrSerializer::Write(const VgrDocument& doc) {
    std::vector<uint8_t> out;
    // Magic + format version.
    out.insert(out.end(), kMagic, kMagic + 8);
    PutU32(out, doc.header.formatVersion);

    // Header JSON (self-describing).
    std::string headerJson = HeaderToJson(doc.header);
    PutU32(out, static_cast<uint32_t>(headerJson.size()));
    out.insert(out.end(), headerJson.begin(), headerJson.end());

    // Section count: documentJson + sections + embedded assets.
    size_t total = 1 + doc.sections.size() + doc.embeddedAssets.size();
    PutU32(out, static_cast<uint32_t>(total));

    // Write each section: name, kind, length, crc32, payload.
    auto writeSection = [&](const std::string& name, const std::string& kind,
                            const std::vector<uint8_t>& raw) {
        std::vector<uint8_t> payload = raw;
        if (doc.header.compression == "rle" && kind == "asset") payload = RleEncode(raw);
        PutString(out, name);
        PutString(out, kind);
        PutU32(out, static_cast<uint32_t>(payload.size()));
        uint32_t crc = Crc32(payload);
        PutU32(out, crc);
        out.insert(out.end(), payload.begin(), payload.end());
    };

    // 1. Document JSON body.
    writeSection("document", "json",
                 std::vector<uint8_t>(doc.documentJson.begin(), doc.documentJson.end()));
    // 2. Named sections.
    for (const auto& s : doc.sections) writeSection(s.name, s.kind, s.payload);
    // 3. Embedded assets.
    for (const auto& [name, bytes] : doc.embeddedAssets) writeSection(name, "asset", bytes);

    // Whole-file integrity hash (after payloads).
    uint32_t fileCrc = Crc32(out.data(), out.size());
    // Materialize the hex string first: using two separate temporaries' c_str()
    // as an [begin, end) range would point at unrelated stack buffers.
    std::string hex = ToHex(fileCrc);
    out.insert(out.end(), hex.begin(), hex.end());
    return out;
}

Result<VgrDocument> VgrSerializer::Read(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 12) return Error::Make(Err::Vgr_BadSignature, "VgrSerializer",
                                              "file too small to be .vgr");
    if (std::memcmp(bytes.data(), kMagic, 8) != 0)
        return Error::Make(Err::Vgr_BadSignature, "VgrSerializer", "bad magic number");
    size_t pos = 8;
    uint32_t version = GetU32(bytes, pos);
    if (version > kFormatVersion)
        return Error::Make(Err::Vgr_UnsupportedVersion, "VgrSerializer",
                           std::format("format version {} not supported", version));

    uint32_t headerLen = GetU32(bytes, pos);
    if (pos + headerLen > bytes.size())
        return Error::Make(Err::Vgr_CorruptSection, "VgrSerializer", "truncated header");
    std::string headerJson(reinterpret_cast<const char*>(bytes.data() + pos), headerLen);
    pos += headerLen;

    VgrDocument doc;
    doc.header = HeaderFromJson(headerJson);
    doc.header.formatVersion = version;

    uint32_t sectionCount = GetU32(bytes, pos);
    for (uint32_t i = 0; i < sectionCount; ++i) {
        std::string name = GetString(bytes, pos);
        std::string kind = GetString(bytes, pos);
        uint32_t len = GetU32(bytes, pos);
        uint32_t crc = GetU32(bytes, pos);
        if (pos + len > bytes.size())
            return Error::Make(Err::Vgr_CorruptSection, "VgrSerializer",
                               "truncated section '" + name + "'");
        std::vector<uint8_t> payload(bytes.begin() + static_cast<long>(pos),
                                     bytes.begin() + static_cast<long>(pos + len));
        pos += len;
        if (Crc32(payload) != crc)
            return Error::Make(Err::Vgr_CorruptSection, "VgrSerializer",
                               "CRC mismatch in section '" + name + "'");
        if (doc.header.compression == "rle" && kind == "asset") payload = RleDecode(payload);
        if (name == "document" && kind == "json") {
            doc.documentJson.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
        } else if (kind == "asset") {
            doc.embeddedAssets.emplace_back(name, std::move(payload));
        } else {
            doc.sections.push_back(VgrSection{name, kind, std::move(payload), crc});
        }
    }

    // Whole-file integrity hash (8 hex chars appended after the payloads).
    if (bytes.size() - pos != 8)
        return Error::Make(Err::Vgr_CorruptSection, "VgrSerializer",
                           "missing whole-file integrity hash");
    uint32_t fileCrc = Crc32(bytes.data(), bytes.size() - 8);
    std::string expected = ToHex(fileCrc);
    std::string actual(reinterpret_cast<const char*>(bytes.data() + pos), 8);
    if (actual != expected)
        return Error::Make(Err::Vgr_CorruptSection, "VgrSerializer",
                           "whole-file CRC mismatch");

    // Migration hook.
    if (version < kFormatVersion) (void)Migrate(doc, version);
    return doc;
}

Result<void> VgrSerializer::Migrate(VgrDocument& doc, uint32_t fromVersion) {
    // v0 (pre-release) -> v1: no structural change; record the migration.
    if (fromVersion == 0) {
        doc.header.customMetadata["migratedFrom"] = "0";
        doc.header.formatVersion = kFormatVersion;
    }
    return Ok();
}

} // namespace bps::vgr
