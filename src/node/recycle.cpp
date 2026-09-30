// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/recycle.h>
#include <node/recycle_serialization.h>

#include <coins.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <undo.h>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace node::recycle {
namespace {

const Txid& ReservedTxid()
{
    static const Txid txid{};
    return txid;
}

COutPoint PoolOutpoint()
{
    return COutPoint{ReservedTxid(), std::numeric_limits<uint32_t>::max()};
}

struct UndoData {
    CAmount pool_before{0};
    // Preserve the old undo wire format so pre-upgrade blocks can disconnect.
    // New undo records carry only this constant-size placeholder.
    Coin schedule{CTxOut{0, CScript{OP_RETURN}}, 0, false};
    std::vector<std::pair<COutPoint, Coin>> expired;

    SERIALIZE_METHODS(UndoData, obj) { READWRITE(obj.pool_before, Using<StateCoinFormatter>(obj.schedule), obj.expired); }
};

CScript Encode(const auto& value)
{
    DataStream stream{};
    stream << value;
    CScript script{OP_RETURN};
    script.insert(script.end(), UCharCast(stream.data()), UCharCast(stream.data() + stream.size()));
    return script;
}

template <typename T>
bool Decode(const CScript& script, T& value)
{
    if (script.empty() || script[0] != OP_RETURN) return false;
    try {
        DataStream stream{std::span{script}.subspan(1)};
        stream >> value;
        return stream.empty();
    } catch (const std::exception&) {
        return false;
    }
}

Coin StateCoin(CAmount amount, CScript script, int height)
{
    return Coin{CTxOut{amount, std::move(script)}, height, false};
}

void PutStateCoin(CCoinsViewCache& view, const COutPoint& outpoint, Coin coin)
{
    view.SpendCoin(outpoint);
    view.AddCoin(outpoint, std::move(coin), /*possible_overwrite=*/false,
                 /*allow_unspendable=*/true);
}

CAmount ExpiringValue(const CCoinsViewCache& view, const CBlock& block, int height, int expiry_blocks)
{
    const int origin{height - expiry_blocks};
    if (origin < 1) return 0;

    const auto outputs{view.GetCoinsByHeight(origin)};

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

bool Apply(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
           CTxUndo* undo, std::string& error, std::vector<COutPoint>* expired_outpoints, int expiry_blocks)
{
    UndoData undo_data;
    undo_data.pool_before = PoolBalance(view);

    CAmount expired_value{0};
    const int origin{height - expiry_blocks};
    if (origin < 1) {
        if (block.vtx[0]->GetValueOut() > base_reward) {
            error = "coinbase claims recycle reward before any output can expire";
            return false;
        }
        return true;
    }
    {
        const auto outputs{view.GetCoinsByHeight(origin)};
        for (const auto& outpoint : outputs) {
            const Coin& coin{view.AccessCoin(outpoint)};
            if (!coin.IsSpent() && coin.nHeight == origin) {
                Coin expired;
                if (!view.SpendCoin(outpoint, &expired)) {
                    error = "recycle expiry: SpendCoin failed";
                    return false;
                }
                expired_value += expired.out.nValue;
                if (expired_outpoints) expired_outpoints->push_back(outpoint);
                undo_data.expired.emplace_back(outpoint, std::move(expired));
            }
        }
    }

    const CAmount available{undo_data.pool_before + expired_value};
    const CAmount claimed{std::max<CAmount>(0, block.vtx[0]->GetValueOut() - base_reward)};
    if (claimed > std::min(MAX_REWARD, available)) {
        error = "coinbase claims excessive recycle reward";
        return false;
    }

    PutStateCoin(view, PoolOutpoint(), StateCoin(available - claimed, CScript{OP_RETURN}, height));

    if (undo) undo->vprevout.emplace_back(StateCoin(0, Encode(undo_data), height));
    return true;
}

} // namespace

CAmount PoolBalance(const CCoinsViewCache& view)
{
    const Coin& coin{view.AccessCoin(PoolOutpoint())};
    return coin.IsSpent() ? 0 : coin.out.nValue;
}

CAmount AvailableReward(const CCoinsViewCache& view, const CBlock& block, int height, int expiry_blocks)
{
    return std::min(MAX_REWARD, PoolBalance(view) + ExpiringValue(view, block, height, expiry_blocks));
}

bool ConnectBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                  CTxUndo& undo, std::string& error, std::vector<COutPoint>* expired, int expiry_blocks)
{
    return Apply(view, block, height, base_reward, &undo, error, expired, expiry_blocks);
}

bool DisconnectBlock(CCoinsViewCache& view, int height, CTxUndo* undo, int expiry_blocks)
{
    if (height - expiry_blocks < 1) return true;
    if (!undo || undo->vprevout.size() != 1) return false;
    UndoData data;
    if (!Decode(undo->vprevout[0].out.scriptPubKey, data)) return false;

    if (height - expiry_blocks == 1) {
        view.SpendCoin(PoolOutpoint());
    } else {
        PutStateCoin(view, PoolOutpoint(), StateCoin(data.pool_before, CScript{OP_RETURN}, height - 1));
    }
    for (auto& [outpoint, coin] : data.expired) view.AddCoin(outpoint, std::move(coin), false);
    return true;
}

bool RollforwardBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                      const CTxUndo* undo, int expiry_blocks)
{
    const int origin{height - expiry_blocks};
    if (origin < 1) {
        std::string error;
        return Apply(view, block, height, base_reward, nullptr, error, nullptr, expiry_blocks);
    }
    if (!undo || undo->vprevout.size() != 1) return false;

    UndoData data;
    if (!Decode(undo->vprevout[0].out.scriptPubKey, data)) return false;
    CAmount expired_value{0};
    for (const auto& [outpoint, coin] : data.expired) expired_value += coin.out.nValue;

    const CAmount available{data.pool_before + expired_value};
    const CAmount claimed{std::max<CAmount>(0, block.vtx[0]->GetValueOut() - base_reward)};
    if (claimed > std::min(MAX_REWARD, available)) return false;

    for (const auto& [outpoint, coin] : data.expired) view.SpendCoin(outpoint);
    PutStateCoin(view, PoolOutpoint(), StateCoin(available - claimed, CScript{OP_RETURN}, height));
    return true;
}

bool ReadUndo(const CTxUndo& undo, CAmount& pool_before, std::vector<std::pair<COutPoint, Coin>>& expired)
{
    UndoData data;
    if (undo.vprevout.size() != 1 || !Decode(undo.vprevout[0].out.scriptPubKey, data)) return false;
    pool_before = data.pool_before;
    expired = std::move(data.expired);
    return true;
}

bool ExpectsStateUndo(int height, const Consensus::Params& consensus)
{
    return consensus.recycle_enabled && height > consensus.recycle_expiry_blocks;
}

bool RemoveStateUndo(CBlockUndo& undo, int height, size_t transaction_count, const Consensus::Params& consensus)
{
    if (transaction_count == 0) return false;
    const bool has_state{ExpectsStateUndo(height, consensus)};
    const size_t expected{transaction_count - 1 + size_t{has_state}};
    if (undo.vtxundo.size() != expected) return false;
    if (has_state) undo.vtxundo.pop_back();
    return true;
}

} // namespace node::recycle
