#include "modules/search/IndexStorage.hpp"

#include "core/config/Json.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace bps::search {

namespace {

// Simple tokenizer: lowercase alphanumeric runs. Multibyte UTF-8 is preserved
// as opaque runs (Bible/song content may be Unicode).
std::vector<std::string> Tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    };
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

std::vector<std::string> AllFields(const SearchDocument& doc) {
    std::vector<std::string> fields;
    fields.push_back(doc.title);
    fields.push_back(doc.content);
    fields.push_back(doc.author);
    fields.push_back(doc.type);
    for (const auto& t : doc.tags) fields.push_back(t);
    for (const auto& k : doc.keywords) fields.push_back(k);
    for (const auto& [k, v] : doc.metadata) {
        fields.push_back(k);
        fields.push_back(v);
    }
    return fields;
}

} // namespace

Result<void> IndexStorage::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    return Ok();
}

Result<void> IndexStorage::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    index_.clear();
    documents_.clear();
    initialized_ = false;
    return Ok();
}

size_t IndexStorage::Upsert(const SearchDocument& doc) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Remove old postings for this doc first (incremental re-index).
    if (documents_.count(doc.id)) {
        for (auto& [term, postings] : index_) postings.erase(doc.id);
    }
    documents_[doc.id] = doc;

    std::map<std::string, size_t, std::less<>> freqs;
    size_t positions = 0;
    for (const auto& field : AllFields(doc)) {
        for (auto& tok : Tokenize(field)) {
            ++freqs[tok];
            ++positions;
        }
    }
    for (const auto& [term, freq] : freqs) {
        Posting p;
        p.docId = doc.id;
        p.frequency = freq;
        p.positions = positions;
        index_[term][doc.id] = p;
    }
    return freqs.size();
}

Result<void> IndexStorage::Remove(std::string_view docId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!documents_.count(std::string(docId)))
        return Error::Make(Err::Search_DocumentNotFound, "IndexStorage",
                           "document '" + std::string(docId) + "' not found");
    for (auto& [term, postings] : index_) postings.erase(std::string(docId));
    documents_.erase(std::string(docId));
    return Ok();
}

std::vector<Posting> IndexStorage::Lookup(std::string_view term) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key;
    for (char c : term) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto it = index_.find(key);
    if (it == index_.end()) return {};
    std::vector<Posting> out;
    out.reserve(it->second.size());
    for (const auto& [_, p] : it->second) out.push_back(p);
    return out;
}

std::vector<std::string> IndexStorage::TermsWithPrefix(std::string_view prefix, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    std::string key;
    for (char c : prefix)
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto it = index_.lower_bound(key);
    while (it != index_.end() && out.size() < limit) {
        if (it->first.rfind(key, 0) != 0) break;
        out.push_back(it->first);
        ++it;
    }
    return out;
}

Result<SearchDocument> IndexStorage::GetDocument(std::string_view docId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = documents_.find(std::string(docId));
    if (it == documents_.end())
        return Error::Make(Err::Search_DocumentNotFound, "IndexStorage",
                           "document '" + std::string(docId) + "' not found");
    return it->second;
}

std::vector<std::string> IndexStorage::DocumentIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(documents_.size());
    for (const auto& [id, _] : documents_) ids.push_back(id);
    return ids;
}

size_t IndexStorage::DocumentCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return documents_.size();
}

size_t IndexStorage::TermCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
}

size_t IndexStorage::TotalPostings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [_, postings] : index_) n += postings.size();
    return n;
}

std::string IndexStorage::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value::Array docs;
    for (const auto& [_, doc] : documents_) {
        json::Value::Object d;
        d["id"] = json::Value::String(doc.id);
        d["type"] = json::Value::String(doc.type);
        d["title"] = json::Value::String(doc.title);
        d["content"] = json::Value::String(doc.content);
        d["author"] = json::Value::String(doc.author);
        d["language"] = json::Value::String(doc.language);
        d["source"] = json::Value::String(doc.source);
        d["version"] = json::Value::String(doc.version);
        d["createdMs"] = json::Value::Number(static_cast<double>(doc.createdMs));
        d["modifiedMs"] = json::Value::Number(static_cast<double>(doc.modifiedMs));
        d["rankBoost"] = json::Value::Number(static_cast<double>(doc.rankBoost));
        json::Value::Array tags;
        for (const auto& t : doc.tags) tags.push_back(json::Value::String(t));
        d["tags"] = json::Value(std::move(tags));
        json::Value::Array kw;
        for (const auto& k : doc.keywords) kw.push_back(json::Value::String(k));
        d["keywords"] = json::Value(std::move(kw));
        json::Value::Array rel;
        for (const auto& r : doc.relatedIds) rel.push_back(json::Value::String(r));
        d["related"] = json::Value(std::move(rel));
        json::Value::Object meta;
        for (const auto& [k, v] : doc.metadata) meta[k] = json::Value::String(v);
        d["metadata"] = json::Value(std::move(meta));
        docs.push_back(json::Value(std::move(d)));
    }
    json::Value::Object root;
    root["version"] = json::Value::Number(1);
    root["documents"] = json::Value(std::move(docs));
    return json::Value(std::move(root)).ToString();
}

Result<size_t> IndexStorage::Restore(std::string_view json) {
    auto parsed = json::Parse(json);
    if (!parsed.ok()) return parsed.error();
    const json::Value& root = parsed.value();
    const auto* arr = root.Find("documents") ? root.Find("documents")->asArray() : nullptr;
    if (!arr) return Error::Make(Err::Search_CorruptIndex, "IndexStorage",
                                 "snapshot missing documents array");
    std::lock_guard<std::mutex> lock(mutex_);
    index_.clear();
    documents_.clear();
    size_t terms = 0;
    for (const auto& v : *arr) {
        SearchDocument doc;
        doc.id = std::string(v.Find("id") ? v.Find("id")->asString() : "");
        doc.type = std::string(v.Find("type") ? v.Find("type")->asString() : "");
        doc.title = std::string(v.Find("title") ? v.Find("title")->asString() : "");
        doc.content = std::string(v.Find("content") ? v.Find("content")->asString() : "");
        doc.author = std::string(v.Find("author") ? v.Find("author")->asString() : "");
        doc.language = std::string(v.Find("language") ? v.Find("language")->asString() : "");
        doc.source = std::string(v.Find("source") ? v.Find("source")->asString() : "");
        doc.version = std::string(v.Find("version") ? v.Find("version")->asString() : "");
        doc.createdMs = v.Find("createdMs") ? static_cast<int64_t>(v.Find("createdMs")->asInt()) : 0;
        doc.modifiedMs = v.Find("modifiedMs") ? static_cast<int64_t>(v.Find("modifiedMs")->asInt()) : 0;
        doc.rankBoost = v.Find("rankBoost") ? static_cast<int64_t>(v.Find("rankBoost")->asInt()) : 0;
        if (const auto* tags = v.Find("tags") ? v.Find("tags")->asArray() : nullptr)
            for (const auto& t : *tags) doc.tags.push_back(std::string(t.asString()));
        if (const auto* kw = v.Find("keywords") ? v.Find("keywords")->asArray() : nullptr)
            for (const auto& k : *kw) doc.keywords.push_back(std::string(k.asString()));
        if (const auto* rel = v.Find("related") ? v.Find("related")->asArray() : nullptr)
            for (const auto& r : *rel) doc.relatedIds.push_back(std::string(r.asString()));
        if (const auto* meta = v.Find("metadata") ? v.Find("metadata")->asObject() : nullptr)
            for (const auto& [k, val] : *meta)
                doc.metadata[std::string(k)] = std::string(val.asString());
        if (doc.id.empty()) continue;
        documents_[doc.id] = doc;
        std::map<std::string, size_t, std::less<>> freqs;
        for (const auto& field : AllFields(doc)) {
            for (auto& tok : Tokenize(field)) ++freqs[tok];
        }
        for (const auto& [term, freq] : freqs) {
            Posting p;
            p.docId = doc.id;
            p.frequency = freq;
            index_[term][doc.id] = p;
            ++terms;
        }
    }
    return terms;
}

} // namespace bps::search
