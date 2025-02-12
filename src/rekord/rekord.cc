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
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "src/core/libcc/libcc.hh"
#include "disk.hh"
#include "repository.hh"
#include "src/core/libnet/s3.hh"
#include "src/core/libpasswd/libpasswd.hh"
#include "vendor/libsodium/src/libsodium/include/sodium.h"
#include "vendor/curl/include/curl/curl.h"
#ifndef _WIN32
    #include <sys/time.h>
    #include <sys/resource.h>
#endif

namespace RG {

static const char *FillRepository(const char *repository)
{
    if (!repository) {
        repository = GetQualifiedEnv("REPOSITORY");

        if (!repository) {
            LogError("Missing repository directory");
            return nullptr;
        }
    }

    return repository;
}

static const char *FillPassword(const char *pwd, Allocator *alloc)
{
    if (!pwd) {
        pwd = GetQualifiedEnv("PASSWORD");

        if (!pwd) {
            pwd = Prompt("Repository password: ", nullptr, "*", alloc);
        }
    }

    return pwd;
}

static bool LooksLikeURL(const char *str)
{
    bool ret = StartsWith(str, "https://") || StartsWith(str, "http://");
    return ret;
}

static std::unique_ptr<rk_Disk> OpenRepository(const char *repository, const char *pwd, int threads = -1)
{
    std::unique_ptr<rk_Disk> disk;
    if (LooksLikeURL(repository)) {
        s3_Config config;
        if (!s3_DecodeURL(repository, &config))
            return nullptr;

        disk = rk_OpenS3Disk(config, pwd);
    } else {
        if (!PathIsAbsolute(repository)) {
            LogError("Repository path '%1' is not absolute", repository);
            return nullptr;
        }

        disk = rk_OpenLocalDisk(repository, pwd);
    }
    if (!disk)
        return nullptr;

    if (threads >= 0) {
        disk->SetThreads(threads);
    }

    return disk;
}

static int RunInit(Span<const char *> arguments)
{
    // Options
    const char *repository = nullptr;

    const auto print_usage = [=](FILE *fp) {
        PrintLn(fp,
R"(Usage: %!..+%1 init <dir>%!0)", FelixTarget);
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else {
                opt.LogUnknownError();
                return 1;
            }
        }

        repository = opt.ConsumeNonOption();
    }

    repository = FillRepository(repository);
    if (!repository)
        return 1;

    // Generate repository passwords
    char full_pwd[33] = {};
    char write_pwd[33] = {};
    if (!pwd_GeneratePassword(full_pwd))
        return 1;
    if (!pwd_GeneratePassword(write_pwd))
        return 1;

    std::unique_ptr<rk_Disk> disk;
    if (LooksLikeURL(repository)) {
        s3_Config config;
        if (!s3_DecodeURL(repository, &config))
            return 1;

        disk = rk_OpenS3Disk(config);
    } else {
        disk = rk_OpenLocalDisk(repository);
    }
    if (!disk)
        return 1;
    if (!disk->Init(full_pwd, write_pwd))
        return 1;

    LogInfo("Repository: %!..+%1%!0", disk->GetURL());
    LogInfo();
    LogInfo("Default full password: %!..+%1%!0", full_pwd);
    LogInfo("  write-only password: %!..+%1%!0", write_pwd);
    LogInfo();
    LogInfo("Please write them down, they cannot be recovered and the backup will be lost if you lose them.");

    return 0;
}

static int RunPut(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    // Options
    rk_PutSettings settings;
    int threads = rk_ComputeDefaultThreads();
    const char *repository = nullptr;
    const char *pwd = nullptr;
    HeapArray<const char *> filenames;

    const auto print_usage = [=](FILE *fp) {
        PrintLn(fp,
R"(Usage: %!..+%1 put [-R <repo>] <filename> ...%!0

Options:
    %!..+-R, --repository <dir>%!0       Set repository directory
        %!..+--password <pwd>%!0         Set repository password

    %!..+-n, --name <name>%!0            Set user friendly name (optional)

        %!..+--follow_symlinks%!0        Follow symbolic links (instead of storing them as-is)
        %!..+--raw%!0                    Skip snapshot object and report data ID

    %!..+-j, --threads <threads>%!0      Change number of threads
                                 %!D..(default: %2)%!0)", FelixTarget, threads);
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-R", "--repository", OptionType::Value)) {
                repository = opt.current_value;
            } else if (opt.Test("--password", OptionType::Value)) {
                pwd = opt.current_value;
            } else if (opt.Test("-n", "--name", OptionType::Value)) {
                settings.name = opt.current_value;
            } else if (opt.Test("--follow_symlinks")) {
                settings.follow_symlinks = true;
            } else if (opt.Test("--raw")) {
                settings.raw = true;
            } else if (opt.Test("-j", "--threads", OptionType::Value)) {
                if (!ParseInt(opt.current_value, &threads))
                    return 1;
                if (threads < 1) {
                    LogError("Threads count cannot be < 1");
                    return 1;
                }
            } else {
                opt.LogUnknownError();
                return 1;
            }
        }

        opt.ConsumeNonOptions(&filenames);
    }

    if (!filenames.len) {
        LogError("No filename provided");
        return 1;
    }
    repository = FillRepository(repository);
    if (!repository)
        return 1;
    pwd = FillPassword(pwd, &temp_alloc);
    if (!pwd)
        return 1;

    std::unique_ptr<rk_Disk> disk = OpenRepository(repository, pwd, threads);
    if (!disk)
        return 1;

    LogInfo("Repository: %!..+%1%!0 (%2)", disk->GetURL(), rk_DiskModeNames[(int)disk->GetMode()]);
    if (disk->GetMode() != rk_DiskMode::WriteOnly) {
        LogWarning("You should use the write-only key with this command");
    }

    LogInfo();
    LogInfo("Backing up...");

    int64_t now = GetMonotonicTime();

    rk_ID id = {};
    int64_t total_len = 0;
    int64_t total_written = 0;
    if (!rk_Put(disk.get(), settings, filenames, &id, &total_len, &total_written))
        return 1;

    double time = (double)(GetMonotonicTime() - now) / 1000.0;

    LogInfo();
    LogInfo("%1 ID: %!..+%2%!0", settings.raw ? "Data" : "Snapshot", id);
    LogInfo("Stored size: %!..+%1%!0", FmtDiskSize(total_len));
    LogInfo("Total written: %!..+%1%!0", FmtDiskSize(total_written));
    LogInfo("Execution time: %!..+%1s%!0", FmtDouble(time, 1));

    return 0;
}

static int RunGet(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    // Options
    rk_GetSettings settings;
    int threads = rk_ComputeDefaultThreads();
    const char *repository = nullptr;
    const char *pwd = nullptr;
    const char *dest_filename = nullptr;
    const char *name = nullptr;

    const auto print_usage = [=](FILE *fp) {
        PrintLn(fp,
R"(Usage: %!..+%1 get [-R <repo>] <ID> -O <path>%!0

Options:
    %!..+-R, --repository <dir>%!0       Set repository directory
        %!..+--password <pwd>%!0         Set repository password

    %!..+-O, --output <path>%!0          Restore file or directory to path
        %!..+--flat%!0                   Use flat names for snapshot files

    %!..+-j, --threads <threads>%!0      Change number of threads
                                 %!D..(default: %2)%!0)", FelixTarget, threads);
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-R", "--repository", OptionType::Value)) {
                repository = opt.current_value;
            } else if (opt.Test("--password", OptionType::Value)) {
                pwd = opt.current_value;
            } else if (opt.Test("-O", "--output", OptionType::Value)) {
                dest_filename = opt.current_value;
            } else if (opt.Test("--flat")) {
                settings.flat = true;
            } else if (opt.Test("-j", "--threads", OptionType::Value)) {
                if (!ParseInt(opt.current_value, &threads))
                    return 1;
                if (threads < 1) {
                    LogError("Threads count cannot be < 1");
                    return 1;
                }
            } else {
                opt.LogUnknownError();
                return 1;
            }
        }

        name = opt.ConsumeNonOption();
    }

    if (!name) {
        LogError("No name provided");
        return 1;
    }
    if (!dest_filename) {
        LogError("Missing destination filename");
        return 1;
    }
    repository = FillRepository(repository);
    if (!repository)
        return 1;
    pwd = FillPassword(pwd, &temp_alloc);
    if (!pwd)
        return 1;

    std::unique_ptr<rk_Disk> disk = OpenRepository(repository, pwd, threads);
    if (!disk)
        return 1;

    LogInfo("Repository: %!..+%1%!0 (%2)", disk->GetURL(), rk_DiskModeNames[(int)disk->GetMode()]);
    if (disk->GetMode() != rk_DiskMode::ReadWrite) {
        LogError("Cannot decrypt with write-only key");
        return 1;
    }

    LogInfo();
    LogInfo("Extracting...");

    int64_t now = GetMonotonicTime();

    int64_t file_len = 0;
    {
        rk_ID id = {};
        if (!rk_ParseID(name, &id))
            return 1;
        if (!rk_Get(disk.get(), id, settings, dest_filename, &file_len))
            return 1;
    }

    double time = (double)(GetMonotonicTime() - now) / 1000.0;

    LogInfo();
    LogInfo("Restored: %!..+%1%!0 (%2)", dest_filename, FmtDiskSize(file_len));
    LogInfo("Execution time: %!..+%1s%!0", FmtDouble(time, 1));

    return 0;
}

static int RunList(Span<const char *> arguments)
{
    BlockAllocator temp_alloc;

    // Options
    int threads = rk_ComputeDefaultThreads();
    const char *repository = nullptr;
    const char *pwd = nullptr;

    const auto print_usage = [=](FILE *fp) {
        PrintLn(fp,
R"(Usage: %!..+%1 list [-R <repo>]%!0

Options:
    %!..+-R, --repository <dir>%!0       Set repository directory
        %!..+--password <pwd>%!0         Set repository password

    %!..+-j, --threads <threads>%!0      Change number of threads
                                 %!D..(default: %2)%!0)", FelixTarget, threads);
    };

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-R", "--repository", OptionType::Value)) {
                repository = opt.current_value;
            } else if (opt.Test("--password", OptionType::Value)) {
                pwd = opt.current_value;
            } else if (opt.Test("-j", "--threads", OptionType::Value)) {
                if (!ParseInt(opt.current_value, &threads))
                    return 1;
                if (threads < 1) {
                    LogError("Threads count cannot be < 1");
                    return 1;
                }
            } else {
                opt.LogUnknownError();
                return 1;
            }
        }
    }

    repository = FillRepository(repository);
    if (!repository)
        return 1;
    pwd = FillPassword(pwd, &temp_alloc);
    if (!pwd)
        return 1;

    std::unique_ptr<rk_Disk> disk = OpenRepository(repository, pwd, threads);
    if (!disk)
        return 1;

    LogInfo("Repository: %!..+%1%!0 (%2)", disk->GetURL(), rk_DiskModeNames[(int)disk->GetMode()]);
    if (disk->GetMode() != rk_DiskMode::ReadWrite) {
        LogError("Cannot list with write-only key");
        return 1;
    }
    LogInfo();

    HeapArray<rk_SnapshotInfo> snapshots;
    if (!rk_List(disk.get(), &temp_alloc, &snapshots))
        return 1;

    if (snapshots.len) {
        for (const rk_SnapshotInfo &snapshot: snapshots) {
            TimeSpec spec = DecomposeTime(snapshot.time);

            PrintLn("%!..+%1%!0", snapshot.id);
            if (snapshot.name) {
                PrintLn("+ Name: %!..+%1%!0", snapshot.name);
            }
            PrintLn("+ Time: %!..+%1%!0", FmtTimeNice(spec));
            PrintLn("+ Size: %!..+%1%!0", FmtDiskSize(snapshot.len));
            PrintLn("+ Storage: %!..+%1%!0", FmtDiskSize(snapshot.stored));
            PrintLn();
        }
    } else {
        LogInfo("There does not seem to be any snapshot");
    }

    return 0;
}

int Main(int argc, char **argv)
{
    RG_CRITICAL(argc >= 1, "First argument is missing");

    const auto print_usage = [](FILE *fp) {
        PrintLn(fp, R"(Usage: %!..+%1 <command> [args]%!0

Commands:
    %!..+init%!0                         Init new backup repository

    %!..+put%!0                          Store encrypted directory or file
    %!..+get%!0                          Get and decrypt directory or file

    %!..+list%!0                         List snapshots

Use %!..+%1 help <command>%!0 or %!..+%1 <command> --help%!0 for more specific help.)", FelixTarget);
    };

    if (argc < 2) {
        print_usage(stderr);
        PrintLn(stderr);
        LogError("No command provided");
        return 1;
    }

#ifndef _WIN32
    {
        const rlim_t max_nofile = 4096;
        struct rlimit lim;

        // Increase maximum number of open file descriptors
        if (getrlimit(RLIMIT_NOFILE, &lim) >= 0) {
            if (lim.rlim_cur < max_nofile) {
                lim.rlim_cur = std::min(max_nofile, lim.rlim_max);

                if (setrlimit(RLIMIT_NOFILE, &lim) >= 0) {
                    if (lim.rlim_cur < max_nofile) {
                        LogError("Maximum number of open descriptors is low: %1 (recommended: %2)", lim.rlim_cur, max_nofile);
                    }
                } else {
                    LogError("Could not raise RLIMIT_NOFILE to %1: %2", max_nofile, strerror(errno));
                }
            }
        } else {
            LogError("getrlimit(RLIMIT_NOFILE) failed: %1", strerror(errno));
        }
    }
#endif

    if (sodium_init() < 0) {
        LogError("Failed to initialize libsodium");
        return 1;
    }
    if (curl_global_init(CURL_GLOBAL_ALL)) {
        LogError("Failed to initialize libcurl");
        return 1;
    }

    const char *cmd = argv[1];
    Span<const char *> arguments((const char **)argv + 2, argc - 2);

    // Handle help and version arguments
    if (TestStr(cmd, "--help") || TestStr(cmd, "help")) {
        if (arguments.len && arguments[0][0] != '-') {
            cmd = arguments[0];
            arguments[0] = (cmd[0] == '-') ? cmd : "--help";
        } else {
            print_usage(stdout);
            return 0;
        }
    } else if (TestStr(cmd, "--version")) {
        PrintLn("%!R..%1%!0 %!..+%2%!0", FelixTarget, FelixVersion);
        PrintLn("Compiler: %1", FelixCompiler);
        return 0;
    }

    if (TestStr(cmd, "init")) {
        return RunInit(arguments);
    } else if (TestStr(cmd, "put")) {
        return RunPut(arguments);
    } else if (TestStr(cmd, "get")) {
        return RunGet(arguments);
    } else if (TestStr(cmd, "list")) {
        return RunList(arguments);
    } else {
        LogError("Unknown command '%1'", cmd);
        return 1;
    }
}

}

// C++ namespaces are stupid
int main(int argc, char **argv) { return RG::Main(argc, argv); }
