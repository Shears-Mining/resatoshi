// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H
#define BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H

#include <util/fs.h>
#include <util/result.h>
#include <validation.h>

#include <string>
#include <string_view>

class ArgsManager;

/** -par default (number of script-checking threads, 0 = auto) */
static constexpr int DEFAULT_SCRIPTCHECK_THREADS{0};

namespace node {
/** Recorded expiry mode, or an existing directory missing its mode record. */
enum class RecycleDirectoryState { NEW, EXISTING_UNMARKED, INDEX, BLOCK, INVALID };
RecycleDirectoryState GetRecycleDirectoryState(const fs::path& datadir);
/** Command-line/config settings, excluding saved GUI preferences. */
bool HasExplicitStartupSetting(const ArgsManager& args, const std::string& name);
/** Explicit configuration wins; otherwise an existing folder owns its mode. */
std::string GetRecycleMode(const ArgsManager& args);
/** Persist complete marker contents by committing a temporary file and renaming. */
[[nodiscard]] bool WriteRecycleMarker(const fs::path& path, std::string_view contents);

[[nodiscard]] util::Result<void> ApplyArgsManOptions(const ArgsManager& args, ChainstateManager::Options& opts);
} // namespace node

#endif // BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H
