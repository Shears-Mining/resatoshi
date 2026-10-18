// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H
#define BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H

#include <util/fs.h>
#include <util/result.h>
#include <validation.h>

#include <string>

class ArgsManager;

/** -par default (number of script-checking threads, 0 = auto) */
static constexpr int DEFAULT_SCRIPTCHECK_THREADS{0};

namespace node {
/** Existing chainstate or block-index directories; used to skip first-run GUI setup. */
bool HasChainstateData(const fs::path& datadir);
/** Command-line/config settings, excluding saved GUI preferences. */
bool HasExplicitStartupSetting(const ArgsManager& args, const std::string& name);

/** Explicit CLI/config mode, then saved settings, then index. */
std::string GetRecycleMode(const ArgsManager& args);

[[nodiscard]] util::Result<void> ApplyArgsManOptions(const ArgsManager& args, ChainstateManager::Options& opts);
} // namespace node

#endif // BITCOIN_NODE_CHAINSTATEMANAGER_ARGS_H
