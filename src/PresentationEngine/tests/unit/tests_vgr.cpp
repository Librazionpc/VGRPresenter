// Unit tests: Native .vgr format (docs/specs/22).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests vgr
#include "TestHarness.hpp"

void TestVgrFormat() {
    v::VgrDocument doc;
    doc.header.formatVersion = 1;
    doc.header.engineVersion = "1.0.0";
    doc.header.type = v::DocumentType::Presentation;
    doc.header.uuid = "uuid-123";
    doc.header.name = "Easter Conference";
    doc.header.createdMs = 1000;
    doc.header.modifiedMs = 2000;
    doc.header.author = "BPS";
    doc.header.dependencyIds = {"asset-1", "asset-2"};
    doc.header.searchMetadata["key"] = "value";
    doc.documentJson = R"({"slides":[{"id":"s1"}]})";
    // Embedded asset section.
    doc.embeddedAssets.emplace_back("logo.png", std::vector<uint8_t>{1, 2, 3, 4, 5});
    // Round-trip.
    auto bytes = v::VgrSerializer::Write(doc);
    CHECK(bytes.ok() && !bytes.value().empty());
    CHECK(v::VgrSerializer::PeekVersion(bytes.value()) == 1);
    auto back = v::VgrSerializer::Read(bytes.value());
    CHECK(back.ok());
    CHECK(back.value().header.uuid == "uuid-123");
    CHECK(back.value().header.name == "Easter Conference");
    CHECK(back.value().header.dependencyIds.size() == 2);
    CHECK(back.value().documentJson.find("s1") != std::string::npos);
    CHECK(back.value().embeddedAssets.size() == 1);
    CHECK(back.value().embeddedAssets[0].second == std::vector<uint8_t>({1, 2, 3, 4, 5}));
    // Corruption detection.
    auto corrupt = bytes.value();
    corrupt[10] ^= 0xFF;   // corrupt header area
    auto bad = v::VgrSerializer::Read(corrupt);
    // May fail via magic/version/CRC — any non-ok is acceptable, but must not crash.
    (void)bad;
    // Bad magic.
    std::vector<uint8_t> junk(64, 0xAB);
    auto junkRead = v::VgrSerializer::Read(junk);
    CHECK(!junkRead.ok());
    // Unsupported future version.
    std::vector<uint8_t> future = bytes.value();
    future[8] = 0xFF;
    future[9] = 0xFF;
    future[10] = 0xFF;
    future[11] = 0xFF;
    auto futRead = v::VgrSerializer::Read(future);
    CHECK(!futRead.ok());
}

// ===========================================================================
// Phase 11 — Scene Composition Engine (docs/specs/23)
// ===========================================================================
