// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_RECYCLE_H
#define BITCOIN_NODE_RECYCLE_H

#include <consensus/amount.h>
#include <primitives/transaction.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

class CBlock;
class CBlockUndo;
class Coin;
class CCoinsViewCache;
struct CRecycleUndo;

namespace Consensus { struct Params; }

namespace node::recycle {

inline constexpr CAmount MAX_REWARD{COIN};

enum class Error {
    NONE,
    PREMATURE_REWARD,
    EXCESSIVE_REWARD,
    SPEND_FAILED,
};

/** Return the recycle pool recorded in the supplied chainstate view. */
CAmount PoolBalance(const CCoinsViewCache& view);

/** Maximum claim after candidate transactions and expiry. */
CAmount AvailableReward(const CCoinsViewCache& view, const CBlock& block, int height,
                        const Consensus::Params& consensus, const CBlock* origin_block = nullptr);

/** Apply expiry using the height index, or the supplied origin block. */
Error ConnectBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                   CRecycleUndo& undo, const Consensus::Params& consensus,
                   std::vector<COutPoint>* expired = nullptr, const CBlock* origin_block = nullptr);

bool DisconnectBlock(CCoinsViewCache& view, int height, const CRecycleUndo* undo, const Consensus::Params& consensus);

/** Idempotent replay of an interrupted, possibly partial chainstate flush. */
bool RollforwardBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                      const CRecycleUndo* undo, const Consensus::Params& consensus);

bool ExpectsStateUndo(int height, const Consensus::Params& consensus);

/** Validate transaction count and the separate expiry payload before mutation. */
bool HasExpectedUndo(const CBlockUndo& undo, int height, size_t transaction_count, const Consensus::Params& consensus);

} // namespace node::recycle

#endif // BITCOIN_NODE_RECYCLE_H
