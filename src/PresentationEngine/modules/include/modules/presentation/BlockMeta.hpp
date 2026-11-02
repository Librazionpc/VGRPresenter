#pragma once

// ---- reading a design's metaJson --------------------------------------------

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

#include <QString>
#include <QTextBoundaryFinder>

#include "modules/rendering/TextEngine.hpp"   // rendering::TextAlign / TextVAlign

namespace bps::presentation {
// ===== the block metaJson probes ============================================
inline bool IsHexColor(std::string_view s) {
    if (s.size() != 7 && s.size() != 9)
        return false;
    if (s[0] != '#')
        return false;
    return std::all_of(s.begin() + 1, s.end(), [](unsigned char c) {
        return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// ---- block metaJson: the design Text() helper's colour/size/align fields ----
inline std::string MetaString(std::string_view json, std::string_view key) {
    const std::string needle = std::format("\"{}\":\"", key);
    const size_t at = json.find(needle);
    if (at == std::string_view::npos) return {};
    const size_t start = at + needle.size();
    const size_t end = json.find('"', start);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(start, end - start));
}

inline std::string ApplyTextCase(std::string_view text, std::string_view textCase) {
    const QString input = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    QString output = input;
    if (textCase == "upper") {
        output = input.toUpper();
    } else if (textCase == "lower") {
        output = input.toLower();
    } else if (textCase == "capitalize") {
        output = input.toLower();
        QTextBoundaryFinder finder(QTextBoundaryFinder::Word, output);
        qsizetype start = 0;
        while (true) {
            const qsizetype end = finder.toNextBoundary();
            if (end < 0)
                break;
            for (qsizetype i = start; i < end; ++i) {
                if (output.at(i).isLetterOrNumber()) {
                    output[i] = output.at(i).toTitleCase();
                    break;
                }
            }
            start = end;
        }
    } else {
        return std::string(text);
    }
    const QByteArray utf8 = output.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}
inline bool MetaHas(std::string_view json, std::string_view key) {
    return json.find(std::format("\"{}\":\"", key)) != std::string_view::npos;
}
inline double MetaNumber(std::string_view json, std::string_view key, double dflt) {
    const std::string needle = std::format("\"{}\":", key);
    const size_t at = json.find(needle);
    if (at == std::string_view::npos) return dflt;
    const size_t start = at + needle.size();
    const size_t end = json.find_first_of(",}", start);
    if (end == std::string_view::npos) return dflt;
    const std::string num(json.substr(start, end - start));
    double out = dflt;
    const char* b = num.c_str();
    char* e = nullptr;
    out = std::strtod(b, &e);
    return e == b ? dflt : out;
}
inline bool MetaBool(std::string_view json, std::string_view key) {
    return json.find(std::format("\"{}\":true", key)) != std::string_view::npos;
}
inline std::string MetaBoolToken(std::string_view json, std::string_view key) {
    const std::string needle = std::format("\"{}\"", key);
    for (size_t at = json.find(needle); at != std::string_view::npos;
         at = json.find(needle, at + 1)) {
        size_t i = at + needle.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i >= json.size() || json[i] != ':') continue;   // a value, not this key
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (json.compare(i, 4, "true") == 0) return "true";
        if (json.compare(i, 5, "false") == 0) return "false";
    }
    return {};
}

inline rendering::TextAlign ParseAlign(std::string_view a) {
    if (a == "left") return rendering::TextAlign::Left;
    if (a == "right") return rendering::TextAlign::Right;
    if (a == "justify") return rendering::TextAlign::Justify;
    return rendering::TextAlign::Center;   // canvas default (see the call site)
}
inline rendering::TextVAlign ParseVAlign(std::string_view a) {
    if (a == "top") return rendering::TextVAlign::Top;
    if (a == "bottom") return rendering::TextVAlign::Bottom;
    return rendering::TextVAlign::Middle;   // canvas default (see the call site)
}
} // namespace bps::presentation