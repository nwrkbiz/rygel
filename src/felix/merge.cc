// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#include "src/core/libcc/libcc.hh"
#include "merge.hh"

namespace RG {

static MergeMode FindDefaultMergeMode(const char *filename)
{
    Span<const char> extension = GetPathExtension(filename);

    if (extension == ".css") {
        return MergeMode::CSS;
    } else if (extension == ".js") {
        return MergeMode::JS;
    } else {
        return MergeMode::Naive;
    }
}

bool LoadMergeRules(const char *filename, unsigned int flags, MergeRuleSet *out_set)
{
    RG_DEFER_NC(out_guard, len = out_set->rules.len) { out_set->rules.RemoveFrom(len); };

    StreamReader st(filename);
    if (!st.IsValid())
        return false;

    IniParser ini(&st);
    ini.PushLogFilter();
    RG_DEFER { PopLogFilter(); };

    bool valid = true;
    {
        IniProperty prop;
        while (ini.Next(&prop)) {
            if (!prop.section.len) {
                LogError("Property is outside section");
                return false;
            }

            MergeRule *rule = out_set->rules.AppendDefault();
            rule->name = DuplicateString(prop.section, &out_set->str_alloc).ptr;
            rule->merge_mode = FindDefaultMergeMode(rule->name);

            bool changed_merge_mode = false;
            do {
                if (prop.key == "CompressionType") {
                    if (OptionToEnum(CompressionTypeNames, prop.value, &rule->compression_type)) {
                        rule->override_compression = true;
                    } else {
                        LogError("Unknown compression type '%1'", prop.value);
                        valid = false;
                    }
                } else if (prop.key == "MergeMode") {
                    if (prop.value == "Naive") {
                        rule->merge_mode = MergeMode::Naive;
                    } else if (prop.value == "CSS") {
                        rule->merge_mode = MergeMode::CSS;
                    } else if (prop.value == "JS") {
                        rule->merge_mode = MergeMode::JS;
                    } else {
                        LogError("Invalid MergeMode value '%1'", prop.value);
                        valid = false;
                    }

                    changed_merge_mode = true;
                } else if (prop.key == "SourceMap") {
                    if (prop.value == "None") {
                        rule->source_map_type = SourceMapType::None;
                    } else if (prop.value == "JSv3") {
                        rule->source_map_type = SourceMapType::JSv3;
                    } else {
                        LogError("Invalid SourceMap value '%1'", prop.value);
                        valid = false;
                    }

                    if (!(flags & (int)MergeFlag::SourceMap)) {
                        rule->source_map_type = SourceMapType::None;
                    }
                } else if (prop.key == "TransformCommand") {
                    if (flags & (int)MergeFlag::RunTransform) {
                        rule->transform_cmd = DuplicateString(prop.value, &out_set->str_alloc).ptr;
                    }
                } else if (prop.key == "TransformCommand_Win32") {
#ifdef _WIN32
                    if (flags & (int)MergeFlag::RunTransform) {
                        rule->transform_cmd = DuplicateString(prop.value, &out_set->str_alloc).ptr;
                    }
#endif
                } else if (prop.key == "TransformCommand_POSIX") {
#ifndef _WIN32
                    if (flags & (int)MergeFlag::RunTransform) {
                        rule->transform_cmd = DuplicateString(prop.value, &out_set->str_alloc).ptr;
                    }
#endif
                } else if (prop.key == "Include") {
                    while (prop.value.len) {
                        Span<const char> part = TrimStr(SplitStrAny(prop.value, " ,", &prop.value));

                        if (part.len) {
                            const char *copy = DuplicateString(part, &out_set->str_alloc).ptr;
                            rule->include.Append(copy);
                        }
                    }
                } else if (prop.key == "Exclude") {
                    while (prop.value.len) {
                        Span<const char> part = TrimStr(SplitStrAny(prop.value, " ,", &prop.value));

                        if (part.len) {
                            const char *copy = DuplicateString(part, &out_set->str_alloc).ptr;
                            rule->exclude.Append(copy);
                        }
                    }
                } else {
                    LogError("Unknown attribute '%1'", prop.key);
                    valid = false;
                }
            } while (ini.NextInSection(&prop));

            if (rule->merge_mode == MergeMode::Naive && !changed_merge_mode) {
                LogError("Using naive merge method for '%1'", filename);
            }
        }
    }
    if (!ini.IsValid() || !valid)
        return false;

    out_guard.Disable();
    return true;
}

static void FindMergeRules(Span<const MergeRule> rules, const char *filename,
                           HeapArray<const MergeRule *> *out_rules)
{
    const auto test_pattern = [&](const char *pattern) { return MatchPathSpec(filename, pattern); };

    for (const MergeRule &rule: rules) {
        if (std::any_of(rule.include.begin(), rule.include.end(), test_pattern) &&
                !std::any_of(rule.exclude.begin(), rule.exclude.end(), test_pattern))
            out_rules->Append(&rule);
    }
}

static void InitSourceMergeData(PackSourceInfo *src, MergeMode merge_mode, Allocator *alloc)
{
    RG_ASSERT(alloc);

    switch (merge_mode) {
        case MergeMode::Naive: {
            src->prefix = "";
            src->suffix = "";
        } break;
        case MergeMode::CSS: {
            src->prefix = Fmt(alloc, "/* %1\n   ------------------------------------ */\n\n", src->filename).ptr;
            src->suffix = "\n";
        } break;
        case MergeMode::JS: {
            src->prefix = Fmt(alloc, "// %1\n// ------------------------------------\n\n", src->filename).ptr;
            src->suffix = "\n";
        } break;
    }
}

static const char *StripDirectoryComponents(Span<const char> filename, int strip_count)
{
    const char *name = filename.ptr;
    for (int i = 0; filename.len && i <= strip_count; i++) {
        name = SplitStrAny(filename, RG_PATH_SEPARATORS, &filename).ptr;
    }

    return name;
}

void ResolveAssets(Span<const char *const> filenames, int strip_count, Span<const MergeRule> rules,
                   CompressionType compression_type, PackAssetSet *out_set)
{
    HashMap<const void *, Size> merge_map;

    // Reuse for performance
    HeapArray<const MergeRule *> file_rules;

    for (const char *filename: filenames) {
        PackSourceInfo src = {};
        src.filename = filename;
        src.name = StripDirectoryComponents(filename, strip_count);

        file_rules.RemoveFrom(0);
        FindMergeRules(rules, filename, &file_rules);

        bool include_raw_file = !file_rules.len;

        for (const MergeRule *rule: file_rules) {
            InitSourceMergeData(&src, rule->merge_mode, &out_set->str_alloc);

            Size asset_idx = merge_map.FindValue(rule, -1);
            if (asset_idx >= 0) {
                PackAssetInfo *asset = &out_set->assets[asset_idx];
                asset->sources.Append(src);

                include_raw_file |= (asset->source_map_type != SourceMapType::None);
            } else {
                merge_map.Set(rule, out_set->assets.len);

                PackAssetInfo asset = {};
                asset.name = rule->name;
                asset.compression_type = rule->override_compression ? rule->compression_type : compression_type;
                if (rule->source_map_type != SourceMapType::None) {
                    if (!rule->transform_cmd) {
                        asset.source_map_type = rule->source_map_type;
                        asset.source_map_name = Fmt(&out_set->str_alloc, "%1.map", rule->name).ptr;
                    } else {
                        LogError("Ignoring source map for transformed asset '%1'", asset.name);
                    }
                }
                if (rule->transform_cmd) {
                    asset.transform_cmd = DuplicateString(rule->transform_cmd, &out_set->str_alloc).ptr;
                }
                out_set->assets.Append(asset)->sources.Append(src);

                include_raw_file |= (asset.source_map_type != SourceMapType::None);
            }
        }

        if (include_raw_file) {
            InitSourceMergeData(&src, MergeMode::Naive, &out_set->str_alloc);

            PackAssetInfo asset = {};
            asset.name = src.name;
            asset.compression_type = compression_type;
            out_set->assets.Append(asset)->sources.Append(src);
        }
    }
}

}
