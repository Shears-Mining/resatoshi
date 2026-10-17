// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/recycle.h>

#include <coins.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <script/script.h>
#include <undo.h>

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace node::recycle {
namespace {

std::set<COutPoint> ExpiryCandidates(const CCoinsViewCache& view, int origin, ExpirySource source)
{
    const CBlock* origin_block{source.GetBlock()};
    if (!origin_block) return view.GetCoinsByHeight(origin);
    std::set<COutPoint> outputs;
    for (const auto& tx : origin_block->vtx) {
        for (size_t n{0}; n < tx->vout.size(); ++n) {
            if (!tx->vout[n].scriptPubKey.IsUnspendable()) outputs.emplace(tx->GetHash(), static_cast<uint32_t>(n));
        }
    }
    return outputs;
}

CAmount ExpiringValue(const CCoinsViewCache& view, const CBlock& block, int height, int expiry_blocks, ExpirySource source)
{
    const int origin{height - expiry_blocks};
    if (origin < 1) return 0;

    const auto outputs{ExpiryCandidates(view, origin, source)};

    std::set<COutPoint> spent;
    for (const auto& tx : block.vtx) {
        if (tx && !tx->IsCoinBase()) {
            for (const auto& input : tx->vin) spent.insert(input.prevout);
        }
    }

    CAmount amount{0};
    for (const auto& outpoint : outputs) {
        const Coin& coin{view.AccessCoin(outpoint)};
        if (!coin.IsSpent() && coin.nHeight == origin && !spent.contains(outpoint)) amount += coin.out.nValue;
    }
    return amount;
}

// Pure accounting shared by normal connection and idempotent crash replay.
std::optional<CAmount> NextPoolBalance(CAmount available, CAmount coinbase_value, CAmount base_reward)
{
    const CAmount claimed{std::max<CAmount>(0, coinbase_value - base_reward)};
    if (claimed > std::min(MAX_REWARD, available)) return std::nullopt;
    return available - claimed;
}

} // namespace

CAmount PoolBalance(const CCoinsViewCache& view)
{
    return view.GetRecyclePool();
}

CAmount AvailableReward(const CCoinsViewCache& view, const CBlock& block, int height,
                        const Consensus::Params& consensus, ExpirySource source)
{
    return std::min(MAX_REWARD, PoolBalance(view) + ExpiringValue(view, block, height, consensus.recycle_expiry_blocks, source));
}

Error ConnectBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                   CRecycleUndo& undo, const Consensus::Params& consensus,
                   ExpirySource source, std::vector<COutPoint>* expired_outpoints)
{
    const int origin{height - consensus.recycle_expiry_blocks};
    undo = {};
    if (origin < 1) {
        return block.vtx[0]->GetValueOut() > base_reward ? Error::PREMATURE_REWARD : Error::NONE;
    }

    undo.active = true;
    undo.pool_before = PoolBalance(view);
    CAmount expired_value{0};
    for (const auto& outpoint : ExpiryCandidates(view, origin, source)) {
        const Coin& coin{view.AccessCoin(outpoint)};
        if (!coin.IsSpent() && coin.nHeight == origin) {
            Coin expired;
            if (!view.SpendCoin(outpoint, &expired)) return Error::SPEND_FAILED;
            expired_value += expired.out.nValue;
            if (expired_outpoints) expired_outpoints->push_back(outpoint);
            undo.expired.emplace_back(outpoint, std::move(expired));
        }
    }
    const CAmount available{undo.pool_before + expired_value};
    const auto pool{NextPoolBalance(available, block.vtx[0]->GetValueOut(), base_reward)};
    if (!pool) return Error::EXCESSIVE_REWARD;
    view.SetRecyclePool(*pool);
    return Error::NONE;
}

UndoResult DisconnectBlock(CCoinsViewCache& view, int height, const CRecycleUndo* undo, const Consensus::Params& consensus)
{
    if (height <= consensus.recycle_expiry_blocks) return !undo || !undo->active ? UndoResult::CLEAN : UndoResult::FAILED;
    if (!undo || !undo->active || !MoneyRange(undo->pool_before)) return UndoResult::FAILED;
    view.SetRecyclePool(undo->pool_before);
    auto result{UndoResult::CLEAN};
    for (const auto& [outpoint, coin] : undo->expired) {
        // Like transaction-input undo, load the persisted state before restoring.
        // A partial reorg flush may already have restored this output. Marking
        // it FRESH would let a later replay erase only the cache entry and leave
        // the persisted coin (and its height-index key) spendable on disk.
        const bool overwrite{view.HaveCoin(outpoint)};
        view.AddCoin(outpoint, Coin{coin}, overwrite);
        if (overwrite) result = UndoResult::UNCLEAN;
    }
    return result;
}

bool RollforwardBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                      const CRecycleUndo* undo, const Consensus::Params& consensus)
{
    if (height <= consensus.recycle_expiry_blocks) return block.vtx[0]->GetValueOut() <= base_reward;
    if (!undo || !undo->active || !MoneyRange(undo->pool_before)) return false;
    CAmount available{undo->pool_before};
    for (const auto& [outpoint, coin] : undo->expired) {
        if (!MoneyRange(coin.out.nValue) || coin.out.nValue > MAX_MONEY - available) return false;
        available += coin.out.nValue;
    }
    const auto pool{NextPoolBalance(available, block.vtx[0]->GetValueOut(), base_reward)};
    if (!pool) return false;
    for (const auto& [outpoint, coin] : undo->expired) view.SpendCoin(outpoint);
    view.SetRecyclePool(*pool);
    return true;
}

bool ExpectsStateUndo(int height, const Consensus::Params& consensus)
{
    return consensus.recycle_enabled && height > consensus.recycle_expiry_blocks;
}

bool HasExpectedUndo(const CBlockUndo& undo, int height, size_t transaction_count, const Consensus::Params& consensus)
{
    if (transaction_count == 0 || undo.vtxundo.size() != transaction_count - 1) return false;
    if (undo.recycle.active != ExpectsStateUndo(height, consensus)) return false;
    if (!undo.recycle.active) return undo.recycle.pool_before == 0 && undo.recycle.expired.empty();
    if (!MoneyRange(undo.recycle.pool_before)) return false;
    CAmount available{undo.recycle.pool_before};
    // Expiry candidates are ordered; strict order also rejects duplicates
    // without allocating another index while reading large undo records.
    const COutPoint* previous{nullptr};
    for (const auto& [outpoint, coin] : undo.recycle.expired) {
        if (coin.IsSpent() || coin.out.scriptPubKey.IsUnspendable() ||
            coin.nHeight != height - consensus.recycle_expiry_blocks || (previous && !(*previous < outpoint)) ||
            !MoneyRange(coin.out.nValue) || coin.out.nValue > MAX_MONEY - available) return false;
        available += coin.out.nValue;
        previous = &outpoint;
    }
    return true;
}

} // namespace node::recycle
