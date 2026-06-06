// attr_pool.cpp — AttrPool and ModuleApi implementation

#include "ast/attr_pool.hpp"
#include "ast/token_info.hpp"
#include "diag/mapper.hpp"
#include "trust/version.h"
#include "utils/file_io.hpp"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MD5.h"
#include <zlib.h>
#include <cstring>
#include <array>
#include <fstream>

namespace trust {

// ────────────────────────────────────────────────────────────────────────────
// AttrParam::as_string(SourceMap) — resolve MapperRange to string_view
// ────────────────────────────────────────────────────────────────────────────

std::string_view AttrParam::as_string(const SourceMap<MapperFile>& mapper) const {
    if (is_string())
        return std::get<std::string_view>(m_value);
    const auto& range = std::get<MapperRange>(m_value);
    auto file_content = mapper.source(range.begin.fileIdx());
    // Offsets are 1-based, convert to 0-based for substr
    auto begin_off = range.begin.offset() - 1;
    auto end_off = range.end.offset() - 1;
    EXPECT(end_off >= begin_off && end_off <= file_content.size());
    return file_content.substr(begin_off, end_off - begin_off);
}

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// Sort a vector and remove duplicates.
void sort_and_unique(std::vector<AttrId>& ids) {
    std::sort(ids.begin(), ids.end());
    auto last = std::unique(ids.begin(), ids.end());
    ids.erase(last, ids.end());
}

struct BestSetMatch {
    AttrId m_id{0};
    std::size_t m_overlap{0};
};

BestSetMatch find_best_set_match(const std::vector<AttrId>& sorted_ids, const std::vector<AttrSet>& sets) {
    BestSetMatch result;

    for (const auto& set : sets) {
        // Count how many members of this set are in sorted_ids
        std::size_t overlap = 0;
        auto sit = set.m_members.begin();
        auto iit = sorted_ids.begin();
        while (sit != set.m_members.end() && iit != sorted_ids.end()) {
            if (*sit == *iit) {
                ++overlap;
                ++sit;
                ++iit;
            } else if (*sit < *iit) {
                ++sit;
            } else {
                ++iit;
            }
        }
        if (overlap > result.m_overlap) {
            result.m_id = set.m_id;
            result.m_overlap = overlap;
        }
    }

    return result;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// AttrPool constructor — zero-initializes builtin IDs, reserves slot 0
// ────────────────────────────────────────────────────────────────────────────

AttrPool::AttrPool()
: m_builtin_ids{}
, m_name_to_id() {
    // Reserve slot 0 as invalid AttrId (placeholder).
    // All real attributes start from ID 1.
    Attr placeholder;
    placeholder.m_id = 0;
    placeholder.m_name = std::string_view{};
    placeholder.m_builtin_kind = BuiltinAttrKind::kNone;
    m_attrs.push_back(std::move(placeholder));
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::make_placeholder_param
// ────────────────────────────────────────────────────────────────────────────

AttrParam AttrPool::make_placeholder_param(AttrParamType type) {
    switch (type) {
    case AttrParamType::kString:
        return AttrParam(std::string_view{});
    case AttrParamType::kRange:
        return AttrParam(MapperRange{});
    }
    return AttrParam(std::string_view{}); // unreachable
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::register_attr_impl — unified registration
// ────────────────────────────────────────────────────────────────────────────

AttrId AttrPool::register_attr_impl(std::string_view name, std::vector<AttrParam> default_params, bool variadic, BuiltinAttrKind kind) {
    EXPECT(kind == BuiltinAttrKind::kNone || (static_cast<std::size_t>(kind) > 0 && static_cast<std::size_t>(kind) < kBuiltinAttrCount));

    std::string_view interned_name = intern(name);

    // Intern string params
    for (auto& p : default_params) {
        if (p.is_string()) {
            p = AttrParam(intern(p.as_string()));
        }
    }

    // Look up existing by name (O(1) via hash map)
    auto it = m_name_to_id.find(std::string(interned_name));
    if (it != m_name_to_id.end()) {
        AttrId id = it->second;
        auto idx = id & detail::kAttrIndexMask;
        EXPECT(idx < m_attrs.size());
        auto& existing = m_attrs[idx];

        // For variadic attrs, ensure the existing is also variadic with matching base type
        if (variadic) {
            if (existing.m_variadic && !existing.m_default_params.empty() &&
                existing.m_default_params[0].type() == (default_params.empty() ? AttrParamType::kString : default_params[0].type())) {
                if (kind != BuiltinAttrKind::kNone) {
                    if (existing.m_builtin_kind == BuiltinAttrKind::kNone)
                        existing.m_builtin_kind = kind;
                    else
                        EXPECT(existing.m_builtin_kind == kind);
                }
                return id;
            }
            // Name matches but not variadic or type mismatch — return existing (best effort)
            return id;
        }

        // Non-variadic: check param match
        if (existing.m_default_params.size() == default_params.size()) {
            bool match = true;
            for (std::size_t i = 0; i < default_params.size(); ++i) {
                if (!(existing.m_default_params[i] == default_params[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                if (kind != BuiltinAttrKind::kNone) {
                    if (existing.m_builtin_kind == BuiltinAttrKind::kNone)
                        existing.m_builtin_kind = kind;
                    else
                        EXPECT(existing.m_builtin_kind == kind);
                }
                return id;
            }
        }
        // Name matches but params differ — return existing (best effort)
        return id;
    }

    // Create new attribute
    Attr attr;
    attr.m_id = static_cast<AttrId>(m_attrs.size());
    attr.m_name = interned_name;
    attr.m_default_params = std::move(default_params);
    attr.m_variadic = variadic;
    if (kind != BuiltinAttrKind::kNone)
        attr.m_builtin_kind = kind;

    m_attrs.push_back(std::move(attr));
    AttrId new_id = m_attrs.back().m_id;
    // interned_name is already a stable string_view into m_strings, safe to use as key
    // Use find/insert with pair instead of operator[] (workaround for libstdc++ 14 + clang incompatibility)
    {
        auto found = m_name_to_id.find(interned_name);
        if (found != m_name_to_id.end()) {
            found->second = new_id;
        } else {
            m_name_to_id.insert(std::make_pair(interned_name, new_id));
        }
    }

    // Record builtin ID if this is a built-in attribute
    if (kind != BuiltinAttrKind::kNone) {
        auto kind_idx = static_cast<std::size_t>(kind);
        if (m_builtin_ids[kind_idx] == 0)
            set_builtin_id(kind, new_id);
        else
            EXPECT(m_builtin_ids[kind_idx] == new_id);
    }

    return new_id;
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::register_attr (by param types)
// ────────────────────────────────────────────────────────────────────────────

AttrId AttrPool::register_attr(std::string_view name, std::vector<AttrParamType> required_param_types, BuiltinAttrKind kind) {
    // Create placeholder params from the given types
    std::vector<AttrParam> default_params;
    default_params.reserve(required_param_types.size());
    for (auto type : required_param_types) {
        default_params.push_back(make_placeholder_param(type));
    }
    return register_attr_impl(name, std::move(default_params), false, kind);
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::register_attr (by concrete params)
// ────────────────────────────────────────────────────────────────────────────

AttrId AttrPool::register_attr(std::string_view name, std::vector<AttrParam> default_params, BuiltinAttrKind kind) {
    return register_attr_impl(name, std::move(default_params), false, kind);
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::register_variadic_attr
// ────────────────────────────────────────────────────────────────────────────

AttrId AttrPool::register_variadic_attr(std::string_view name, AttrParamType base_type, BuiltinAttrKind kind) {
    return register_attr_impl(name, {make_placeholder_param(base_type)}, true, kind);
}

// ────────────────────────────────────────────────────────────────────────────
// Attr::to_string — human-readable representation (without SourceMap)
// ────────────────────────────────────────────────────────────────────────────

std::string Attr::to_string() const {
    std::string result(m_name);

    if (!m_default_params.empty()) {
        result += "(";
        for (std::size_t i = 0; i < m_default_params.size(); ++i) {
            if (i > 0)
                result += ", ";
            const auto& p = m_default_params[i];
            // Only string params can be printed without context
            if (p.is_string()) {
                result += "\"";
                result += p.as_string();
                result += "\"";
            }
        }
        result += ")";
    }

    return result;
}

// ────────────────────────────────────────────────────────────────────────────
// Attr::to_string — human-readable representation (with SourceMap for Range)
// ────────────────────────────────────────────────────────────────────────────

std::string Attr::to_string(const SourceMap<MapperFile>& mapper) const {
    std::string result(m_name);

    if (!m_default_params.empty()) {
        result += "(";
        for (std::size_t i = 0; i < m_default_params.size(); ++i) {
            if (i > 0)
                result += ", ";
            const auto& p = m_default_params[i];
            auto s = p.as_string(mapper);
            result += "\"";
            result += s;
            result += "\"";
        }
        result += ")";
    }

    return result;
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::lookup
// ────────────────────────────────────────────────────────────────────────────

std::optional<AttrId> AttrPool::lookup(std::string_view name) const noexcept {
    auto it = m_name_to_id.find(std::string(name));
    if (it != m_name_to_id.end())
        return it->second;
    return std::nullopt;
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::get_name
// ────────────────────────────────────────────────────────────────────────────

std::string_view AttrPool::get_name(AttrId id) const {
    auto idx = id & detail::kAttrIndexMask;
    EXPECT(idx < m_attrs.size());
    return m_attrs[idx].m_name;
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::has_attr (by name)
// ────────────────────────────────────────────────────────────────────────────

bool AttrPool::has_attr(std::string_view name) const noexcept {
    return m_name_to_id.find(std::string(name)) != m_name_to_id.end();
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::has_attr (by BuiltinAttrKind)
// ────────────────────────────────────────────────────────────────────────────

bool AttrPool::has_attr(BuiltinAttrKind kind) const noexcept {
    auto idx = static_cast<std::size_t>(kind);
    if (idx == 0 || idx >= kBuiltinAttrCount)
        return false;
    return m_builtin_ids[idx] != 0;
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::add_set — create a set (or return existing)
// ────────────────────────────────────────────────────────────────────────────

AttrId AttrPool::add_set(std::vector<AttrId> ids) {
    sort_and_unique(ids);

    // Look up existing
    for (std::size_t i = 0; i < m_sets.size(); ++i) {
        if (m_sets[i].m_members == ids)
            return m_sets[i].m_id;
    }

    // Create new set
    AttrSet new_set;
    new_set.m_members = std::move(ids);
    new_set.m_id = detail::kAttrSetFlag | static_cast<AttrId>(m_sets.size());

    m_sets.push_back(std::move(new_set));
    return m_sets.back().m_id;
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::add_multi — batch attribute registration with optimal representation
// ────────────────────────────────────────────────────────────────────────────

std::vector<AttrId> AttrPool::add_multi(std::vector<AttrId> ids, bool create_set) {
    if (ids.empty())
        return {};

    // 1. Sort and deduplicate
    sort_and_unique(ids);

    // 2. Check for exact match in existing sets
    for (const auto& set : m_sets) {
        if (set.m_members == ids)
            return {set.m_id};
    }

    // 3. Find best matching set
    BestSetMatch best = find_best_set_match(ids, m_sets);

    // 4. Compute missing IDs (those not in the best set)
    std::vector<AttrId> missing;
    if (best.m_id != 0) {
        const auto& best_set = get_set(best.m_id);
        auto it = ids.begin();
        auto sit = best_set.m_members.begin();
        while (it != ids.end()) {
            if (sit != best_set.m_members.end() && *it == *sit) {
                ++it;
                ++sit;
            } else if (sit != best_set.m_members.end() && *sit < *it) {
                ++sit;
            } else {
                missing.push_back(*it);
                ++it;
            }
        }
    } else {
        missing = ids;
    }

    // 5. Build result
    if (create_set) {
        // Combine best set + missing into a new set
        std::vector<AttrId> combined;
        if (best.m_id != 0) {
            const auto& best_set = get_set(best.m_id);
            combined = best_set.m_members;
        }
        combined.insert(combined.end(), missing.begin(), missing.end());
        sort_and_unique(combined);

        AttrId new_set_id = add_set(std::move(combined));
        return {new_set_id};
    }

    // Return best match + missing separately
    std::vector<AttrId> result;
    if (best.m_id != 0)
        result.push_back(best.m_id);
    result.insert(result.end(), missing.begin(), missing.end());
    return result;
}

// ────────────────────────────────────────────────────────────────────────────
// TokenInfo::add_attrs — batch attribute addition with optimal representation
// ────────────────────────────────────────────────────────────────────────────

void TokenInfo::add_attrs(AttrPool& pool, std::vector<AttrId> ids, bool create_set) {
    auto resolved = pool.add_multi(std::move(ids), create_set);
    for (auto id : resolved) {
        m_attrs.push_back(id);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// TokenInfo::has_attr (by singleton AttrId, with set resolution)
// ────────────────────────────────────────────────────────────────────────────

bool TokenInfo::has_attr(const AttrPoolView& pool, AttrId target) const {
    // Check each attribute in this token, resolving sets to find matching singletons
    for (auto id : m_attrs) {
        auto resolved = pool.resolve(id);
        if (std::find(resolved.begin(), resolved.end(), target) != resolved.end())
            return true;
    }
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// TokenInfo::has_attr (by name)
// ────────────────────────────────────────────────────────────────────────────

bool TokenInfo::has_attr(const AttrPoolView& pool, std::string_view name) const {
    auto id = pool.lookup(name);
    if (!id.has_value())
        return false;
    return has_attr(pool, id.value());
}

// ────────────────────────────────────────────────────────────────────────────
// ModuleApi helpers — name storage
// ────────────────────────────────────────────────────────────────────────────

PackedName ModuleApi::append_name_data(std::string_view s) {
    auto offset = static_cast<uint32_t>(m_name_data.size());
    auto length = static_cast<uint8_t>(s.size());
    EXPECT(length == s.size()); // ensure no truncation (max 255 bytes)
    m_name_data.insert(m_name_data.end(), s.begin(), s.end());
    return PackedName(offset, length);
}

PackedName ModuleApi::append_param_data(const AttrParam& param) {
    std::string str;
    // All params are serialized as strings (kRange to_string works without SourceMap here)
    if (param.is_string()) {
        str = param.as_string();
    } else {
        // For MapperRange, we don't have a SourceMap here, so serialize as offset text
        const auto& r = std::get<MapperRange>(param.m_value);
        str = std::to_string(r.begin.offset()) + ".." + std::to_string(r.end.offset());
    }
    return append_name_data(str);
}

/// Parse a string fragment back to an AttrParam.
/// The fragment was serialized by append_param_data.
/// All parameter types (int, string, range) are stored as plain strings
/// and are restored as strings — the original type is not preserved.
static AttrParam parse_param_from_string(std::string_view s) {
    return AttrParam(std::string_view(s));
}

std::pair<uint32_t, uint8_t> ModuleApi::find_longest_prefix(std::string_view s) const {
    uint32_t best_idx = 0;
    uint8_t best_len = 0;

    for (uint32_t i = 0; i < m_prefixes.size(); ++i) {
        auto off = m_prefixes[i].offset();
        auto len = m_prefixes[i].length();
        if (len <= best_len)
            continue;
        if (static_cast<std::size_t>(len) > s.size())
            continue;
        if (std::string_view(m_name_data.data() + off, len) == s.substr(0, len)) {
            best_idx = i;
            best_len = len;
        }
    }

    return {best_idx, best_len};
}

std::string ModuleApi::get_full_name(const ModuleTokenRecord& rec) const {
    std::string result;
    if (rec.m_prefix_id != 0) {
        auto off = m_prefixes[rec.m_prefix_id].offset();
        auto len = m_prefixes[rec.m_prefix_id].length();
        result.append(m_name_data.data() + off, len);
        // Prefix is a substring of the full name; they are simply concatenated.
        // There is no implicit "::" — the prefix may be any shared substring
        // (e.g. "ns::get_name" for tokens "ns::get_name1" and "ns::get_name2").
        // If the token equals the prefix (empty suffix), the result is just the prefix.
    }
    result.append(m_name_data.data() + rec.m_name.offset(), rec.m_name.length());
    return result;
}

// ────────────────────────────────────────────────────────────────────────────
// AttrPool::export_attrs — create self-contained ModuleApi (does NOT modify input tokens)
// ────────────────────────────────────────────────────────────────────────────

auto AttrPool::export_attrs(std::span<TokenInfo*> tokens) -> std::unique_ptr<ModuleApi> {
    auto result = std::make_unique<ModuleApi>();

    // Add placeholder at index 0 (ID=0 is reserved as invalid)
    ModuleAttrRecord placeholder;
    placeholder.m_name = {};
    placeholder.m_params = {};
    result->m_attr_records.push_back(std::move(placeholder));

    // Map old AttrId → new AttrId (global across all tokens for deduplication)
    std::unordered_map<AttrId, AttrId> old_to_new;

    for (auto* token : tokens) {
        // Resolve all attrs in this token into singletons
        std::vector<AttrId> resolved_singletons;
        for (auto id : token->m_attrs) {
            auto singletons = resolve(id); // `this` is the source pool
            resolved_singletons.insert(resolved_singletons.end(), singletons.begin(), singletons.end());
        }

        // Remove duplicates locally
        sort_and_unique(resolved_singletons);

        // Register each singleton in the new ModuleApi (deduplicating via old_to_new)
        for (auto old_id : resolved_singletons) {
            // Use count/insert instead of try_emplace (workaround for libstdc++ 14 + clang incompatibility)
            if (old_to_new.count(old_id) != 0)
                continue; // already registered
            auto it = old_to_new.insert(std::make_pair(old_id, AttrId{0})).first;

            const Attr& attr = get(old_id); // `this` is the source pool

            // Intern name into ModuleApi's m_name_data
            ModuleAttrRecord rec;
            rec.m_name = result->append_name_data(attr.m_name);

            // Serialize each param as a string and store as PackedName
            rec.m_params.reserve(attr.m_default_params.size());
            for (const auto& p : attr.m_default_params) {
                rec.m_params.push_back(result->append_param_data(p));
            }

            AttrId new_id = static_cast<AttrId>(result->m_attr_records.size());
            result->m_attr_records.push_back(std::move(rec));

            // Register name → id lookup
            std::string_view interned_name = result->intern(attr.m_name);
            result->m_name_to_id.insert(std::make_pair(interned_name, new_id));

            it->second = new_id;
        }

        // Build new AttrId list for this token's record (using new ModuleApi AttrIds)
        std::vector<AttrId> new_ids;
        for (auto old_id : token->m_attrs) {
            auto singletons = resolve(old_id); // `this` is the source pool
            for (auto s : singletons) {
                auto it = old_to_new.find(s);
                EXPECT(it != old_to_new.end());
                new_ids.push_back(it->second);
            }
        }

        // Build the token record with prefix/suffix name storage
        ModuleTokenRecord token_rec;

        if (new_ids.empty()) {
            token_rec.m_attr = 0;
        } else if (new_ids.size() == 1) {
            token_rec.m_attr = new_ids[0];
        } else {
            sort_and_unique(new_ids);
            // Check for exact match in existing sets
            AttrId set_id = 0;
            for (const auto& existing_set : result->m_sets) {
                if (existing_set.m_members == new_ids) {
                    set_id = existing_set.m_id;
                    break;
                }
            }
            if (set_id == 0) {
                AttrSet new_set;
                new_set.m_members = std::move(new_ids);
                new_set.m_id = detail::kAttrSetFlag | static_cast<AttrId>(result->m_sets.size());
                result->m_sets.push_back(std::move(new_set));
                set_id = result->m_sets.back().m_id;
            }
            token_rec.m_attr = set_id;
        }

        // Split token name into prefix + suffix
        std::string_view full_name = token->text;
        auto [prefix_id, prefix_len] = result->find_longest_prefix(full_name);

        if (prefix_id != 0 && prefix_len > 0) {
            // Use existing prefix, store the suffix
            token_rec.m_prefix_id = prefix_id;
            token_rec.m_name = result->append_name_data(full_name.substr(prefix_len));

            // Also register the entire name as a new prefix candidate for future tokens
            result->m_prefixes.push_back(result->append_name_data(full_name));
        } else {
            // No matching prefix — store entire name as suffix, register as new prefix
            token_rec.m_name = result->append_name_data(full_name);

            // Register the entire name as a prefix candidate
            result->m_prefixes.push_back(token_rec.m_name);
        }

        result->m_tokens.push_back(std::move(token_rec));
    }

    return result;
}

// ────────────────────────────────────────────────────────────────────────────
// ModuleApi::get_token_name
// ────────────────────────────────────────────────────────────────────────────

std::string ModuleApi::get_token_name(std::size_t index) const {
    EXPECT(index < m_tokens.size());
    return get_full_name(m_tokens[index]);
}

// ────────────────────────────────────────────────────────────────────────────
// ModuleApi::create_token (by index)
// ────────────────────────────────────────────────────────────────────────────

std::shared_ptr<TokenInfo> ModuleApi::create_token(std::size_t index, AttrPool& pool) const {
    EXPECT(index < m_tokens.size());
    const auto& record = m_tokens[index];
    auto full_name = get_full_name(record);
    auto token = TokenInfo::make(ParserToken::Kind::Ident, std::move(full_name), MapperRange{});

    // Register each attribute in the pool
    std::vector<AttrId> resolved_attrs;
    if (record.m_attr != 0) {
        auto resolved = resolve(record.m_attr); // resolve sets within ModuleApi
        resolved_attrs.insert(resolved_attrs.end(), resolved.begin(), resolved.end());
    }

    for (auto attr_id : resolved_attrs) {
        const auto& rec = m_attr_records[attr_id & detail::kAttrIndexMask];
        std::string_view attr_name(m_name_data.data() + rec.m_name.offset(), rec.m_name.length());

        std::vector<AttrParam> params;
        params.reserve(rec.m_params.size());
        for (const auto& pn : rec.m_params) {
            std::string_view param_str(m_name_data.data() + pn.offset(), pn.length());
            params.push_back(parse_param_from_string(param_str));
        }

        AttrId new_id = pool.register_attr(attr_name, std::move(params));
        token->add_attr(new_id);
    }

    return token;
}

// ────────────────────────────────────────────────────────────────────────────
// ModuleApi::lookup
// ────────────────────────────────────────────────────────────────────────────

std::optional<AttrId> ModuleApi::lookup(std::string_view name) const noexcept {
    auto it = m_name_to_id.find(std::string(name));
    if (it != m_name_to_id.end())
        return it->second;
    return std::nullopt;
}

// ────────────────────────────────────────────────────────────────────────────
// ModuleApi::has_attr (by name)
// ────────────────────────────────────────────────────────────────────────────

bool ModuleApi::has_attr(std::string_view name) const noexcept {
    return m_name_to_id.find(std::string(name)) != m_name_to_id.end();
}

// ────────────────────────────────────────────────────────────────────────────
// ModuleApi::get_name
// ────────────────────────────────────────────────────────────────────────────

std::string_view ModuleApi::get_name(AttrId id) const {
    auto idx = id & detail::kAttrIndexMask;
    EXPECT(idx < m_attr_records.size());
    const auto& rec = m_attr_records[idx];
    return std::string_view(m_name_data.data() + rec.m_name.offset(), rec.m_name.length());
}

// ────────────────────────────────────────────────────────────────────────────
// ModuleApi::resolve
// ────────────────────────────────────────────────────────────────────────────

std::vector<AttrId> ModuleApi::resolve(AttrId id) const {
    return detail::resolve_attr_set(id, m_sets);
}

// ══════════════════════════════════════════════════════════════
//  Константы формата msgpack для ModuleApi
// ══════════════════════════════════════════════════════════════

namespace {
enum : uint8_t {
    kApiFieldNameData = 0,    // binary — сырые байты m_name_data
    kApiFieldAttrRecords = 1, // array of {name PackedName, params [PackedName]}
    kApiFieldSets = 2,        // array of [member_id, ...]
    kApiFieldPrefixes = 3,    // array of {offset, length}
    kApiFieldTokens = 4,      // array of {prefix_id, name PackedName, [attr_ids]}
    kApiFieldCount = 5,
};

/// Упаковать PackedName как [offset: uint32, length: uint8]
void writePackedName(MsgpackWriter& wr, PackedName pn) {
    wr.packArray(2);
    wr.packUint32(pn.offset());
    wr.packUint8(static_cast<uint8_t>(pn.length()));
}

/// Прочитать PackedName из msgpack_array с двумя элементами [offset, length]
PackedName readPackedName(const msgpack_object& arr) {
    EXPECT(arr.type == MSGPACK_OBJECT_ARRAY);
    EXPECT(arr.via.array.size >= 2);
    msgpack_object* fields = arr.via.array.ptr;
    EXPECT(fields[0].type == MSGPACK_OBJECT_POSITIVE_INTEGER);
    EXPECT(fields[1].type == MSGPACK_OBJECT_POSITIVE_INTEGER);
    return PackedName(static_cast<uint32_t>(fields[0].via.u64), static_cast<uint32_t>(fields[1].via.u64));
}

} // anonymous namespace

// ══════════════════════════════════════════════════════════════
//         ModuleApi::packToMsgpack
// ══════════════════════════════════════════════════════════════

std::vector<char> ModuleApi::packToMsgpack() const {
    MsgpackWriter wr;
    wr.packArray(kApiFieldCount);

    // [0] name_data: binary
    wr.packString(std::string_view(m_name_data.data(), m_name_data.size()));

    // [1] attr_records: array of {name PackedName, params [PackedName]}
    wr.packArray(m_attr_records.size());
    for (const auto& rec : m_attr_records) {
        wr.packArray(2);
        writePackedName(wr, rec.m_name);
        // params array
        wr.packArray(rec.m_params.size());
        for (const auto& pn : rec.m_params)
            writePackedName(wr, pn);
    }

    // [2] sets: array of [member_id, ...]
    wr.packArray(m_sets.size());
    for (const auto& set : m_sets) {
        wr.packArray(set.m_members.size());
        for (auto id : set.m_members)
            wr.packUint32(id);
    }

    // [3] prefixes: array of {offset, length}
    wr.packArray(m_prefixes.size());
    for (const auto& pn : m_prefixes)
        writePackedName(wr, pn);

    // [4] tokens: array of {prefix_id, name PackedName, attr_id}
    wr.packArray(m_tokens.size());
    for (const auto& tok : m_tokens) {
        wr.packArray(3);
        wr.packUint32(tok.m_prefix_id);
        writePackedName(wr, tok.m_name);
        wr.packUint32(tok.m_attr);
    }

    // ══════════════════════════════════════════════════════════
    //  Сжатие: msgpack → zlib (compress2) → [orig_size][compressed][checksum]
    // ══════════════════════════════════════════════════════════

    msgpack_sbuffer sbuf = std::move(wr).take_sbuf();
    uLong orig_size = static_cast<uLong>(sbuf.size);
    const auto* raw = reinterpret_cast<const unsigned char*>(sbuf.data);

    // Сжимаем через zlib (deflate)
    uLong compressed_capacity = compressBound(orig_size);
    auto compressed = std::make_unique<unsigned char[]>(compressed_capacity);
    uLong compressed_size = compressed_capacity;
    if (compress2(compressed.get(), &compressed_size, raw, orig_size, Z_DEFAULT_COMPRESSION) != Z_OK) {
        msgpack_sbuffer_destroy(&sbuf);
        FAULT("ModuleApi::packToMsgpack: zlib compress failed");
    }

    msgpack_sbuffer_destroy(&sbuf);

    // Формируем результат: [orig_size_LE4][compressed][checksum8]
    std::vector<char> result;
    result.reserve(4 + compressed_size + 8);

    // orig_size (4 байта LE)
    for (int i = 0; i < 4; ++i)
        result.push_back(static_cast<char>((orig_size >> (i * 8)) & 0xFF));

    // compressed data
    result.insert(result.end(), reinterpret_cast<const char*>(compressed.get()), reinterpret_cast<const char*>(compressed.get()) + compressed_size);

    // checksum от [orig_size_LE4 + compressed]
    auto checksumArr = llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(result.data()), result.size());
    auto checksumRes = llvm::MD5::hash(checksumArr);
    uint64_t checksum = 0;
    for (int i = 0; i < 8; ++i)
        checksum |= static_cast<uint64_t>(checksumRes[i]) << (i * 8);
    for (int i = 0; i < 8; ++i)
        result.push_back(static_cast<char>((checksum >> (i * 8)) & 0xFF));

    return result;
}

// ══════════════════════════════════════════════════════════════
//         ModuleApi::fromMsgpack
// ══════════════════════════════════════════════════════════════

std::unique_ptr<ModuleApi> ModuleApi::fromMsgpack(const uint8_t* data, size_t size) {
    // Минимальный размер: 4 (orig_size) + 1 (compressed min) + 8 (checksum) = 13
    if (size < 13)
        return nullptr;

    // ── Проверка checksum от [orig_size_LE4 + compressed] ──
    size_t payload_size = size - 8;
    uint64_t stored_checksum = 0;
    for (int i = 0; i < 8; ++i)
        stored_checksum |= static_cast<uint64_t>(data[payload_size + i]) << (i * 8);

    auto checksumArr = llvm::ArrayRef<uint8_t>(data, payload_size);
    auto checksumRes = llvm::MD5::hash(checksumArr);
    uint64_t computed_checksum = 0;
    for (int i = 0; i < 8; ++i)
        computed_checksum |= static_cast<uint64_t>(checksumRes[i]) << (i * 8);
    if (computed_checksum != stored_checksum)
        return nullptr;

    // ── Читаем original_size (4 байта LE) ──
    uLong orig_size = 0;
    for (int i = 0; i < 4; ++i)
        orig_size |= static_cast<uLong>(static_cast<unsigned char>(data[i])) << (i * 8);
    if (orig_size == 0 || orig_size > 1024 * 1024 * 1024) // reasonable limit: 1GB
        return nullptr;

    // ── Распаковываем через zlib (inflate) ──
    uLong compressed_size = static_cast<uLong>(payload_size - 4);
    const auto* compressed_data = data + 4;

    auto decompressed = std::make_unique<unsigned char[]>(orig_size);
    uLong decompressed_size = orig_size;
    int zret = uncompress(decompressed.get(), &decompressed_size, compressed_data, compressed_size);
    if (zret != Z_OK)
        return nullptr;
    if (decompressed_size != orig_size)
        return nullptr;

    // ── Парсим msgpack из распакованных данных ──
    MsgpackReader reader(decompressed.get(), decompressed_size);
    if (!reader.is_valid())
        return nullptr;

    return unpackFromMsgpackObject(reader.root());
}

// ══════════════════════════════════════════════════════════════
//         ModuleApi::unpackFromMsgpackObject
// ══════════════════════════════════════════════════════════════

std::unique_ptr<ModuleApi> ModuleApi::unpackFromMsgpackObject(const msgpack_object& obj) {
    if (obj.type != MSGPACK_OBJECT_ARRAY)
        return nullptr;

    uint32_t array_size = obj.via.array.size;
    if (array_size < kApiFieldCount)
        return nullptr;

    // Создаём пустой ModuleApi
    auto result = std::make_unique<ModuleApi>();
    msgpack_object* fields = obj.via.array.ptr;

    // ── [0] name_data: binary ──
    {
        msgpack_object& nd = fields[kApiFieldNameData];
        if (nd.type != MSGPACK_OBJECT_STR)
            return nullptr;
        std::string_view nd_str(nd.via.str.ptr, nd.via.str.size);
        result->m_name_data.assign(nd_str.begin(), nd_str.end());
    }

    // Вспомогательная лямбда для валидации PackedName в пределах name_data
    auto validate_pn = [&](PackedName pn) -> bool {
        auto off = pn.offset();
        auto len = pn.length();
        return off + len <= result->m_name_data.size();
    };

    // ── [1] attr_records ──
    {
        msgpack_object& ar = fields[kApiFieldAttrRecords];
        if (ar.type != MSGPACK_OBJECT_ARRAY)
            return nullptr;

        for (uint32_t i = 0; i < ar.via.array.size; ++i) {
            // The first record in the exported data is always the placeholder (index 0).
            // After processing all exported records, result->m_attr_records will
            // have the correct indices.
            msgpack_object attr_entry = ar.via.array.ptr[i];
            if (attr_entry.type != MSGPACK_OBJECT_ARRAY || attr_entry.via.array.size < 2)
                return nullptr;

            msgpack_object* attr_fields = attr_entry.via.array.ptr;

            // name PackedName
            PackedName name_pn = readPackedName(attr_fields[0]);
            if (!validate_pn(name_pn))
                return nullptr;

            // Параметры
            std::vector<PackedName> params;
            if (attr_fields[1].type == MSGPACK_OBJECT_ARRAY) {
                for (uint32_t j = 0; j < attr_fields[1].via.array.size; ++j) {
                    PackedName pn = readPackedName(attr_fields[1].via.array.ptr[j]);
                    if (!validate_pn(pn))
                        return nullptr;
                    params.push_back(pn);
                }
            }

            // Создаём запись
            ModuleAttrRecord rec;
            rec.m_name = name_pn;
            rec.m_params = std::move(params);

            AttrId new_id = static_cast<AttrId>(result->m_attr_records.size());
            result->m_attr_records.push_back(std::move(rec));

            // Intern name и заполняем m_name_to_id
            std::string_view attr_name(result->m_name_data.data() + name_pn.offset(), name_pn.length());
            std::string_view interned = result->intern(attr_name);
            result->m_name_to_id.insert(std::make_pair(interned, new_id));
        }
    }

    // ── [2] sets ──
    {
        msgpack_object& sets = fields[kApiFieldSets];
        if (sets.type != MSGPACK_OBJECT_ARRAY)
            return nullptr;

        for (uint32_t i = 0; i < sets.via.array.size; ++i) {
            msgpack_object set_arr = sets.via.array.ptr[i];
            if (set_arr.type != MSGPACK_OBJECT_ARRAY)
                return nullptr;

            AttrSet as;
            as.m_id = detail::kAttrSetFlag | static_cast<AttrId>(result->m_sets.size());
            as.m_members.reserve(set_arr.via.array.size);
            for (uint32_t j = 0; j < set_arr.via.array.size; ++j) {
                msgpack_object member = set_arr.via.array.ptr[j];
                if (member.type != MSGPACK_OBJECT_POSITIVE_INTEGER)
                    return nullptr;
                as.m_members.push_back(static_cast<AttrId>(member.via.u64));
            }
            result->m_sets.push_back(std::move(as));
        }
    }

    // ── [3] prefixes ──
    {
        msgpack_object& prefixes = fields[kApiFieldPrefixes];
        if (prefixes.type != MSGPACK_OBJECT_ARRAY)
            return nullptr;

        result->m_prefixes.clear();
        for (uint32_t i = 0; i < prefixes.via.array.size; ++i) {
            PackedName pn = readPackedName(prefixes.via.array.ptr[i]);
            if (!validate_pn(pn))
                return nullptr;
            result->m_prefixes.push_back(pn);
        }

        // Убеждаемся, что первый префикс — пустой (индекс 0 = пустой префикс)
        if (result->m_prefixes.empty() || result->m_prefixes[0] != PackedName{0, 0})
            return nullptr;
    }

    // ── [4] tokens ──
    {
        msgpack_object& tokens = fields[kApiFieldTokens];
        if (tokens.type != MSGPACK_OBJECT_ARRAY)
            return nullptr;

        for (uint32_t i = 0; i < tokens.via.array.size; ++i) {
            msgpack_object tok_entry = tokens.via.array.ptr[i];
            if (tok_entry.type != MSGPACK_OBJECT_ARRAY || tok_entry.via.array.size < 3)
                return nullptr;

            msgpack_object* tok_fields = tok_entry.via.array.ptr;

            // prefix_id
            if (tok_fields[0].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
                return nullptr;
            uint32_t prefix_id = static_cast<uint32_t>(tok_fields[0].via.u64);
            if (prefix_id >= result->m_prefixes.size())
                return nullptr;

            // name PackedName
            PackedName name_pn = readPackedName(tok_fields[1]);
            if (!validate_pn(name_pn))
                return nullptr;

            // attr_id
            if (tok_fields[2].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
                return nullptr;
            AttrId attr_id = static_cast<AttrId>(tok_fields[2].via.u64);

            ModuleTokenRecord trec;
            trec.m_prefix_id = prefix_id;
            trec.m_name = name_pn;
            trec.m_attr = attr_id;
            result->m_tokens.push_back(std::move(trec));
        }
    }

    return result;
}

// ══════════════════════════════════════════════════════════════
//         ModuleApi::save_to_file
// ══════════════════════════════════════════════════════════════

bool ModuleApi::save_to_file(const std::string& path) const {
    auto packed = packToMsgpack();
    if (packed.empty())
        return false;

    return utils::FileIO::write(path, packed);
}

// ══════════════════════════════════════════════════════════════
//         ModuleApi::load_from_file
// ══════════════════════════════════════════════════════════════

std::unique_ptr<ModuleApi> ModuleApi::load_from_file(const std::string& path) {
    auto buf = utils::FileIO::read<std::vector<char>>(path);
    if (!buf)
        return nullptr;

    return fromMsgpack(reinterpret_cast<const uint8_t*>(buf->data()), buf->size());
}

} // namespace trust
