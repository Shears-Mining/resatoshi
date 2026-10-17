// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstatemanager_args.h>

#include <arith_uint256.h>
#include <common/args.h>
#include <common/system.h>
#include <logging.h>
#include <node/coins_view_args.h>
#include <node/database_args.h>
#include <tinyformat.h>
#include <uint256.h>
#include <univalue.h>
#include <util/fs_helpers.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

namespace node {
RecycleDirectoryState GetRecycleDirectoryState(const fs::path& datadir)
{
    const auto marker{datadir / "recycle-mode"};
    if (fs::exists(marker)) {
        std::ifstream stream{marker.std_path()};
        std::string mode;
        std::getline(stream, mode);
        if (stream && mode == "index") return RecycleDirectoryState::INDEX;
        if (stream && mode == "block") return RecycleDirectoryState::BLOCK;
        return RecycleDirectoryState::INVALID;
    }
    if (fs::exists(datadir / "chainstate" / "CURRENT") || fs::exists(datadir / "blocks" / "index" / "CURRENT")) {
        return RecycleDirectoryState::EXISTING_UNMARKED;
    }
    return RecycleDirectoryState::NEW;
}

bool HasExplicitStartupSetting(const ArgsManager& args, const std::string& name)
{
    bool supplied{false};
    const auto network{args.GetChainTypeString()};
    args.LockSettings([&](const common::Settings& settings) {
        auto external{settings};
        external.rw_settings.clear();
        supplied = !common::GetSetting(external, network, name, /*ignore_default_section_config=*/false,
                                       /*ignore_nonpersistent=*/false, /*get_chain_type=*/false).isNull();
    });
    return supplied;
}

std::string GetRecycleMode(const ArgsManager& args)
{
    if (!HasExplicitStartupSetting(args, "recyclemode")) {
        const auto state{GetRecycleDirectoryState(args.GetDataDirNet())};
        if (state == RecycleDirectoryState::INDEX) return "index";
        if (state == RecycleDirectoryState::BLOCK) return "block";
    }
    return args.GetArg("-recyclemode", "index");
}

bool WriteRecycleMarker(const fs::path& path, std::string_view contents)
{
    auto temporary{path};
    temporary += ".tmp";
    FILE* file{fsbridge::fopen(temporary, "wb")};
    if (!file) return false;
    const bool written{std::fwrite(contents.data(), 1, contents.size(), file) == contents.size() && FileCommit(file)};
    const bool closed{std::fclose(file) == 0};
    if (!written || !closed || !RenameOver(temporary, path)) return false;
    DirectoryCommit(path.parent_path());
    return true;
}

util::Result<void> ApplyArgsManOptions(const ArgsManager& args, ChainstateManager::Options& opts)
{
    const auto recycle_mode{GetRecycleMode(args)};
    if (recycle_mode != "index" && recycle_mode != "block") {
        return util::Error{Untranslated("Invalid -recyclemode; use index or block")};
    }
    opts.recycle_from_block = recycle_mode == "block";
    const auto interval{args.GetIntArg("-recyclecheckinterval", 3600)};
    if (interval < 1 || interval > 86400 * 365) return util::Error{Untranslated("Invalid -recyclecheckinterval; use 1..31536000 seconds")};
    opts.recycle_check_interval = std::chrono::seconds{interval};

    if (auto value{args.GetIntArg("-checkblockindex")}) {
        // Interpret bare -checkblockindex argument as 1 instead of 0.
        opts.check_block_index = args.GetArg("-checkblockindex")->empty() ? 1 : *value;
    }

    if (auto value{args.GetArg("-minimumchainwork")}) {
        if (auto min_work{uint256::FromUserHex(*value)}) {
            opts.minimum_chain_work = UintToArith256(*min_work);
        } else {
            return util::Error{Untranslated(strprintf("Invalid minimum work specified (%s), must be up to %d hex digits", *value, uint256::size() * 2))};
        }
    }

    if (auto value{args.GetArg("-assumevalid")}) {
        if (auto block_hash{uint256::FromUserHex(*value)}) {
            opts.assumed_valid_block = *block_hash;
        } else {
            return util::Error{Untranslated(strprintf("Invalid assumevalid block hash specified (%s), must be up to %d hex digits (or 0 to disable)", *value, uint256::size() * 2))};
        }
    }

    if (auto value{args.GetIntArg("-maxtipage")}) opts.max_tip_age = std::chrono::seconds{*value};

    ReadDatabaseArgs(args, opts.coins_db);
    ReadCoinsViewArgs(args, opts.coins_view);

    int script_threads = args.GetIntArg("-par", DEFAULT_SCRIPTCHECK_THREADS);
    if (script_threads <= 0) {
        // -par=0 means autodetect (number of cores - 1 script threads)
        // -par=-n means "leave n cores free" (number of cores - n - 1 script threads)
        script_threads += GetNumCores();
    }
    // Subtract 1 because the main thread counts towards the par threads.
    opts.worker_threads_num = script_threads - 1;

    if (auto max_size = args.GetIntArg("-maxsigcachesize")) {
        // 1. When supplied with a max_size of 0, both the signature cache and
        //    script execution cache create the minimum possible cache (2
        //    elements). Therefore, we can use 0 as a floor here.
        // 2. Multiply first, divide after to avoid integer truncation.
        size_t clamped_size_each = std::max<int64_t>(*max_size, 0) * (1 << 20) / 2;
        opts.script_execution_cache_bytes = clamped_size_each;
        opts.signature_cache_bytes = clamped_size_each;
    }

    return {};
}
} // namespace node
