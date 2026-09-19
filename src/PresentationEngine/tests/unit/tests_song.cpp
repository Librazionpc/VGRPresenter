// Unit tests: Song & Lyrics Engine (docs/specs/25).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests song
#include "TestHarness.hpp"

void TestSongChords() {
    auto parse = [](const char* t) { return sn::ChordSystem::Parse(t); };
    auto c1 = parse("G");
    CHECK(c1.ok() && c1.value().root == "G" && c1.value().quality == sn::ChordQuality::Major);
    auto c2 = parse("Am");
    CHECK(c2.ok() && c2.value().root == "A" && c2.value().quality == sn::ChordQuality::Minor);
    auto c3 = parse("D/F#");
    CHECK(c3.ok() && c3.value().root == "D" && c3.value().bass == "F#");
    auto c4 = parse("Cmaj7");
    CHECK(c4.ok() && c4.value().quality == sn::ChordQuality::Seventh);
    auto c5 = parse("Em7");
    CHECK(c5.ok() && c5.value().quality == sn::ChordQuality::Minor && c5.value().extension == "7");
    auto c6 = parse("G/B");
    CHECK(c6.ok() && c6.value().root == "G" && c6.value().bass == "B");
    auto c7 = parse("C#m7b5");
    CHECK(c7.ok() && c7.value().root == "C#" && c7.value().quality == sn::ChordQuality::Minor &&
          c7.value().extension == "7b5");
    auto c8 = parse("Gsus4");
    CHECK(c8.ok() && c8.value().quality == sn::ChordQuality::Suspended);
    auto custom = parse("Xyz9");
    CHECK(custom.ok() && !custom.value().custom.empty());
    CHECK(!sn::ChordSystem::IsChordToken("Verse 1"));   // section markers aren't chords

    // Display round-trips.
    CHECK(c5.value().Display() == "Em7");
    CHECK(c3.value().Display() == "D/F#");
    CHECK(c4.value().Display() == "Cmaj7");

    // Transposition.
    auto t1 = sn::ChordSystem::Transpose(c1.value(), 2);
    CHECK(t1.ok() && t1.value().root == "A");
    auto t2 = sn::ChordSystem::Transpose(c2.value(), 2);
    CHECK(t2.ok() && t2.value().root == "B" && t2.value().quality == sn::ChordQuality::Minor);
    auto t3 = sn::ChordSystem::Transpose(c3.value(), 2);
    CHECK(t3.ok() && t3.value().root == "E" && t3.value().bass == "G#");
    auto t5 = sn::ChordSystem::Transpose(c5.value(), 2);
    CHECK(t5.ok() && t5.value().root == "F#" && t5.value().extension == "7");
    auto t6 = sn::ChordSystem::Transpose(c6.value(), 2);
    CHECK(t6.ok() && t6.value().root == "A" && t6.value().bass == "C#");
    // Enharmonic spelling preserved: Db +2 -> Eb, C# +2 -> D#.
    auto db = parse("Db");
    auto dbT = sn::ChordSystem::Transpose(db.value(), 2);
    CHECK(dbT.ok() && dbT.value().root == "Eb");
    auto csh = parse("C#");
    auto cshT = sn::ChordSystem::Transpose(csh.value(), 2);
    CHECK(cshT.ok() && cshT.value().root == "D#");
    // Keys.
    CHECK(sn::ChordSystem::TransposeKey("C", 2).ok());
    CHECK(sn::ChordSystem::TransposeKey("C", 2).value() == "D");
    CHECK(sn::ChordSystem::TransposeKey("Dm", 2).value() == "Em");
    CHECK(sn::ChordSystem::TransposeKey("G", -2).value() == "F");
    // Custom chords pass through transposition untouched.
    auto ct = sn::ChordSystem::Transpose(custom.value(), 5);
    CHECK(ct.ok() && ct.value().custom == custom.value().custom);
}
void TestSongProviders() {
    auto& eng = sn::SongEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    CHECK(eng.ProviderNames().size() >= 7);

    const char* chordpro =
        "{title: Amazing Grace}\n{key: C}\n{author: John Newton}\n{ccli: 12345}\n\n"
        "[Verse 1]\n"
        "C          G\n"
        "Amazing grace, how sweet the sound\n\n"
        "[Chorus]\n"
        "F          C\n"
        "That saved a wretch like me\n";
    auto id1 = eng.Import(chordpro, "chordpro", sn::ImportOptions{std::string("grace"), true, false});
    CHECK(id1.ok() && id1.value() == "grace");
    auto song1 = eng.GetSong("grace");
    CHECK(song1.ok());
    CHECK(song1.ok() && song1.value().metadata.title == "Amazing Grace");
    CHECK(song1.ok() && song1.value().metadata.originalKey == "C");
    CHECK(song1.ok() && song1.value().metadata.authors.size() == 1);
    CHECK(song1.ok() && song1.value().sections.size() == 2);
    CHECK(song1.ok() && song1.value().sections[0].name == "VERSE 1");
    bool hasChord = false;
    for (const auto& sec : song1.ok() ? song1.value().sections : std::vector<sn::SongSection>{})
        for (const auto& line : sec.lines)
            for (const auto& cr : line.chords)
                if (cr.chord.Display() == "C") hasChord = true;
    CHECK(hasChord);

    const char* opensong =
        "<song><title>Amazing Grace</title><author>John Newton</author><key>C</key>"
        "<lyrics><verse name=\"v1\"><lines>Amazing grace, how sweet the sound</lines></verse></lyrics>"
        "</song>";
    auto id2 = eng.Import(opensong, "opensong", sn::ImportOptions{std::string(), false, false});
    CHECK(id2.ok());
    CHECK(eng.GetSong(id2.value()).ok() && eng.GetSong(id2.value()).value().metadata.title == "Amazing Grace");

    const char* openlp =
        "<song><properties><titles><title>Holy Holy Holy</title></titles>"
        "<authors><author>Reginald Heber</author></authors></properties>"
        "<lyrics><verse name=\"v1\" type=\"verse\"><lines>Holy, holy, holy! Lord God Almighty</lines></verse></lyrics>"
        "</song>";
    auto id3 = eng.Import(openlp, "openlp", sn::ImportOptions{std::string(), false, false});
    CHECK(id3.ok());
    CHECK(eng.GetSong(id3.value()).ok() &&
          eng.GetSong(id3.value()).value().metadata.title == "Holy Holy Holy");

    const char* pro =
        "<rvb><song><title>Great Is Thy Faithfulness</title>"
        "<lyrics><verse name=\"v1\"><lines>Great is thy faithfulness, O God my Father</lines></verse></lyrics>"
        "</song></rvb>";
    auto id4 = eng.Import(pro, "propresenter", sn::ImportOptions{std::string(), false, false});
    CHECK(id4.ok());
    CHECK(eng.GetSong(id4.value()).ok() &&
          eng.GetSong(id4.value()).value().metadata.title == "Great Is Thy Faithfulness");

    const char* ew =
        "<song><title>How Great Thou Art</title>"
        "<stanza name=\"v1\"><line>O Lord my God, when I in awesome wonder</line></stanza>"
        "</song>";
    auto id5 = eng.Import(ew, "easyworship", sn::ImportOptions{std::string(), false, false});
    CHECK(id5.ok());
    CHECK(eng.GetSong(id5.value()).ok() && eng.GetSong(id5.value()).value().sections.size() == 1);

    const char* txt =
        "[Verse 1]\nAmazing grace, how sweet the sound\nthat saved a wretch like me\n\n"
        "[Chorus]\nI once was lost but now am found\n";
    auto id6 = eng.Import(txt, "txt", sn::ImportOptions{std::string(), false, false});
    CHECK(id6.ok());
    CHECK(eng.GetSong(id6.value()).ok() && eng.GetSong(id6.value()).value().sections.size() == 2);

    const char* js =
        "{\"metadata\":{\"title\":\"It Is Well\",\"originalKey\":\"D\"},"
        "\"sections\":[{\"id\":\"v1\",\"name\":\"VERSE 1\","
        "\"lines\":[{\"lyrics\":\"When peace like a river attendeth my way\","
        "\"chords\":[{\"column\":0,\"chord\":\"D\"}]}]}]}";
    auto id7 = eng.Import(js, "json", sn::ImportOptions{std::string(), false, false});
    CHECK(id7.ok());
    auto song7 = eng.GetSong(id7.value());
    CHECK(song7.ok() && song7.value().metadata.title == "It Is Well");
    CHECK(song7.ok() && song7.value().metadata.originalKey == "D");
    CHECK(song7.ok() && song7.value().sections[0].lines[0].chords.size() == 1);

    // Failure modes.
    CHECK(!eng.Import("not a song", "zzz", {}).ok());
    CHECK(!eng.Import("<song></song>", "opensong", {}).ok());
    CHECK(eng.Import(chordpro, "chordpro", sn::ImportOptions{std::string("grace"), false, false})
              .error().code == Err::Song_AlreadyExists);

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}
void TestSongEngine() {
    auto& eng = sn::SongEngine::Instance();
    auto& search = s::SearchEngine::Instance();
    CHECK(search.Initialize().ok());
    CHECK(search.Start().ok());
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());

    int importedEvents = 0;
    Subscription sub = EventBus::Instance().Subscribe<events::SongImported>(
        [&](const events::SongImported&) { ++importedEvents; }, 0);

    const char* chordpro =
        "{title: Amazing Grace}\n{key: C}\n{author: John Newton}\n\n"
        "[Verse 1]\nC          G\nAmazing grace, how sweet the sound\n\n"
        "[Chorus]\nF          C\nThat saved a wretch like me\n";
    auto id = eng.Import(chordpro, "chordpro", sn::ImportOptions{std::string("grace"), true, true});
    CHECK(id.ok() && id.value() == "grace");
    CHECK(importedEvents == 1);
    (void)EventBus::Instance().Unsubscribe(sub);

    // Content search finds the actual lyrics.
    auto found = eng.Search("how sweet the sound");
    CHECK(found.ok() && !found.value().empty());
    CHECK(found.ok() && found.value().front().id == "grace");

    // Transposition never mutates the original.
    auto original = eng.GetSong("grace");
    auto transposed = eng.Transposed("grace", 2);
    CHECK(transposed.ok());
    bool cBecameD = false;
    for (const auto& sec : transposed.value().sections)
        for (const auto& line : sec.lines)
            for (const auto& cr : line.chords)
                if (cr.chord.Display() == "D") cBecameD = true;
    CHECK(cBecameD);
    bool originalUntouched = false;
    for (const auto& sec : original.value().sections)
        for (const auto& line : sec.lines)
            for (const auto& cr : line.chords)
                if (cr.chord.Display() == "C") originalUntouched = true;
    CHECK(originalUntouched);

    // Performance key.
    CHECK(eng.SetPerformanceKey("grace", "D").ok());
    CHECK(eng.GetSong("grace").value().metadata.performanceKey == "D");
    CHECK(!eng.SetPerformanceKey("grace", "not-a-key").ok());

    // Arrangements are independent orderings of the same sections.
    auto arrs0 = eng.Arrangements("grace");
    CHECK(arrs0.ok() && arrs0.value().size() == 1 && arrs0.value()[0].id == "original");
    sn::Arrangement sunday;
    sunday.id = "sunday";
    sunday.name = "Sunday";
    sunday.sectionIds = {eng.GetSong("grace").value().sections[1].id,
                         eng.GetSong("grace").value().sections[0].id};   // chorus first
    CHECK(eng.AddArrangement("grace", sunday).ok());
    auto arrs = eng.Arrangements("grace");
    CHECK(arrs.ok() && arrs.value().size() == 2);
    CHECK(eng.AddArrangement("grace", sunday).error().code == Err::AlreadyExists);
    sn::Arrangement bad;
    bad.id = "bad";
    bad.name = "Bad";
    bad.sectionIds = {"no-such-section"};
    CHECK(eng.AddArrangement("grace", bad).error().code == Err::Song_SectionNotFound);
    CHECK(eng.RemoveArrangement("grace", "sunday").ok());
    CHECK(eng.Arrangements("grace").value().size() == 1);

    // Reorder sections without rewriting lyrics.
    auto before = eng.GetSong("grace").value().sections;
    std::vector<std::string> order;
    for (auto it = before.rbegin(); it != before.rend(); ++it) order.push_back(it->id);
    CHECK(eng.ReorderSections("grace", order).ok());
    auto after = eng.GetSong("grace").value().sections;
    CHECK(after.front().id == before.back().id);
    CHECK(!eng.ReorderSections("grace", {"only-one"}).ok());

    // Collections (worship set).
    CHECK(eng.CreateCollection("Sunday Morning").ok());
    CHECK(eng.AddToCollection("Sunday Morning", "grace").ok());
    auto col = eng.Collection("Sunday Morning");
    CHECK(col.ok() && col.value().size() == 1 && col.value()[0] == "grace");
    CHECK(eng.CollectionNames().size() == 1);

    // Duplicate detection — runs while the stored title is still the original so
    // the import guard sees the near-identical song (docs/specs/25 §Duplicate).
    const char* dup =
        "{title: Amazing Grace}\n{key: C}\n\n[Verse 1]\nAmazing grace, how sweet the sound\n";
    CHECK(eng.Import(dup, "chordpro", sn::ImportOptions{std::string(), true, true})
              .error().code == Err::Song_Duplicate);
    sn::Song similar = eng.GetSong("grace").value();
    similar.id = "grace-copy";
    similar.metadata.title = "Amazing Grace";
    auto created = eng.CreateSong(similar);
    CHECK(created.ok());
    auto groups = eng.FindDuplicates();
    CHECK(groups.ok() && !groups.value().empty());
    auto likely = eng.LikelyDuplicates("grace");
    CHECK(likely.ok() && !likely.value().empty());

    // Versioning + recovery.
    auto v0 = eng.Versions("grace");
    CHECK(v0.ok() && v0.value().size() == 1);
    auto updated = eng.GetSong("grace").value();
    updated.metadata.title = "Amazing Grace (Updated)";
    CHECK(eng.UpdateSong("grace", updated).ok());
    auto versions = eng.Versions("grace");
    CHECK(versions.ok() && versions.value().size() == 2);
    auto restored = eng.RestoreVersion("grace", versions.value()[0].id);
    CHECK(restored.ok() && restored.value().metadata.title == "Amazing Grace");
    CHECK(eng.RestoreVersion("grace", "nope").error().code == Err::Song_VersionNotFound);

    // Remove.
    CHECK(eng.RemoveSong("grace-copy").ok());
    CHECK(!eng.GetSong("grace-copy").ok());
    CHECK(eng.RemoveSong("grace").ok());
    CHECK(eng.SongCount() == 0);
    CHECK(!eng.RemoveSong("grace").ok());

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(search.Stop().ok());
    CHECK(search.Shutdown().ok());
}

// ===========================================================================
// Phase 14 — Service Flow, Playlist & Automation Engine (docs/specs/26)
// ===========================================================================
