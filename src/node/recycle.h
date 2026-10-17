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

/** Callers must explicitly select the candidate source; block mode requires a block. */
class ExpirySource
{
    const CBlock* m_block;
    explicit ExpirySource(const CBlock* block) : m_block{block} {}
public:
    static ExpirySource Index() { return ExpirySource{nullptr}; }
    static ExpirySource Block(const CBlock& block) { return ExpirySource{&block}; }
    const CBlock* GetBlock() const { return m_block; }
};

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
                        const Consensus::Params& consensus, ExpirySource source);

/** Apply expiry using the height index, or the supplied origin block. */
Error ConnectBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                   CRecycleUndo& undo, const Consensus::Params& consensus,
                   ExpirySource source, std::vector<COutPoint>* expired = nullptr);

/** An overwrite is unclean during normal disconnect, but expected in crash replay. */
enum class UndoResult { CLEAN, UNCLEAN, FAILED };
UndoResult DisconnectBlock(CCoinsViewCache& view, int height, const CRecycleUndo* undo, const Consensus::Params& consensus);

/** Idempotent replay of an interrupted, possibly partial chainstate flush. */
bool RollforwardBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                      const CRecycleUndo* undo, const Consensus::Params& consensus);

bool ExpectsStateUndo(int height, const Consensus::Params& consensus);

/** Validate transaction count and the separate expiry payload before mutation. */
bool HasExpectedUndo(const CBlockUndo& undo, int height, size_t transaction_count, const Consensus::Params& consensus);

} // namespace node::recycle

#endif // BITCOIN_NODE_RECYCLE_H
