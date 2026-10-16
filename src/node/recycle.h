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
class CTxUndo;

namespace Consensus { struct Params; }

namespace node::recycle {

inline constexpr int EXPIRY_BLOCKS{5'256'000};
inline constexpr CAmount MAX_REWARD{COIN};

enum class Error {
    NONE,
    PREMATURE_REWARD,
    EXCESSIVE_REWARD,
    SPEND_FAILED,
};

/** Return the recycle pool recorded in the supplied chainstate view. */
CAmount PoolBalance(const CCoinsViewCache& view);

/** Return the maximum recycle reward a candidate block may claim. */
CAmount AvailableReward(const CCoinsViewCache& view, const CBlock& block, int height, int expiry_blocks = EXPIRY_BLOCKS, const CBlock* origin_block = nullptr);

/** Apply UTXO expiry and pool accounting, returning Error::NONE on success.
 * A supplied origin block replaces height-index lookup.
 */
Error ConnectBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                   CTxUndo& undo, std::vector<COutPoint>* expired = nullptr, int expiry_blocks = EXPIRY_BLOCKS, const CBlock* origin_block = nullptr);

/** Undo ConnectBlock. */
bool DisconnectBlock(CCoinsViewCache& view, int height, CTxUndo* undo, int expiry_blocks = EXPIRY_BLOCKS);

/** Reapply the state transition while recovering an interrupted chainstate flush. */
bool RollforwardBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                      const CTxUndo* undo, int expiry_blocks = EXPIRY_BLOCKS);

/** Read the existing undo payload for index accounting. */
bool ReadUndo(const CTxUndo& undo, CAmount& pool_before, std::vector<std::pair<COutPoint, Coin>>& expired);

/** Whether the network rules require an internal state undo entry at this height.
 * Does not inspect or modify undo data.
 */
bool ExpectsStateUndo(int height, const Consensus::Params& consensus);

/** Check the exact transaction/state undo count without modifying the records. */
bool HasExpectedUndoCount(const CBlockUndo& undo, int height, size_t transaction_count, const Consensus::Params& consensus);

/** Remove internal state from a copy of block undo before exposing transaction inputs.
 * Returns false, without modifying undo, if its entry count is inconsistent.
 */
bool RemoveStateUndo(CBlockUndo& undo, int height, size_t transaction_count, const Consensus::Params& consensus);

} // namespace node::recycle

#endif // BITCOIN_NODE_RECYCLE_H
