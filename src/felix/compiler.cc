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
#include "compiler.hh"
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

namespace RG {

static bool SplitPrefixSuffix(const char *binary, const char *needle,
                              Span<const char> *out_prefix, Span<const char> *out_suffix,
                              Span<const char> *out_version)
{
    const char *ptr = strstr(binary, needle);
    if (!ptr) {
        LogError("Compiler binary path must contain '%1'", needle);
        return false;
    }

    *out_prefix = MakeSpan(binary, ptr - binary);
    *out_suffix = ptr + strlen(needle);

    if (out_suffix->ptr[0] == '-' && std::all_of(out_suffix->ptr + 1, out_suffix->end(), IsAsciiDigit)) {
        *out_version = *out_suffix;
    } else {
        *out_version = {};
    }

    return true;
}

static void AddEnvironmentFlags(Span<const char *const> names, HeapArray<char> *out_buf)
{
    for (const char *name: names) {
        const char *flags = getenv(name);

        if (flags && flags[0]) {
            Fmt(out_buf, " %1", flags);
        }
    }
}

static void MakePackCommand(Span<const char *const> pack_filenames, bool optimize,
                            bool use_arrays, const char *pack_options, const char *dest_filename,
                            Allocator *alloc, Command *out_cmd)
{
    RG_ASSERT(alloc);

    HeapArray<char> buf(alloc);

    Fmt(&buf, "\"%1\" pack -O \"%2\"", GetApplicationExecutable(), dest_filename);

    Fmt(&buf, optimize ? " -mRunTransform" : " -mSourceMap");
    Fmt(&buf, use_arrays ? "" : " -fUseLiterals");

    if (pack_options) {
        Fmt(&buf, " %1", pack_options);
    }
    for (const char *pack_filename: pack_filenames) {
        Fmt(&buf, " \"%1\"", pack_filename);
    }

    out_cmd->cache_len = buf.len;
    out_cmd->cmd_line = buf.TrimAndLeak(1);
}

static bool ReadCommandOutput(const char *cmd_line, Size max_len, HeapArray<char> *out_output)
{
    int exit_code;
    if (!ExecuteCommandLine(cmd_line, {}, Kilobytes(4), out_output, &exit_code))
        return false;
    if (exit_code) {
        LogDebug("Command '%1 failed (exit code: %2)", cmd_line, exit_code);
        return false;
    }

    return true;
}

static int ParseVersion(const char *cmd, Span<const char> output, const char *marker)
{
    Span<const char> remain = output;

    while (remain.len) {
        Span<const char> token = SplitStr(remain, ' ', &remain);

        if (token == marker) {
            int major = 0;
            int minor = 0;
            int patch = 0;

            if (!ParseInt(remain, &major, 0, &remain)) {
                LogError("Unexpected version format returned by '%1'", cmd);
                return -1;
            }
            if(remain[0] == '.') {
                remain = remain.Take(1, remain.len - 1);

                if (!ParseInt(remain, &minor, 0, &remain)) {
                    LogError("Unexpected version format returned by '%1'", cmd);
                    return -1;
                }
            }
            if(remain[0] == '.') {
                remain = remain.Take(1, remain.len - 1);

                if (!ParseInt(remain, &patch, 0, &remain)) {
                    LogError("Unexpected version format returned by '%1'", cmd);
                    return -1;
                }
            }

            int version = major * 10000 + minor * 100 + patch;
            return version;
        }
    }

    return -1;
}

static bool IdentifyCompiler(const char *bin, const char *needle)
{
    bin = SplitStrReverseAny(bin, RG_PATH_SEPARATORS).ptr;

    const char *ptr = strstr(bin, needle);
    Size len = (Size)strlen(needle);

    if (!ptr)
        return false;
    if (ptr != bin && !strchr("_-.", ptr[-1]))
        return false;
    if (ptr[len] && !strchr("_-.", ptr[len]))
        return false;

    return true;
}

static bool DetectCcache() {
    static bool detected = false;
    static bool init = false;

    if (!init) {
        detected = FindExecutableInPath("ccache");

        if (detected) {
            SetEnvironmentVar("CCACHE_DEPEND", "1");
            SetEnvironmentVar("CCACHE_SLOPPINESS", "pch_defines,time_macros,include_file_ctime,include_file_mtime");
        }
    }

    return detected;
}

class ClangCompiler final: public Compiler {
    const char *cc;
    const char *cxx;
    const char *rc;
    const char *ld;

    int clang_ver = 0;
    int lld_ver = 0;

    BlockAllocator str_alloc;

public:
    ClangCompiler(HostPlatform host) : Compiler(host, "Clang") {}

    static std::unique_ptr<const Compiler> Create(HostPlatform host, const char *cc, const char *ld)
    {
        std::unique_ptr<ClangCompiler> compiler = std::make_unique<ClangCompiler>(host);

        // Prefer LLD
        if (!ld && FindExecutableInPath("ld.lld")) {
            ld = "lld";
        }

        // Find executables
        {
            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cc, "clang", &prefix, &suffix, &version))
                return nullptr;

            compiler->cc = DuplicateString(cc, &compiler->str_alloc).ptr;
            compiler->cxx = Fmt(&compiler->str_alloc, "%1clang++%2", prefix, suffix).ptr;
            compiler->rc = Fmt(&compiler->str_alloc, "%1llvm-rc%2", prefix, version).ptr;
            if (ld) {
                compiler->ld = ld;
            } else if (suffix.len) {
                compiler->ld = Fmt(&compiler->str_alloc, "%1lld%2", prefix, suffix).ptr;
            } else {
                compiler->ld = nullptr;
            }
        }

        Async async;

        // Determine Clang version
        async.Run([&]() {
            char cmd[2048];
            Fmt(cmd, "\"%1\" --version", compiler->cc);

            HeapArray<char> output;
            if (ReadCommandOutput(cmd, Kilobytes(4), &output)) {
                compiler->clang_ver = ParseVersion(cmd, output, "version");
            }

            return true;
        });

        // Determine LLD version
        async.Run([&]() {
            if (compiler->ld && IdentifyCompiler(compiler->ld, "lld")) {
                char cmd[4096];
                if (PathIsAbsolute(compiler->ld)) {
                    Fmt(cmd, "\"%1\" --version", compiler->ld);
                } else {
#ifdef _WIN32
                    Fmt(cmd, "\"%1-link\" --version", compiler->ld);
#else
                    Fmt(cmd, "\"ld.%1\" --version", compiler->ld);
#endif
                }

                HeapArray<char> output;
                if (ReadCommandOutput(cmd, Kilobytes(4), &output)) {
                    compiler->lld_ver = ParseVersion(cmd, output, "LLD");
                }
            }

            return true;
        });

        async.Sync();

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        if (DetectCcache()) {
            supported |= (int)CompileFeature::Ccache;
        }
        supported |= (int)CompileFeature::HotAssets;
        supported |= (int)CompileFeature::PCH;
        supported |= (int)CompileFeature::DebugInfo;
        supported |= (int)CompileFeature::StaticLink;
        supported |= (int)CompileFeature::ASan;
        supported |= (int)CompileFeature::UBSan;
        supported |= (int)CompileFeature::LTO;
        supported |= (int)CompileFeature::ZeroInit;
        if (host != HostPlatform::OpenBSD) {
            supported |= (int)CompileFeature::CFI; // LTO only
        }
        if (host != HostPlatform::Windows) {
            supported |= (int)CompileFeature::TSan;
            supported |= (int)CompileFeature::ShuffleCode; // Requires lld version >= 11
        }
        if (host == HostPlatform::Linux) {
            supported |= (int)CompileFeature::SafeStack;
        }
        supported |= (int)CompileFeature::Cxx17;
        if (host == HostPlatform::Windows) {
            supported |= (int)CompileFeature::NoConsole;
        }
        supported |= (int)CompileFeature::SSE41;
        supported |= (int)CompileFeature::SSE42;
        supported |= (int)CompileFeature::AVX2;
        supported |= (int)CompileFeature::AVX512;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }
        if ((features & (int)CompileFeature::ASan) && (features & (int)CompileFeature::TSan)) {
            LogError("Cannot use ASan and TSan at the same time");
            return false;
        }
        if (!(features & (int)CompileFeature::LTO) && (features & (int)CompileFeature::CFI)) {
            LogError("Clang CFI feature requires LTO compilation");
            return false;
        }
        if (lld_ver < 110000 && (features & (int)CompileFeature::ShuffleCode)) {
            LogError("ShuffleCode requires LLD >= 11, try --host option (e.g. --host=,clang-11,lld-11)");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override
    {
        const char *ext = (host == HostPlatform::Windows) ? ".obj" : ".o";
        return ext;
    }
    const char *GetLinkExtension() const override
    {
        const char *ext = (host == HostPlatform::Windows) ? ".exe" : "";
        return ext;
    }
    const char *GetPostExtension() const override { return nullptr; }

    bool GetCore(Span<const char *const>, Allocator *, HeapArray<const char *> *,
                 HeapArray<const char *> *, const char **) const override { return true; }

    void MakePackCommand(Span<const char *const> pack_filenames, bool optimize,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        RG::MakePackCommand(pack_filenames, optimize, false, pack_options,
                            dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type, bool warnings,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        MakeObjectCommand(pch_filename, src_type, warnings, nullptr, definitions, include_directories,
                          include_files, features, env_flags, nullptr, alloc, out_cmd);
    }

    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *cache_filename = Fmt(alloc, "%1.gch", pch_filename).ptr;
        return cache_filename;
    }
    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, bool warnings,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> include_files,
                           uint32_t features, bool env_flags, const char *dest_filename,
                           Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        if (features & (int)CompileFeature::Ccache) {
            Fmt(&buf, "ccache ");
        }

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" -std=gnu11", cc); } break;
            case SourceType::CXX: {
                const char *std = (features & (int)CompileFeature::Cxx17) ? "17" : "2a";
                Fmt(&buf, "\"%1\" -std=gnu++%2", cxx, std);
            } break;
        }
        if (dest_filename) {
            Fmt(&buf, " -o \"%1\"", dest_filename);
        } else {
            switch (src_type) {
                case SourceType::C: { Fmt(&buf, " -x c-header -Xclang -fno-pch-timestamp"); } break;
                case SourceType::CXX: { Fmt(&buf, " -x c++-header -Xclang -fno-pch-timestamp"); } break;
            }
        }
        Fmt(&buf, " -MD -MF \"%1.d\"", dest_filename ? dest_filename : src_filename);
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " -I. -fvisibility=hidden -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks");
        if (clang_ver >= 120000) {
            Fmt(&buf, " -fno-finite-loops");
        }
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " -O2 -DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " -Os -DNDEBUG");
        } else {
            Fmt(&buf, " -O0 -ftrapv -fno-omit-frame-pointer");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto");
        }
        if (warnings) {
            Fmt(&buf, " -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-unknown-warning-option");
        } else {
            Fmt(&buf, " -Wno-everything");
        }
        if (features & (int)CompileFeature::HotAssets) {
            Fmt(&buf, " -DFELIX_HOT_ASSETS");
        }
#if defined(__x86_64__)
        if (features & (int)CompileFeature::SSE41) {
            Fmt(&buf, " -msse4.1");
        }
        if (features & (int)CompileFeature::SSE42) {
            Fmt(&buf, " -msse4.2");
        }
        if (features & (int)CompileFeature::AVX2) {
            Fmt(&buf, " -mavx2");
        }
        if (features & (int)CompileFeature::AVX512) {
            Fmt(&buf, " -mavx512f -mavx512vl");
        }
#elif defined(__i386__)
        Fmt(&buf, " -msse2");
#endif

        // Platform flags
        switch (host) {
            case HostPlatform::Windows: {
                Fmt(&buf, " -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE"
                          " -D_MT -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_VC_NODEFAULTLIB"
                          " -Wno-unknown-pragmas -Wno-deprecated-declarations");
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -pthread -fPIC");
                if (clang_ver >= 110000) {
                    Fmt(&buf, " -fno-semantic-interposition");
                }
            } break;

            default: {
                Fmt(&buf, " -D_FILE_OFFSET_BITS=64 -pthread -fPIC");
                if (clang_ver >= 110000) {
                    Fmt(&buf, " -fno-semantic-interposition");
                }
                if (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) {
                    Fmt(&buf, " -D_FORTIFY_SOURCE=2");
                }
            } break;
        }

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " -g");
        }
        if (host == HostPlatform::Windows) {
            if (features & (int)CompileFeature::StaticLink) {
                if (src_type == SourceType::CXX) {
                    Fmt(&buf, " -Xclang -flto-visibility-public-std -D_SILENCE_CLANG_CONCEPTS_MESSAGE");
                }
            } else {
                Fmt(&buf, " -D_DLL");
            }
        }
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        Fmt(&buf, " -fstack-protector-strong --param ssp-buffer-size=4");
        if (host == HostPlatform::Linux && (clang_ver >= 110000)) {
            Fmt(&buf, " -fstack-clash-protection");
        }
        if (features & (int)CompileFeature::SafeStack) {
            Fmt(&buf, " -fsanitize=safe-stack");
        }
        if (features & (int)CompileFeature::ZeroInit) {
            Fmt(&buf, " -ftrivial-auto-var-init=zero -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang");
        }
        if (features & (int)CompileFeature::CFI) {
            RG_ASSERT(features & (int)CompileFeature::LTO);

            Fmt(&buf, " -fsanitize=cfi");
            if (src_type == SourceType::C) {
                // There's too much pointer type fuckery going on in C
                // to not take this precaution. Without it, SQLite3 crashes.
                Fmt(&buf, " -fsanitize-cfi-icall-generalize-pointers");
            }

            if (clang_ver >= 140000) {
                Fmt(&buf, " -cfprotection=branch");
            }
        }
        if (features & (int)CompileFeature::ShuffleCode) {
            Fmt(&buf, " -ffunction-sections -fdata-sections");
        }

        // Sources and definitions
        Fmt(&buf, " -DFELIX -c \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " -include \"%1\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " -D%1", definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"-I%1\"", include_directory);
        }
        for (const char *include_file: include_files) {
            Fmt(&buf, " -include \"%1\"", include_file);
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::CXX: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::MakeLike;
        out_cmd->deps_filename = Fmt(alloc, "%1.d", dest_filename ? dest_filename : src_filename).ptr;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        out_cmd->cmd_line = Fmt(alloc, "\"%1\" /FO\"%2\" \"%3\"", rc, dest_filename, rc_filename);
        out_cmd->cache_len = out_cmd->cmd_line.len;
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, LinkType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case LinkType::Executable: {
                bool link_static = features & (int)CompileFeature::StaticLink;
                Fmt(&buf, "\"%1\"%2", cxx, link_static ? " -static" : "");
            } break;
            case LinkType::SharedLibrary: { Fmt(&buf, "\"%1\" -shared", cxx); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto");

            if (host != HostPlatform::Windows) {
                Fmt(&buf, " -Wl,-O1");
            }
        }

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        for (const char *lib: libraries) {
            if (host == HostPlatform::macOS && lib[0] == '!') {
                Fmt(&buf, " -framework %1", lib + 1);
            } else {
                Fmt(&buf, " -l%1", lib);
            }
        }

        // Platform flags
        switch (host) {
            case HostPlatform::Windows: {
                const char *suffix = (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) ? "" : "d";

                Fmt(&buf, " -Wl,/NODEFAULTLIB:libcmt -Wl,/NODEFAULTLIB:msvcrt -Wl,setargv.obj -Wl,oldnames.lib");

                if (features & (int)CompileFeature::StaticLink) {
                    Fmt(&buf, " -Wl,libcmt%1.lib", suffix);
                } else {
                    Fmt(&buf, " -Wl,msvcrt%1.lib", suffix);
                }

                if (features & (int)CompileFeature::DebugInfo) {
                    Fmt(&buf, " -g");
                }
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -ldl -pthread -framework CoreFoundation -framework SystemConfiguration");
            } break;

            default: {
                Fmt(&buf, " -pthread -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code,-z,stack-size=1048576");
                if (host == HostPlatform::Linux) {
                    Fmt(&buf, " -ldl -lrt");
                }
                if (link_type == LinkType::Executable) {
                    Fmt(&buf, " -pie");
                }

                // XXX: Ugly, but cross-compilation is not yet supported for this platform anyway
#if defined(__arm__) || defined(__thumb__)
                Fmt(&buf, " -latomic");
#endif
            } break;
        }

        // Features
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
            if (host == HostPlatform::Windows && !(features & (int)CompileFeature::StaticLink)) {
                Fmt(&buf, " -shared-libasan");
            }
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        if (features & (int)CompileFeature::SafeStack) {
            Fmt(&buf, " -fsanitize=safe-stack");
        }
        if (features & (int)CompileFeature::CFI) {
            RG_ASSERT(features & (int)CompileFeature::LTO);
            Fmt(&buf, " -fsanitize=cfi");
        }
        if (features & (int)CompileFeature::ShuffleCode) {
            if (lld_ver >= 130000) {
                Fmt(&buf, " -Wl,--shuffle-sections=*=0");
            } else {
                Fmt(&buf, " -Wl,--shuffle-sections=0");
            }
        }
        if (features & (int)CompileFeature::NoConsole) {
            Fmt(&buf, " -Wl,/subsystem:windows, -Wl,/entry:mainCRTStartup");
        }

        if (ld) {
            Fmt(&buf, " -fuse-ld=%1", ld);
        }
        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);
    }

    void MakePostCommand(const char *, const char *, Allocator *, Command *) const override { RG_UNREACHABLE(); }
};

class GnuCompiler final: public Compiler {
    const char *cc;
    const char *cxx;
    const char *windres;
    const char *ld;

    int gcc_ver = 0;
    bool i686 = false;

    BlockAllocator str_alloc;

public:
    GnuCompiler(HostPlatform host) : Compiler(host, "GCC") {}

    static std::unique_ptr<const Compiler> Create(HostPlatform host, const char *cc, const char *ld)
    {
        std::unique_ptr<GnuCompiler> compiler = std::make_unique<GnuCompiler>(host);

        // Find executables
        {
            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cc, "gcc", &prefix, &suffix, &version))
                return nullptr;

            compiler->cc = DuplicateString(cc, &compiler->str_alloc).ptr;
            compiler->cxx = Fmt(&compiler->str_alloc, "%1g++%2", prefix, suffix).ptr;
            compiler->windres = Fmt(&compiler->str_alloc, "%1windres%2", prefix, version).ptr;
            compiler->ld = ld ? DuplicateString(ld, &compiler->str_alloc).ptr : nullptr;
        }

        // Determine GCC version
        {
            char cmd[2048];
            Fmt(cmd, "\"%1\" -v", compiler->cc);

            HeapArray<char> output;
            if (ReadCommandOutput(cmd, Kilobytes(4), &output)) {
                compiler->gcc_ver = ParseVersion(cmd, output, "version");
                compiler->i686 = FindStr(output, "i686") >= 0;
            }

        };

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        if (DetectCcache()) {
            supported |= (int)CompileFeature::Ccache;
        }
        supported |= (int)CompileFeature::HotAssets;
        supported |= (int)CompileFeature::DebugInfo;
        supported |= (int)CompileFeature::StaticLink;
        if (host != HostPlatform::Windows) {
            // Sometimes it works, somestimes not and the object files are
            // corrupt... just avoid PCH on MinGW
            supported |= (int)CompileFeature::PCH;
            supported |= (int)CompileFeature::ASan;
            supported |= (int)CompileFeature::TSan;
            supported |= (int)CompileFeature::UBSan;
            supported |= (int)CompileFeature::LTO;
        }
        supported |= (int)CompileFeature::ZeroInit;
        if (gcc_ver >= 90000) {
            supported |= (int)CompileFeature::CFI;
        }
        supported |= (int)CompileFeature::Cxx17;
        if (host == HostPlatform::Windows) {
            supported |= (int)CompileFeature::NoConsole;
        }
        supported |= (int)CompileFeature::SSE41;
        supported |= (int)CompileFeature::SSE42;
        supported |= (int)CompileFeature::AVX2;
        supported |= (int)CompileFeature::AVX512;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }
        if ((features & (int)CompileFeature::ASan) && (features & (int)CompileFeature::TSan)) {
            LogError("Cannot use ASan and TSan at the same time");
            return false;
        }
        if (gcc_ver < 120100 && (features & (int)CompileFeature::ZeroInit)) {
            LogError("ZeroInit requires GCC >= 12.1, try --host option (e.g. --host=,gcc-12)");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override { return ".o"; }
    const char *GetLinkExtension() const override
    {
        const char *ext = (host == HostPlatform::Windows) ? ".exe" : "";
        return ext;
    }
    const char *GetPostExtension() const override { return nullptr; }

    bool GetCore(Span<const char *const>, Allocator *, HeapArray<const char *> *,
                 HeapArray<const char *> *, const char **) const override { return true; }

    void MakePackCommand(Span<const char *const> pack_filenames, bool optimize,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        RG::MakePackCommand(pack_filenames, optimize, false, pack_options,
                            dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type, bool warnings,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        MakeObjectCommand(pch_filename, src_type, warnings, nullptr, definitions,
                          include_directories, include_files, features, env_flags, nullptr, alloc, out_cmd);
    }

    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *cache_filename = Fmt(alloc, "%1.gch", pch_filename).ptr;
        return cache_filename;
    }
    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, bool warnings,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> include_files,
                           uint32_t features, bool env_flags, const char *dest_filename,
                           Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        if (features & (int)CompileFeature::Ccache) {
            Fmt(&buf, "ccache ");
        }

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" -std=gnu11", cc); } break;
            case SourceType::CXX: {
                const char *std = (features & (int)CompileFeature::Cxx17) ? "17" : "2a";
                Fmt(&buf, "\"%1\" -std=gnu++%2", cxx, std);
            } break;
        }
        if (dest_filename) {
            Fmt(&buf, " -o \"%1\"", dest_filename);
        } else {
            switch (src_type) {
                case SourceType::C: { Fmt(&buf, " -x c-header"); } break;
                case SourceType::CXX: { Fmt(&buf, " -x c++-header"); } break;
            }
        }
        Fmt(&buf, " -I. -MD -MF \"%1.d\"", dest_filename ? dest_filename : src_filename);
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " -fvisibility=hidden -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks");
        if (gcc_ver >= 100000) {
            Fmt(&buf, " -fno-finite-loops");
        }
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " -O2 -DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " -Os -DNDEBUG");
        } else {
            Fmt(&buf, " -O0 -fsanitize=signed-integer-overflow -fsanitize-undefined-trap-on-error -fno-omit-frame-pointer");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto");
        }
        if (warnings) {
            Fmt(&buf, " -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-cast-function-type");
            if (src_type == SourceType::CXX) {
                Fmt(&buf, " -Wno-init-list-lifetime");
            }
        } else {
            Fmt(&buf, " -w");
        }
        if (features & (int)CompileFeature::HotAssets) {
            Fmt(&buf, " -DFELIX_HOT_ASSETS");
        }
#if defined(__x86_64__)
        if (features & (int)CompileFeature::SSE41) {
            Fmt(&buf, " -msse4.1");
        }
        if (features & (int)CompileFeature::SSE42) {
            Fmt(&buf, " -msse4.2");
        }
        if (features & (int)CompileFeature::AVX2) {
            Fmt(&buf, " -mavx2");
        }
        if (features & (int)CompileFeature::AVX512) {
            Fmt(&buf, " -mavx512f -mavx512vl");
        }
#elif defined(__i386__)
        Fmt(&buf, " -msse2");
#endif

        // Platform flags
        switch (host) {
            case HostPlatform::Windows: {
                Fmt(&buf, " -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE"
                          " -D__USE_MINGW_ANSI_STDIO=1");
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -pthread -fPIC -fno-semantic-interposition");
            } break;

            default: {
                Fmt(&buf, " -D_FILE_OFFSET_BITS=64 -pthread -fPIC -fno-semantic-interposition");
                if (features & ((int)CompileFeature::OptimizeSpeed | (int)CompileFeature::OptimizeSize)) {
                    Fmt(&buf, " -D_FORTIFY_SOURCE=2");
                }

                // XXX: Ugly, but cross-compilation is not yet supported for this platform anyway
#if defined(__arm__) || defined(__thumb__)
                Fmt(&buf, " -Wno-psabi");
#endif
            } break;
        }

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " -g");
        }
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        Fmt(&buf, " -fstack-protector-strong --param ssp-buffer-size=4");
        if (host != HostPlatform::Windows) {
            Fmt(&buf, " -fstack-clash-protection");
        }
        if (features & (int)CompileFeature::ZeroInit) {
            Fmt(&buf, " -ftrivial-auto-var-init=zero");
        }
        if (features & (int)CompileFeature::CFI) {
            Fmt(&buf, " -cfprotection=branch");
        }

        // Sources and definitions
        Fmt(&buf, " -DFELIX -c \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " -include \"%1\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " -D%1", definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"-I%1\"", include_directory);
        }
        for (const char *include_file: include_files) {
            Fmt(&buf, " -include \"%1\"", include_file);
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::CXX: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fdiagnostics-color=always");
        } else {
            Fmt(&buf, " -fdiagnostics-color=never");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::MakeLike;
        out_cmd->deps_filename = Fmt(alloc, "%1.d", dest_filename ? dest_filename : src_filename).ptr;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        out_cmd->cmd_line = Fmt(alloc, "\"%1\" -O coff \"%2\" \"%3\"", windres, rc_filename, dest_filename);
        out_cmd->cache_len = out_cmd->cmd_line.len;
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, LinkType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case LinkType::Executable: {
                bool static_link = features & (int)CompileFeature::StaticLink;
                Fmt(&buf, "\"%1\"%2", cxx, static_link ? " -static" : "");
            } break;
            case LinkType::SharedLibrary: { Fmt(&buf, "\"%1\" -shared", cxx); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (!(features & (int)CompileFeature::DebugInfo)) {
            Fmt(&buf, " -s");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto -Wl,-O1");
        }

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        for (const char *lib: libraries) {
            if (host == HostPlatform::macOS && lib[0] == '!') {
                Fmt(&buf, " -framework %1", lib + 1);
            } else {
                Fmt(&buf, " -l%1", lib);
            }
        }

        // Platform flags and libraries
        switch (host) {
            case HostPlatform::Windows: {
                Fmt(&buf, " -Wl,--dynamicbase -Wl,--nxcompat");
                if (!i686) {
                    Fmt(&buf, " -Wl,--high-entropy-va");
                }
            } break;

            case HostPlatform::macOS: {
                Fmt(&buf, " -ldl -pthread -framework CoreFoundation -framework SystemConfiguration");
            } break;

            default: {
                Fmt(&buf, " -pthread -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code,-z,stack-size=1048576");
                if (host == HostPlatform::Linux) {
                    Fmt(&buf, " -ldl -lrt");
                }
                if (link_type == LinkType::Executable) {
                    Fmt(&buf, " -pie");
                }

                // XXX: Ugly, but cross-compilation is not yet supported for this platform anyway
#if defined(__arm__) || defined(__thumb__)
                Fmt(&buf, " -latomic");
#endif
            } break;
        }

        // Features
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " -fsanitize=address");
        }
        if (features & (int)CompileFeature::TSan) {
            Fmt(&buf, " -fsanitize=thread");
        }
        if (features & (int)CompileFeature::UBSan) {
            Fmt(&buf, " -fsanitize=undefined");
        }
        if (host == HostPlatform::Windows) {
            Fmt(&buf, " -lssp");
        }
        if (features & (int)CompileFeature::NoConsole) {
            Fmt(&buf, " -mwindows");
        }

        if (ld) {
            Fmt(&buf, " -fuse-ld=%1", ld);
        }
        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fdiagnostics-color=always");
        } else {
            Fmt(&buf, " -fdiagnostics-color=never");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);
    }

    void MakePostCommand(const char *, const char *, Allocator *, Command *) const override { RG_UNREACHABLE(); }
};

#ifdef _WIN32
class MsCompiler final: public Compiler {
    const char *cl;
    const char *rc;
    const char *link;

    BlockAllocator str_alloc;

public:
    MsCompiler() : Compiler(HostPlatform::Windows, "MSVC") {}

    static std::unique_ptr<const Compiler> Create(const char *cl)
    {
        std::unique_ptr<MsCompiler> compiler = std::make_unique<MsCompiler>();

        // Find executables
        {
            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cl, "cl", &prefix, &suffix, &version))
                return nullptr;

            compiler->cl = DuplicateString(cl, &compiler->str_alloc).ptr;
            compiler->rc = Fmt(&compiler->str_alloc, "%1rc%2", prefix, version).ptr;
            compiler->link = Fmt(&compiler->str_alloc, "%1link%2", prefix, version).ptr;
        }

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        supported |= (int)CompileFeature::HotAssets;
        supported |= (int)CompileFeature::PCH;
        supported |= (int)CompileFeature::DebugInfo;
        supported |= (int)CompileFeature::StaticLink;
        supported |= (int)CompileFeature::ASan;
        supported |= (int)CompileFeature::LTO;
        supported |= (int)CompileFeature::CFI;
        supported |= (int)CompileFeature::Cxx17;
        if (host == HostPlatform::Windows) {
            supported |= (int)CompileFeature::NoConsole;
        }
        supported |= (int)CompileFeature::SSE41;
        supported |= (int)CompileFeature::SSE42;
        supported |= (int)CompileFeature::AVX2;
        supported |= (int)CompileFeature::AVX512;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override { return ".obj"; }
    const char *GetLinkExtension() const override { return ".exe"; }
    const char *GetPostExtension() const override { return nullptr; }

    bool GetCore(Span<const char *const>, Allocator *, HeapArray<const char *> *,
                 HeapArray<const char *> *, const char **) const override { return true; }

    void MakePackCommand(Span<const char *const> pack_filenames, bool optimize,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        // Strings literals are limited in length in MSVC, even with concatenation (64kiB)
        RG::MakePackCommand(pack_filenames, optimize, true, pack_options,
                            dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type, bool warnings,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        MakeObjectCommand(pch_filename, src_type, warnings, nullptr, definitions, include_directories,
                          include_files, features, env_flags, nullptr, alloc, out_cmd);
    }

    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *cache_filename = Fmt(alloc, "%1.pch", pch_filename).ptr;
        return cache_filename;
    }
    const char *GetPchObject(const char *pch_filename, Allocator *alloc) const override
    {
        RG_ASSERT(alloc);

        const char *obj_filename = Fmt(alloc, "%1.obj", pch_filename).ptr;
        return obj_filename;
    }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, bool warnings,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> include_files,
                           uint32_t features, bool env_flags, const char *dest_filename,
                           Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" /nologo", cl); } break;
            case SourceType::CXX: {
                const char *std = (features & (int)CompileFeature::Cxx17) ? "17" : "latest";
                Fmt(&buf, "\"%1\" /nologo /std:c++%2 /Zc:__cplusplus", cl, std);
            } break;
        }
        if (dest_filename) {
            Fmt(&buf, " \"/Fo%1\"", dest_filename);
        } else {
            Fmt(&buf, " /Yc \"/Fp%1.pch\" \"/Fo%1.obj\"", src_filename);
        }
        Fmt(&buf, " /showIncludes");
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " /I. /EHsc /utf-8");
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " /O2 /DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " /O1 /DNDEBUG");
        } else {
            Fmt(&buf, " /Od /RTCsu");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " /GL");
        }
        if (warnings) {
            Fmt(&buf, " /W4 /wd4200 /wd4458 /wd4706 /wd4100 /wd4127 /wd4702");
        } else {
            Fmt(&buf, " /w");
        }
        if (features & (int)CompileFeature::HotAssets) {
            Fmt(&buf, " /DFELIX_HOT_ASSETS");
        }

        // Platform flags
        Fmt(&buf, " /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 /DUNICODE /D_UNICODE"
                  " /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE");

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " /Z7 /Zo");
        }
        if (features & (int)CompileFeature::StaticLink) {
            Fmt(&buf, " /MT");
        } else {
            Fmt(&buf, " /MD");
        }
        if (features & (int)CompileFeature::ASan) {
            Fmt(&buf, " /fsanitize=address");
        }
        Fmt(&buf, " /GS");
        if (features & (int)CompileFeature::CFI) {
            Fmt(&buf, " /guard:cf /guard:ehcont");
        }
#ifdef __x86_64__
        if (features & (int)CompileFeature::AVX2) {
            Fmt(&buf, " /arch:AVX2");
        }
        if (features & (int)CompileFeature::AVX512) {
            Fmt(&buf, " /arch:AVX512");
        }
#endif

        // Sources and definitions
        Fmt(&buf, " /DFELIX /c /utf-8 \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " \"/FI%1\" \"/Yu%1\" \"/Fp%1.pch\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " /D%1", definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"/I%1\"", include_directory);
        }
        for (const char *include_file: include_files) {
            if (PathIsAbsolute(include_file)) {
                Fmt(&buf, " \"/FI%1\"", include_file);
            } else {
                const char *cwd = GetWorkingDirectory();
                Fmt(&buf, " \"/FI%1%/%2\"", cwd, include_file);
            }
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::CXX: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        out_cmd->cmd_line = buf.TrimAndLeak(1);
        out_cmd->skip_lines = 1;

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::ShowIncludes;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        out_cmd->cmd_line = Fmt(alloc, "\"%1\" /nologo /FO\"%2\" \"%3\"", rc, dest_filename, rc_filename);
        out_cmd->cache_len = out_cmd->cmd_line.len;
    }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, LinkType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case LinkType::Executable: { Fmt(&buf, "\"%1\" /nologo", link); } break;
            case LinkType::SharedLibrary: { Fmt(&buf, "\"%1\" /nologo /DLL", link); } break;
        }
        Fmt(&buf, " \"/OUT:%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " /LTCG");
        }
        Fmt(&buf, " /DYNAMICBASE /HIGHENTROPYVA");

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        for (const char *lib: libraries) {
            Fmt(&buf, " %1.lib", lib);
        }
        Fmt(&buf, " setargv.obj");

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " /DEBUG:FULL");
        } else {
            Fmt(&buf, " /DEBUG:NONE");
        }
        if (features & (int)CompileFeature::CFI) {
            Fmt(&buf, " /GUARD:cf /GUARD:ehcont");
        }
        if (features & (int)CompileFeature::NoConsole) {
            Fmt(&buf, " /SUBSYSTEM:windows /ENTRY:mainCRTStartup");
        }

        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        out_cmd->cmd_line = buf.TrimAndLeak(1);
        out_cmd->skip_lines = 1;
    }

    void MakePostCommand(const char *, const char *, Allocator *, Command *) const override { RG_UNREACHABLE(); }
};
#endif

class TeensyCompiler final: public Compiler {
    enum class Model {
        Teensy20,
        Teensy20pp,
        TeensyLC,
        Teensy30,
        Teensy31,
        Teensy35,
        Teensy36,
        Teensy40,
        Teensy41
    };

    const char *cc;
    const char *cxx;
    const char *ld;
    const char *objcopy;
    Model model;

    BlockAllocator str_alloc;

public:
    TeensyCompiler(HostPlatform host) : Compiler(host, "GCC") {}

    static std::unique_ptr<const Compiler> Create(HostPlatform host, const char *cc)
    {
        std::unique_ptr<TeensyCompiler> compiler = std::make_unique<TeensyCompiler>(host);

        // Decode model string
        switch (host) {
            case HostPlatform::Teensy20: { compiler->model = Model::Teensy20; } break;
            case HostPlatform::Teensy20pp: { compiler->model = Model::Teensy20pp; } break;
            case HostPlatform::TeensyLC: { compiler->model = Model::TeensyLC; } break;
            case HostPlatform::Teensy30: { compiler->model = Model::Teensy30; } break;
            case HostPlatform::Teensy31: { compiler->model = Model::Teensy31; } break;
            case HostPlatform::Teensy35: { compiler->model = Model::Teensy35; } break;
            case HostPlatform::Teensy36: { compiler->model = Model::Teensy36; } break;
            case HostPlatform::Teensy40: { compiler->model = Model::Teensy40; } break;
            case HostPlatform::Teensy41: { compiler->model = Model::Teensy41; } break;

            default: { RG_UNREACHABLE(); } break;
        }

        // Find executables
        {
            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cc, "gcc", &prefix, &suffix, &version))
                return nullptr;

            compiler->cc = DuplicateString(cc, &compiler->str_alloc).ptr;
            compiler->cxx = Fmt(&compiler->str_alloc, "%1g++%2", prefix, suffix).ptr;
            compiler->ld = Fmt(&compiler->str_alloc, "%1ld%2", prefix, version).ptr;
            compiler->objcopy = Fmt(&compiler->str_alloc, "%1objcopy%2", prefix, version).ptr;
        }

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        supported |= (int)CompileFeature::DebugInfo;
        supported |= (int)CompileFeature::LTO;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override { return ".o"; }
    const char *GetLinkExtension() const override { return ".elf"; }
    const char *GetPostExtension() const override { return ".hex"; }

    bool GetCore(Span<const char *const> definitions, Allocator *alloc,
                 HeapArray<const char *> *out_filenames, HeapArray<const char *> *out_definitions,
                 const char **out_ns) const override
    {
        const char *dirname = nullptr;
        switch (model) {
            case Model::Teensy20:
            case Model::Teensy20pp: { dirname = "vendor/teensy/cores/teensy"; } break;
            case Model::TeensyLC:
            case Model::Teensy30:
            case Model::Teensy31:
            case Model::Teensy35:
            case Model::Teensy36: { dirname = "vendor/teensy/cores/teensy3"; } break;
            case Model::Teensy40:
            case Model::Teensy41: { dirname = "vendor/teensy/cores/teensy4"; } break;
        }
        RG_ASSERT(dirname);

        EnumResult ret = EnumerateDirectory(dirname, nullptr, 1024,
                                            [&](const char *basename, FileType) {
            if (TestStr(basename, "Blink.cc"))
                return true;

            if (DetermineSourceType(basename)) {
                const char *src_filename = NormalizePath(basename, dirname, alloc).ptr;
                out_filenames->Append(src_filename);
            }

            return true;
        });
        if (ret != EnumResult::Success)
            return false;

        uint64_t hash = 0;
        for (const char *definition: definitions) {
            if (StartsWith(definition, "F_CPU=") ||
                    StartsWith(definition, "USB_") ||
                    StartsWith(definition, "LAYOUT_")) {
                out_definitions->Append(definition);
                hash ^= HashTraits<const char *>::Hash(definition);
            }
        }
        *out_ns = Fmt(alloc, "%1", FmtHex(hash).Pad0(-16)).ptr;

        return true;
    }

    void MakePackCommand(Span<const char *const> pack_filenames, bool optimize,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        // Strings literals are limited in length in MSVC, even with concatenation (64kiB)
        RG::MakePackCommand(pack_filenames, optimize, true, pack_options,
                            dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type, bool warnings,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override { RG_UNREACHABLE(); }
    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override { return nullptr; }
    const char *GetPchObject(const char *pch_filename, Allocator *alloc) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, bool warnings,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> include_files,
                           uint32_t features, bool env_flags, const char *dest_filename,
                           Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" -std=gnu11", cc); } break;
            case SourceType::CXX: { Fmt(&buf, "\"%1\" -std=gnu++14", cxx); } break;
        }
        RG_ASSERT(dest_filename); // No PCH
        Fmt(&buf, " -o \"%1\"", dest_filename);
        Fmt(&buf, " -MD -MF \"%1.d\"", dest_filename ? dest_filename : src_filename);
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " -I. -fvisibility=hidden -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks");
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " -O2 -DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " -Os -DNDEBUG");
        } else {
            Fmt(&buf, " -O0 -fsanitize=signed-integer-overflow -fsanitize-undefined-trap-on-error -fno-omit-frame-pointer");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto");
        }
        if (warnings) {
            Fmt(&buf, " -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter");
        } else {
            Fmt(&buf, " -w");
        }

        // Don't override explicit user defines
        bool set_fcpu = true;
        bool set_usb = true;
        bool set_layout = true;
        for (const char *definition: definitions) {
            set_fcpu &= !StartsWith(definition, "F_CPU=");
            set_usb &= !StartsWith(definition, "USB_");
            set_layout &= !StartsWith(definition, "LAYOUT_");
        }

        // Platform flags
        Fmt(&buf, " -ffunction-sections -fdata-sections -nostdlib");
        Fmt(&buf, " -DARDUINO=10805 -DTEENSYDUINO=153");
        switch (model) {
            case Model::Teensy20: { Fmt(&buf, " -DARDUINO_ARCH_AVR -DARDUINO_TEENSY2 -Ivendor/teensy/cores/teensy -mmcu=atmega32u4%1", set_fcpu ? " -DF_CPU=16000000" : ""); } break;
            case Model::Teensy20pp: { Fmt(&buf, " -DARDUINO_ARCH_AVR -DARDUINO_TEENSY2PP -Ivendor/teensy/cores/teensy -mmcu=at90usb1286%1", set_fcpu ? " -DF_CPU=16000000" : ""); } break;
            case Model::TeensyLC: { Fmt(&buf, " -DARDUINO_TEENSYLC -Ivendor/teensy/cores/teensy3 -mcpu=cortex-m0plus -mthumb"
                                              " -fsingle-precision-constant -mno-unaligned-access -Wno-error=narrowing -D__MKL26Z64__%1", set_fcpu ? " -DF_CPU=48000000" : ""); } break;
            case Model::Teensy30: { Fmt(&buf, " -DARDUINO_TEENSY30 -Ivendor/teensy/cores/teensy3 -mcpu=cortex-m4 -mthumb"
                                              " -fsingle-precision-constant -mno-unaligned-access -Wno-error=narrowing -D__MK20DX128__%1", set_fcpu ? " -DF_CPU=96000000" : ""); } break;
            case Model::Teensy31: { Fmt(&buf, " -DARDUINO_TEENSY31 -Ivendor/teensy/cores/teensy3 -mcpu=cortex-m4 -mthumb"
                                              " -fsingle-precision-constant -mno-unaligned-access -Wno-error=narrowing -D__MK20DX256__%1", set_fcpu ? " -DF_CPU=96000000" : ""); } break;
            case Model::Teensy35: { Fmt(&buf, " -DARDUINO_TEENSY35 -Ivendor/teensy/cores/teensy3 -mcpu=cortex-m4 -mthumb -mfloat-abi=hard"
                                              " -mfpu=fpv4-sp-d16 -fsingle-precision-constant -mno-unaligned-access -Wno-error=narrowing -D__MK64FX512__%1", set_fcpu ? " -DF_CPU=120000000" : ""); } break;
            case Model::Teensy36: { Fmt(&buf, " -DARDUINO_TEENSY36 -Ivendor/teensy/cores/teensy3 -mcpu=cortex-m4 -mthumb -mfloat-abi=hard"
                                              " -mfpu=fpv4-sp-d16 -fsingle-precision-constant -mno-unaligned-access -Wno-error=narrowing -D__MK66FX1M0__%1", set_fcpu ? " -DF_CPU=180000000" : ""); } break;
            case Model::Teensy40: { Fmt(&buf, " -DARDUINO_TEENSY40 -Ivendor/teensy/cores/teensy4 -mcpu=cortex-m7 -mthumb -mfloat-abi=hard"
                                              " -mfpu=fpv5-d16 -mno-unaligned-access -D__IMXRT1062__%1", set_fcpu ? " -DF_CPU=600000000" : ""); } break;
            case Model::Teensy41: { Fmt(&buf, " -DARDUINO_TEENSY41 -Ivendor/teensy/cores/teensy4 -mcpu=cortex-m7 -mthumb -mfloat-abi=hard"
                                              " -mfpu=fpv5-d16 -mno-unaligned-access -D__IMXRT1062__%1", set_fcpu ? " -DF_CPU=600000000" : ""); } break;
        }
        if (src_type == SourceType::CXX) {
            Fmt(&buf, " -felide-constructors -fno-exceptions -fno-rtti");
        }
        if (set_usb) {
            Fmt(&buf, " -DUSB_SERIAL");
        }
        if (set_layout) {
            Fmt(&buf, " -DLAYOUT_US_ENGLISH");
        }

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " -g");
        }
        if (features & (int)CompileFeature::ZeroInit) {
            Fmt(&buf, " -ftrivial-auto-var-init=zero");
        }

        // Sources and definitions
        Fmt(&buf, " -DFELIX -c \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " -include \"%1\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " -D%1", definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"-I%1\"", include_directory);
        }
        for (const char *include_file: include_files) {
            Fmt(&buf, " -include \"%1\"", include_file);
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::CXX: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fdiagnostics-color=always");
        } else {
            Fmt(&buf, " -fdiagnostics-color=never");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::MakeLike;
        out_cmd->deps_filename = Fmt(alloc, "%1.d", dest_filename ? dest_filename : src_filename).ptr;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override { RG_UNREACHABLE(); }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, LinkType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case LinkType::Executable: { Fmt(&buf, "\"%1\"", cc); } break;
            case LinkType::SharedLibrary: { RG_UNREACHABLE(); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Build mode
        if (!(features & (int)CompileFeature::DebugInfo)) {
            Fmt(&buf, " -s");
        }
        if (features & (int)CompileFeature::LTO) {
            Fmt(&buf, " -flto -Wl,-Os");
        }

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        for (const char *lib: libraries) {
            Fmt(&buf, " -l%1", lib);
        }

        // Platform flags and libraries
        Fmt(&buf, " -Wl,--gc-sections,--defsym=__rtc_localtime=0 --specs=nano.specs");
        switch (model) {
            case Model::Teensy20: { Fmt(&buf, " -mmcu=atmega32u4"); } break;
            case Model::Teensy20pp: { Fmt(&buf, " -mmcu=at90usb1286"); } break;
            case Model::TeensyLC: { Fmt(&buf, " -mcpu=cortex-m0plus -mthumb -larm_cortexM0l_math -fsingle-precision-constant -Tvendor/teensy/cores/teensy3/mkl26z64.ld"); } break;
            case Model::Teensy30: { Fmt(&buf, " -mcpu=cortex-m4 -mthumb -larm_cortexM4l_math -fsingle-precision-constant -Tvendor/teensy/cores/teensy3/mk20dx128.ld"); } break;
            case Model::Teensy31: { Fmt(&buf, " -mcpu=cortex-m4 -mthumb -larm_cortexM4l_math -fsingle-precision-constant -Tvendor/teensy/cores/teensy3/mk20dx256.ld"); } break;
            case Model::Teensy35: { Fmt(&buf, " -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -larm_cortexM4lf_math -fsingle-precision-constant -Tvendor/teensy/cores/teensy3/mk64fx512.ld"); } break;
            case Model::Teensy36: { Fmt(&buf, " -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -larm_cortexM4lf_math -fsingle-precision-constant -Tvendor/teensy/cores/teensy3/mk66fx1m0.ld"); } break;
            case Model::Teensy40: { Fmt(&buf, " -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -larm_cortexM7lfsp_math -Tvendor/teensy/cores/teensy4/imxrt1062.ld"); } break;
            case Model::Teensy41: { Fmt(&buf, " -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -larm_cortexM7lfsp_math -Tvendor/teensy/cores/teensy4/imxrt1062_t41.ld"); } break;
        }
        Fmt(&buf, " -lm -lstdc++");

        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fdiagnostics-color=always");
        } else {
            Fmt(&buf, " -fdiagnostics-color=never");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);
    }

    void MakePostCommand(const char *src_filename, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        Span<const char> cmd = Fmt(alloc, "\"%1\" -O ihex -R .eeprom \"%2\" \"%3\"", objcopy, src_filename, dest_filename);
        out_cmd->cmd_line = cmd;
    }
};

class EmCompiler final: public Compiler {
    const char *cc;
    const char *cxx;

    BlockAllocator str_alloc;

public:
    EmCompiler(HostPlatform host) : Compiler(host, "EmCC") {}

    static std::unique_ptr<const Compiler> Create(HostPlatform host, const char *cc)
    {
        std::unique_ptr<EmCompiler> compiler = std::make_unique<EmCompiler>(host);

        // Find executables
        {
            if (!FindExecutableInPath(cc, &compiler->str_alloc, &cc)) {
                LogError("Could not find '%1' in PATH", cc);
                return nullptr;
            }
            if (host == HostPlatform::EmscriptenBox && !FindExecutableInPath("wasm2c")) {
                LogError("Could not find 'wasm2c' in PATH");
                return nullptr;
            }

            Span<const char> prefix;
            Span<const char> suffix;
            Span<const char> version;
            if (!SplitPrefixSuffix(cc, "emcc", &prefix, &suffix, &version))
                return nullptr;

            compiler->cc = cc;
            compiler->cxx = Fmt(&compiler->str_alloc, "%1em++%2", prefix, suffix).ptr;
        }

        return compiler;
    }

    bool CheckFeatures(uint32_t features, uint32_t maybe_features, uint32_t *out_features) const override
    {
        uint32_t supported = 0;

        supported |= (int)CompileFeature::OptimizeSpeed;
        supported |= (int)CompileFeature::OptimizeSize;
        supported |= (int)CompileFeature::DebugInfo;
        supported |= (int)CompileFeature::Cxx17;

        uint32_t unsupported = features & ~supported;
        if (unsupported) {
            LogError("Some features are not supported by %1: %2",
                     name, FmtFlags(unsupported, CompileFeatureOptions));
            return false;
        }

        features |= (supported & maybe_features);

        if ((features & (int)CompileFeature::OptimizeSpeed) && (features & (int)CompileFeature::OptimizeSize)) {
            LogError("Cannot use OptimizeSpeed and OptimizeSize at the same time");
            return false;
        }

        *out_features = features;
        return true;
    }

    const char *GetObjectExtension() const override { return ".o"; }
    const char *GetLinkExtension() const override
    {
        switch (host) {
            case HostPlatform::EmscriptenNode: return ".js";
            case HostPlatform::EmscriptenWeb: return ".html";
            case HostPlatform::EmscriptenBox: return ".wasm";

            default: { RG_UNREACHABLE(); } break;
        }
    }
    const char *GetPostExtension() const override
    {
        if (host == HostPlatform::EmscriptenBox) {
            return ".c";
        } else {
            return nullptr;
        }
    }

    bool GetCore(Span<const char *const>, Allocator *, HeapArray<const char *> *,
                 HeapArray<const char *> *, const char **) const override { return true; }

    void MakePackCommand(Span<const char *const> pack_filenames, bool optimize,
                         const char *pack_options, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);
        RG::MakePackCommand(pack_filenames, optimize, false, pack_options,
                            dest_filename, alloc, out_cmd);
    }

    void MakePchCommand(const char *pch_filename, SourceType src_type, bool warnings,
                        Span<const char *const> definitions, Span<const char *const> include_directories,
                        Span<const char *const> include_files, uint32_t features, bool env_flags,
                        Allocator *alloc, Command *out_cmd) const override { RG_UNREACHABLE(); }

    const char *GetPchCache(const char *pch_filename, Allocator *alloc) const override { return nullptr; }
    const char *GetPchObject(const char *, Allocator *) const override { return nullptr; }

    void MakeObjectCommand(const char *src_filename, SourceType src_type, bool warnings,
                           const char *pch_filename, Span<const char *const> definitions,
                           Span<const char *const> include_directories, Span<const char *const> include_files,
                           uint32_t features, bool env_flags, const char *dest_filename,
                           Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Compiler
        switch (src_type) {
            case SourceType::C: { Fmt(&buf, "\"%1\" -std=gnu11", cc); } break;
            case SourceType::CXX: {
                const char *std = (features & (int)CompileFeature::Cxx17) ? "17" : "2a";
                Fmt(&buf, "\"%1\" -std=gnu++%2", cxx, std);
            } break;
        }
        RG_ASSERT(dest_filename); // No PCH
        Fmt(&buf, " -o \"%1\"", dest_filename);
        Fmt(&buf, " -MD -MF \"%1.d\"", dest_filename ? dest_filename : src_filename);
        out_cmd->rsp_offset = buf.len;

        // Build options
        Fmt(&buf, " -I. -fvisibility=hidden -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks");
        if (features & (int)CompileFeature::OptimizeSpeed) {
            Fmt(&buf, " -O1 -DNDEBUG");
        } else if (features & (int)CompileFeature::OptimizeSize) {
            Fmt(&buf, " -Os -DNDEBUG");
        } else {
            Fmt(&buf, " -O0 -ftrapv");
        }
        if (warnings) {
            Fmt(&buf, " -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter");
        } else {
            Fmt(&buf, " -Wno-everything");
        }

        // Features
        if (features & (int)CompileFeature::DebugInfo) {
            Fmt(&buf, " -g");
        }

        // Sources and definitions
        Fmt(&buf, " -DFELIX -c \"%1\"", src_filename);
        if (pch_filename) {
            Fmt(&buf, " -include \"%1\"", pch_filename);
        }
        for (const char *definition: definitions) {
            Fmt(&buf, " -D%1", definition);
        }
        for (const char *include_directory: include_directories) {
            Fmt(&buf, " \"-I%1\"", include_directory);
        }
        for (const char *include_file: include_files) {
            Fmt(&buf, " -include \"%1\"", include_file);
        }

        if (env_flags) {
            switch (src_type) {
                case SourceType::C: { AddEnvironmentFlags({"CPPFLAGS", "CFLAGS"}, &buf); } break;
                case SourceType::CXX: { AddEnvironmentFlags({"CPPFLAGS", "CXXFLAGS"}, &buf); } break;
            }
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);

        // Dependencies
        out_cmd->deps_mode = Command::DependencyMode::MakeLike;
        out_cmd->deps_filename = Fmt(alloc, "%1.d", dest_filename ? dest_filename : src_filename).ptr;
    }

    void MakeResourceCommand(const char *rc_filename, const char *dest_filename,
                             Allocator *alloc, Command *out_cmd) const override { RG_UNREACHABLE(); }

    void MakeLinkCommand(Span<const char *const> obj_filenames,
                         Span<const char *const> libraries, LinkType link_type,
                         uint32_t features, bool env_flags, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        HeapArray<char> buf(alloc);

        // Linker
        switch (link_type) {
            case LinkType::Executable: { Fmt(&buf, "\"%1\"", cxx); } break;
            case LinkType::SharedLibrary: { Fmt(&buf, "\"%1\" -shared", cxx); } break;
        }
        Fmt(&buf, " -o \"%1\"", dest_filename);
        out_cmd->rsp_offset = buf.len;

        // Objects and libraries
        for (const char *obj_filename: obj_filenames) {
            Fmt(&buf, " \"%1\"", obj_filename);
        }
        for (const char *lib: libraries) {
            Fmt(&buf, " -l%1", lib);
        }

        // Features
        Fmt(&buf, " -s STANDALONE_WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s MAXIMUM_MEMORY=2147483648");
        if (host == HostPlatform::EmscriptenNode) {
            Fmt(&buf, " -s NODERAWFS=1 -lnodefs.js");
        }

        if (env_flags) {
            AddEnvironmentFlags("LDFLAGS", &buf);
        }

        out_cmd->cache_len = buf.len;
        if (FileIsVt100(stdout)) {
            Fmt(&buf, " -fcolor-diagnostics -fansi-escape-codes");
        } else {
            Fmt(&buf, " -fno-color-diagnostics");
        }
        out_cmd->cmd_line = buf.TrimAndLeak(1);
    }

    void MakePostCommand(const char *src_filename, const char *dest_filename,
                         Allocator *alloc, Command *out_cmd) const override
    {
        RG_ASSERT(alloc);

        Span<const char> cmd = Fmt(alloc, "wasm2c \"%1\" -o \"%2\"", src_filename, dest_filename);
        out_cmd->cmd_line = cmd;
    }
};

static void FindArduinoCompiler(const char *name, const char *compiler, Span<char> out_cc)
{
#ifdef _WIN32
    wchar_t buf[2048];
    DWORD buf_len = RG_LEN(buf);

    bool arduino = !RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Arduino", L"Install_Dir",
                                 RRF_RT_REG_SZ, nullptr, buf, &buf_len) ||
                   !RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Arduino", L"Install_Dir",
                                 RRF_RT_REG_SZ, nullptr, buf, &buf_len) ||
                   !RegGetValueW(HKEY_CURRENT_USER, L"Software\\Arduino", L"Install_Dir",
                                 RRF_RT_REG_SZ, nullptr, buf, &buf_len) ||
                   !RegGetValueW(HKEY_CURRENT_USER, L"Software\\WOW6432Node\\Arduino", L"Install_Dir",
                                 RRF_RT_REG_SZ, nullptr, buf, &buf_len);
    if (!arduino)
        return;

    Size pos = ConvertWin32WideToUtf8(buf, out_cc);
    if (pos < 0)
        return;

    Span<char> remain = out_cc.Take(pos, out_cc.len - pos);
    Fmt(remain, "%/%1.exe", compiler);
    for (Size i = 0; remain.ptr[i]; i++) {
        char c = remain.ptr[i];
        remain.ptr[i] = (c == '/') ? '\\' : c;
    }

    if (TestFile(out_cc.ptr, FileType::File)) {
        LogDebug("Found %1 compiler for Teensy: '%2'", name, out_cc.ptr);
    } else {
        out_cc[0] = 0;
    }
#else
    struct TestPath {
        const char *env;
        const char *path;
    };

    static const TestPath test_paths[] = {
        {nullptr, "/usr/share/arduino"},
        {nullptr, "/usr/local/share/arduino"},
        {"HOME",  ".local/share/arduino"},
#ifdef __APPLE__
        {nullptr, "/Applications/Arduino.app/Contents/Java"}
#endif
    };

    for (const TestPath &test: test_paths) {
        if (test.env) {
            Span<const char> prefix = getenv(test.env);

            if (!prefix.len)
                continue;

            while (prefix.len && IsPathSeparator(prefix[prefix.len - 1])) {
                prefix.len--;
            }

            Fmt(out_cc, "%1%/%2%/%3", prefix, test.path, compiler);
        } else {
            Fmt(out_cc, "%1%/%2", test.path, compiler);
        }

        if (TestFile(out_cc.ptr, FileType::File)) {
            LogDebug("Found %1 compiler for Teensy: '%2'", name, out_cc.ptr);
            return;
        }
    }

    out_cc[0] = 0;
#endif
}

std::unique_ptr<const Compiler> PrepareCompiler(PlatformSpecifier spec)
{
    if (spec.host == NativeHost) {
        if (!spec.cc) {
            for (const SupportedCompiler &supported: SupportedCompilers) {
                if (supported.cc && FindExecutableInPath(supported.cc)) {
                    spec.cc = supported.cc;
                    break;
                }
            }

            if (!spec.cc) {
                LogError("Could not find any supported compiler in PATH");
                return nullptr;
            }
        } else if (!FindExecutableInPath(spec.cc)) {
            LogError("Cannot find compiler '%1' in PATH", spec.cc);
            return nullptr;
        }

        if (spec.ld) {
            if (TestStr(spec.ld, "bfd") || TestStr(spec.ld, "ld")) {
                if (!FindExecutableInPath("ld.bfd")) {
                    LogError("Cannot find linker 'ld' in PATH");
                    return nullptr;
                }

                spec.ld = "bfd";
#ifdef _WIN32
            } else if (TestStr(spec.ld, "link")) {
                if (!FindExecutableInPath("link")) {
                    LogError("Cannot find linker 'link.exe' in PATH");
                    return nullptr;
                }
#endif
            } else {
                char buf[512];
                Fmt(buf, "ld.%1", spec.ld);

                if (!FindExecutableInPath(buf)) {
                    LogError("Cannot find linker '%1' in PATH", buf);
                    return nullptr;
                }
            }
        }

        if (IdentifyCompiler(spec.cc, "clang")) {
            return ClangCompiler::Create(spec.host, spec.cc, spec.ld);
        } else if (IdentifyCompiler(spec.cc, "gcc")) {
            return GnuCompiler::Create(spec.host, spec.cc, spec.ld);
#ifdef _WIN32
        } else if (IdentifyCompiler(spec.cc, "cl")) {
            if (spec.ld) {
                LogError("Cannot use custom linker with MSVC compiler");
                return nullptr;
            }

            return MsCompiler::Create(spec.cc);
#endif
        } else {
            LogError("Cannot find driver for compiler '%1'", spec.cc);
            return nullptr;
        }
    } else if (StartsWith(HostPlatformNames[(int)spec.host], "WASM/Emscripten/")) {
        if (!spec.cc) {
            spec.cc = "emcc";
        }
        if (!IdentifyCompiler(spec.cc, "emcc")) {
            LogError("Only Emcripten (emcc) can be used for WASM cross-compilation");
            return nullptr;
        }

        if (spec.ld) {
            LogError("Cannot use custom linker for host '%1'", HostPlatformNames[(int)spec.host]);
            return nullptr;
        }

        return EmCompiler::Create(spec.host, spec.cc);
#ifdef __linux__
    } else if (spec.host == HostPlatform::Windows) {
        if (!spec.cc) {
            LogError("Path to cross-platform MinGW must be explicitly specified");
            return nullptr;
        }
        if (!IdentifyCompiler(spec.cc, "mingw-w64")) {
            LogError("Only MinGW-w64 can be used for Windows cross-compilation at the moment");
            return nullptr;
        }

        return GnuCompiler::Create(spec.host, spec.cc, spec.ld);
#endif
    } else if (StartsWith(HostPlatformNames[(int)spec.host], "Embedded/Teensy/AVR/")) {
        if (!spec.cc) {
            static std::once_flag flag;
            static char cc[2048];

            std::call_once(flag, []() { FindArduinoCompiler("GCC AVR", "hardware/tools/avr/bin/avr-gcc", cc); });

            if (!cc[0]) {
                LogError("Path to Teensy compiler must be explicitly specified");
                return nullptr;
            }

            spec.cc = cc;
        }

        if (spec.ld) {
            LogError("Cannot use custom linker for host '%1'", HostPlatformNames[(int)spec.host]);
            return nullptr;
        }

        return TeensyCompiler::Create(spec.host, spec.cc);
    } else if (StartsWith(HostPlatformNames[(int)spec.host], "Embedded/Teensy/ARM/")) {
        if (!spec.cc) {
            static std::once_flag flag;
            static char cc[2048];

            std::call_once(flag, []() { FindArduinoCompiler("GCC ARM", "hardware/tools/arm/bin/arm-none-eabi-gcc", cc); });

            if (!cc[0]) {
                LogError("Path to Teensy compiler must be explicitly specified");
                return nullptr;
            }

            spec.cc = cc;
        }

        if (spec.ld) {
            LogError("Cannot use custom linker for host '%1'", HostPlatformNames[(int)spec.host]);
            return nullptr;
        }

        return TeensyCompiler::Create(spec.host, spec.cc);
    } else {
        LogError("Cross-compilation from host '%1' to '%2' is not supported",
                 HostPlatformNames[(int)NativeHost], HostPlatformNames[(int)spec.host]);
        return nullptr;
    }
}

bool DetermineSourceType(const char *filename, SourceType *out_type)
{
    Span<const char> extension = GetPathExtension(filename);

    if (extension == ".c") {
        if (out_type) {
            *out_type = SourceType::C;
        }
        return true;
    } else if (extension == ".cc" || extension == ".cpp") {
        if (out_type) {
            *out_type = SourceType::CXX;
        }
        return true;
    } else {
        return false;
    }
}

static const SupportedCompiler CompilerTable[] = {
#if defined(_WIN32)
    {"Clang", "clang"},
    {"MSVC", "cl"},
    {"GCC", "gcc"},
#elif defined(__linux__)
    {"GCC", "gcc"},
    {"Clang", "clang"},
#else
    {"Clang", "clang"},
    {"GCC", "gcc"},
#endif

    {"EmCC", "emcc"},

    {"Teensy (GCC AVR)"},
    {"Teensy (GCC ARM)"}
};
const Span<const SupportedCompiler> SupportedCompilers = CompilerTable;

}
