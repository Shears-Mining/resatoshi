// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/params.h>
#include <node/recycle.h>
#include <node/recycle_serialization.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <primitives/block.h>
#include <script/script.h>
#include <undo.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

#include <boost/test/unit_test.hpp>

namespace {

CBlock CoinbaseBlock(CAmount value, int unique)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.SetNull();
    tx.vin[0].scriptSig = CScript{} << unique;
    tx.vout.emplace_back(value, CScript{} << OP_TRUE);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    return block;
}

// Deliberately bypass the normal atomic coin/index writes to model disk damage.
struct TestHeightEntry {
    uint8_t prefix{'E'};
    uint32_t height{1};
    COutPoint outpoint;
    SERIALIZE_METHODS(TestHeightEntry, obj) { READWRITE(obj.prefix, obj.height, obj.outpoint); }
};

struct TestCoinEntry {
    uint8_t prefix{'C'};
    COutPoint outpoint;
    SERIALIZE_METHODS(TestCoinEntry, obj) { READWRITE(obj.prefix, obj.outpoint.hash, VARINT(obj.outpoint.n)); }
};

std::unique_ptr<RecycleIndexSnapshot> CheckSnapshot(CCoinsViewDB& db)
{
    LOCK(cs_main);
    return db.GetRecycleIndexSnapshot();
}

bool ContinueCheck(const std::string&, const RecycleIndexCheckResult&) { return true; }

} // namespace

BOOST_AUTO_TEST_SUITE(recycle_tests)

BOOST_AUTO_TEST_CASE(transaction_undo_excludes_recycle_state)
{
    Consensus::Params consensus;
    consensus.recycle_enabled = true;
    consensus.recycle_expiry_blocks = 200;
    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(2 * COIN, 1)};
    AddCoins(view, *origin.vtx[0], 1);
    CTxUndo early;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, 1, 2 * COIN, early, error, nullptr, 200));

    // Before expiry a coinbase-only block has no input records.
    CBlockUndo before;
    BOOST_REQUIRE(node::recycle::RemoveStateUndo(before, 200, 1, consensus));
    BOOST_CHECK(before.vtxundo.empty());

    CBlock expiry{CoinbaseBlock(COIN, 201)};
    AddCoins(view, *expiry.vtx[0], 201);
    CTxUndo state;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, 201, 0, state, error, nullptr, 200));
    CBlockUndo disk;
    disk.vtxundo.push_back(state);
    CBlockUndo exposed{disk};
    BOOST_REQUIRE(node::recycle::RemoveStateUndo(exposed, 201, 1, consensus));
    BOOST_CHECK(exposed.vtxundo.empty());

    // Keep every field of a real input record, even when its transaction
    // follows the coinbase and precedes the internal state entry.
    CTxUndo transaction;
    transaction.vprevout.emplace_back(origin.vtx[0]->vout[0], 1, true);
    disk.vtxundo.insert(disk.vtxundo.begin(), transaction);
    exposed = disk;
    BOOST_REQUIRE(node::recycle::RemoveStateUndo(exposed, 201, 2, consensus));
    BOOST_REQUIRE_EQUAL(exposed.vtxundo.size(), 1U);
    BOOST_REQUIRE_EQUAL(exposed.vtxundo[0].vprevout.size(), 1U);
    const auto& input{exposed.vtxundo[0].vprevout[0]};
    BOOST_CHECK(input.out == origin.vtx[0]->vout[0]);
    BOOST_CHECK_EQUAL(input.nHeight, 1U);
    BOOST_CHECK(input.fCoinBase);

    // No mutation on inconsistent input, and no stripping on other networks.
    BOOST_CHECK(!node::recycle::RemoveStateUndo(exposed, 201, 2, consensus));
    BOOST_CHECK(!node::recycle::RemoveStateUndo(exposed, 201, 0, consensus));
    BOOST_CHECK_EQUAL(exposed.vtxundo.size(), 1U);
    consensus.recycle_enabled = false;
    BOOST_REQUIRE(node::recycle::RemoveStateUndo(exposed, 201, 2, consensus));
    BOOST_CHECK_EQUAL(exposed.vtxundo.size(), 1U);

    // Stripping the API copy must leave on-disk recovery information intact.
    CAmount pool_before;
    std::vector<std::pair<COutPoint, Coin>> expired;
    BOOST_REQUIRE(node::recycle::ReadUndo(disk.vtxundo.back(), pool_before, expired));
    BOOST_CHECK_EQUAL(expired.size(), 1U);
    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, 201, &disk.vtxundo.back(), 200));
    BOOST_CHECK(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
}

BOOST_AUTO_TEST_CASE(expiry_reward_and_disconnect)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    constexpr CAmount BASE_REWARD{50 * COIN};
    constexpr CAmount EXPIRED_VALUE{COIN / 2};

    CBlock origin{CoinbaseBlock(EXPIRED_VALUE, 1)};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo early_undo;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, EXPIRED_VALUE, early_undo, error));
    BOOST_CHECK(!view.HaveCoin(COutPoint{Txid{}, 1}));
    BOOST_CHECK(early_undo.vprevout.empty());

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(BASE_REWARD + EXPIRED_VALUE, 2)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), EXPIRED_VALUE);
    AddCoins(view, *expiry.vtx[0], expiry_height);

    CTxUndo recycle_undo;
    std::vector<COutPoint> expired;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD,
                                               recycle_undo, error, &expired));
    BOOST_CHECK_EQUAL(expired.size(), 1U);
    BOOST_CHECK(!view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);

    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &recycle_undo));
    BOOST_CHECK(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);
}

BOOST_AUTO_TEST_CASE(unclaimed_value_stays_in_pool_and_reward_is_capped)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    constexpr CAmount BASE_REWARD{50 * COIN};

    CBlock origin{CoinbaseBlock(2 * COIN, 3)};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, 2 * COIN, unused, error));

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(BASE_REWARD, 4)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), COIN);
    AddCoins(view, *expiry.vtx[0], expiry_height);
    CTxUndo recycle_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD,
                                               recycle_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 2 * COIN);
}

BOOST_AUTO_TEST_CASE(reward_boundaries_and_excess_claim)
{
    constexpr CAmount BASE_REWARD{50 * COIN};
    constexpr CAmount REMAINDER{COIN / 3};

    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(REMAINDER, 10)};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, REMAINDER, unused, error));

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock exact{CoinbaseBlock(BASE_REWARD + REMAINDER, 11)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, exact, expiry_height), REMAINDER);

    CBlock excessive{CoinbaseBlock(BASE_REWARD + REMAINDER + 1, 12)};
    AddCoins(view, *excessive.vtx[0], expiry_height);
    CTxUndo undo;
    BOOST_CHECK(!node::recycle::ConnectBlock(view, excessive, expiry_height, BASE_REWARD, undo, error));
    BOOST_CHECK_EQUAL(error, "coinbase claims excessive recycle reward");
}

BOOST_AUTO_TEST_CASE(pool_accounting_stress)
{
    constexpr CAmount BASE_REWARD{50 * COIN};
    CCoinsView base;
    CCoinsViewCache view{&base};
    CAmount expected_pool{0};

    for (int iteration = 0; iteration < 1'000; ++iteration) {
        const int origin_height{iteration + 1};
        const CAmount expiring{(1 + iteration % 4) * (COIN / 2)};
        CBlock origin{CoinbaseBlock(expiring, 1'000 + iteration)};
        AddCoins(view, *origin.vtx[0], origin_height);
        CTxUndo unused;
        std::string error;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, origin_height, expiring, unused, error));

        const int expiry_height{origin_height + node::recycle::EXPIRY_BLOCKS};
        const CAmount available{expected_pool + expiring};
        const CAmount claimed{std::min(node::recycle::MAX_REWARD, available)};
        CBlock expiry{CoinbaseBlock(BASE_REWARD + claimed, 10'000 + iteration)};
        BOOST_REQUIRE_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), claimed);
        AddCoins(view, *expiry.vtx[0], expiry_height);
        CTxUndo undo;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD, undo, error));
        expected_pool = available - claimed;
        BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), expected_pool);
        BOOST_REQUIRE(expected_pool >= 0);
    }
}

BOOST_AUTO_TEST_CASE(partial_reward_fee_separation_and_reorg)
{
    constexpr CAmount SUBSIDY{50 * COIN};
    constexpr CAmount FEES{12'345};
    constexpr CAmount EXPIRED{2 * COIN};
    constexpr CAmount PARTIAL_REWARD{COIN / 3};

    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(EXPIRED, 20'000)};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, EXPIRED, unused, error));

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(SUBSIDY + FEES + PARTIAL_REWARD, 20'001)};
    AddCoins(view, *expiry.vtx[0], expiry_height);
    CTxUndo expiry_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, SUBSIDY + FEES,
                                               expiry_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED - PARTIAL_REWARD);

    // Crash recovery may replay a transition whose UTXO and recycle changes
    // were already written. Reapplying it must be idempotent.
    BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, expiry_height,
                                                   SUBSIDY + FEES, &expiry_undo));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED - PARTIAL_REWARD);

    // Reorg the partially rewarded expiry block out and replace it with a block
    // that claims no recycle reward. The expiry must be applied exactly once.
    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &expiry_undo));
    BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), 0);
    BOOST_REQUIRE(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    CBlock alternate{CoinbaseBlock(SUBSIDY + FEES, 20'002)};
    AddCoins(view, *alternate.vtx[0], expiry_height);
    CTxUndo alternate_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, alternate, expiry_height, SUBSIDY + FEES,
                                               alternate_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED);
}

BOOST_AUTO_TEST_CASE(spent_in_expiry_block_is_not_recycled)
{
    constexpr CAmount VALUE{3 * COIN / 4};
    constexpr CAmount BASE_REWARD{50 * COIN};

    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(VALUE, 30'000)};
    const COutPoint origin_out{origin.vtx[0]->GetHash(), 0};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, VALUE, unused, error));

    CMutableTransaction spend;
    spend.vin.emplace_back(origin_out);
    spend.vout.emplace_back(VALUE, CScript{} << OP_TRUE);
    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(BASE_REWARD, 30'001)};
    expiry.vtx.push_back(MakeTransactionRef(std::move(spend)));

    // Template accounting must exclude an output spent by the candidate block.
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), 0);

    // ConnectBlock calls recycle accounting after normal transaction updates.
    Coin spent;
    BOOST_REQUIRE(view.SpendCoin(origin_out, &spent));
    AddCoins(view, *expiry.vtx[0], expiry_height);
    AddCoins(view, *expiry.vtx[1], expiry_height);
    CTxUndo recycle_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD,
                                               recycle_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);
}


BOOST_FIXTURE_TEST_CASE(large_state_disk_and_undo_roundtrip, BasicTestingSetup)
{
    for (int count : {200, 277, 278, 1000}) {
        const auto path{m_path_root / fs::PathFromString(strprintf("coins-%d", count))};
        CBlock origin{CoinbaseBlock(count, count)};
        CMutableTransaction tx{*origin.vtx[0]};
        tx.vout.assign(count, CTxOut{1, CScript{} << OP_TRUE});
        origin.vtx[0] = MakeTransactionRef(std::move(tx));
        const COutPoint schedule_key{Txid{}, 1'000'001};
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
            BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
            CCoinsViewCache view{&db};
            AddCoins(view, *origin.vtx[0], 1);
            CTxUndo unused;
            std::string error;
            BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, 1, count, unused, error));
            view.SetBestBlock(uint256{1});
            view.Flush();
        }
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
            BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
            CCoinsViewCache view{&db};
            BOOST_CHECK(!view.HaveCoin(schedule_key));
            BOOST_CHECK_EQUAL(view.GetCoinsByHeight(1).size(), count);
            const int height{1 + node::recycle::EXPIRY_BLOCKS};
            CBlock expiry{CoinbaseBlock(50 * COIN + count, count + 1)};
            BOOST_REQUIRE_EQUAL(node::recycle::AvailableReward(view, expiry, height), count);
            AddCoins(view, *expiry.vtx[0], height);
            CBlockUndo block_undo;
            block_undo.vtxundo.emplace_back();
            std::string error;
            BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, height, 50 * COIN, block_undo.vtxundo.back(), error));
            DataStream bytes;
            bytes << block_undo;
            CBlockUndo restored;
            bytes >> restored;
            if (count == 1000) BOOST_REQUIRE(restored.vtxundo.back().vprevout[0].out.scriptPubKey.size() > MAX_SCRIPT_SIZE);
            BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, height, 50 * COIN, &restored.vtxundo.back()));
            BOOST_REQUIRE(node::recycle::DisconnectBlock(view, height, &restored.vtxundo.back()));
            BOOST_CHECK(!view.HaveCoin(schedule_key));
            BOOST_CHECK_EQUAL(view.GetCoinsByHeight(1).size(), count);
            BOOST_CHECK(!view.HaveCoin(COutPoint{Txid{}, std::numeric_limits<uint32_t>::max()}));
            for (int n{0}; n < count; ++n) BOOST_CHECK(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), static_cast<uint32_t>(n)}));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(compaction_preserves_recycle_state_and_recovery, BasicTestingSetup)
{
    const auto path{m_path_root / "recycle-compaction"};
    constexpr int COUNT{300};
    constexpr CAmount VALUE{3 * COIN};
    CBlock origin{CoinbaseBlock(VALUE, 40'000)};
    CMutableTransaction tx{*origin.vtx[0]};
    tx.vout.assign(COUNT, CTxOut{VALUE / COUNT, CScript{} << OP_TRUE});
    origin.vtx[0] = MakeTransactionRef(std::move(tx));
    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    const CBlock expiry{CoinbaseBlock(50 * COIN, 40'001)};
    CTxUndo expiry_undo;

    using Records = std::vector<std::pair<COutPoint, Coin>>;
    const auto same_records = [](const Records& a, const Records& b) {
        return std::ranges::equal(a, b, [](const auto& left, const auto& right) {
            return left.first == right.first && left.second.out == right.second.out &&
                   left.second.nHeight == right.second.nHeight && left.second.fCoinBase == right.second.fCoinBase;
        });
    };
    const auto snapshot = [](CCoinsViewDB& db) {
        Records records;
        for (auto cursor{db.Cursor()}; cursor->Valid(); cursor->Next()) {
            COutPoint key;
            Coin coin;
            BOOST_REQUIRE(cursor->GetKey(key));
            BOOST_REQUIRE(cursor->GetValue(coin));
            records.emplace_back(key, std::move(coin));
        }
        return records;
    };
    const auto compact_and_resize = [&](CCoinsViewDB& db) {
        const auto before{snapshot(db)};
        const auto tip{db.GetBestBlock()};
        for (size_t cache_size : {1U << 20, 2U << 20, 1U << 20}) {
            auto pending{WITH_LOCK(::cs_main, return db.CompactFull())};
            // Resize must safely serialize with any still-running compaction.
            WITH_LOCK(::cs_main, db.ResizeCache(cache_size));
            pending.get();
            BOOST_CHECK(same_records(snapshot(db), before));
            BOOST_CHECK(db.GetBestBlock() == tip);
            BOOST_CHECK(db.RecycleStateReady());
            BOOST_CHECK(!db.NeedsUpgrade());
        }
    };

    Records original_records;
    Records expired_records;
    std::shared_future<void> pending_shutdown;
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
        BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
        CCoinsViewCache view{&db};
        AddCoins(view, *origin.vtx[0], 1);
        CTxUndo unused;
        std::string error;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, 1, VALUE, unused, error));
        BOOST_CHECK_EQUAL(view.GetCoinsByHeight(1).size(), COUNT);
        view.SetBestBlock(uint256{1});
        view.Flush();
        original_records = snapshot(db);
        compact_and_resize(db);

        AddCoins(view, *expiry.vtx[0], expiry_height);
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, 50 * COIN, expiry_undo, error));
        BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), VALUE);
        view.SetBestBlock(uint256{2});
        view.Flush();
        expired_records = snapshot(db);
        compact_and_resize(db);
        // Destruction must wait before releasing the database, even if the
        // caller has not explicitly waited for the background operation.
        pending_shutdown = WITH_LOCK(::cs_main, return db.CompactFull());
    }
    BOOST_REQUIRE(pending_shutdown.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
    pending_shutdown.get();
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
        BOOST_CHECK(same_records(snapshot(db), expired_records));
        BOOST_CHECK(db.GetBestBlock() == uint256{2});
        BOOST_REQUIRE(db.RecycleStateReady());
        BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
        CCoinsViewCache view{&db};
        BOOST_REQUIRE(view.GetBestBlock() == uint256{2});
        BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), VALUE);

        // Recovery replay after compaction must not recycle the value twice.
        BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, expiry_height, 50 * COIN, &expiry_undo));
        view.Flush();
        BOOST_CHECK(same_records(snapshot(db), expired_records));

        BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &expiry_undo));
        BOOST_REQUIRE(view.SpendCoin(COutPoint{expiry.vtx[0]->GetHash(), 0}));
        BOOST_CHECK_EQUAL(view.GetCoinsByHeight(1).size(), COUNT);
        BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), 0);
        view.SetBestBlock(uint256{1});
        view.Flush();
        BOOST_CHECK(same_records(snapshot(db), original_records));
        compact_and_resize(db);
    }
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
        BOOST_CHECK(same_records(snapshot(db), original_records));
        BOOST_CHECK(db.GetBestBlock() == uint256{1});
        BOOST_CHECK(db.RecycleStateReady());
    }
}

BOOST_AUTO_TEST_CASE(metadata_reader_bounds_and_truncation)
{
    CScript decoded;
    DataStream oversized;
    const uint32_t size{static_cast<uint32_t>(MAX_SIZE) + 1 + ScriptCompression::nSpecialScripts};
    oversized << VARINT(size);
    BOOST_CHECK_THROW((oversized >> Using<node::recycle::MetadataScriptCompression>(decoded)), std::ios_base::failure);

    // The long-script exception applies only to internal OP_RETURN records.
    CScript non_metadata;
    non_metadata.resize(MAX_SCRIPT_SIZE + 1);
    non_metadata.front() = OP_TRUE;
    DataStream non_metadata_stream;
    non_metadata_stream << Using<ScriptCompression>(non_metadata);
    BOOST_CHECK_THROW((non_metadata_stream >> Using<node::recycle::MetadataScriptCompression>(decoded)), std::ios_base::failure);

    DataStream truncated;
    const uint32_t missing_payload{100 + ScriptCompression::nSpecialScripts};
    truncated << VARINT(missing_payload);
    BOOST_CHECK_THROW((truncated >> Using<node::recycle::MetadataScriptCompression>(decoded)), std::ios_base::failure);
}

BOOST_FIXTURE_TEST_CASE(index_migration_and_interruption, BasicTestingSetup)
{
    const auto path{m_path_root / "legacy-index"};
    CBlock block{CoinbaseBlock(COIN, 101)};
    CMutableTransaction transaction{*block.vtx[0]};
    transaction.vout.push_back(transaction.vout[0]);
    block.vtx[0] = MakeTransactionRef(std::move(transaction));
    const COutPoint output{block.vtx[0]->GetHash(), 0};
    const COutPoint schedule{Txid{}, 1'000'001};
    const COutPoint pool{Txid{}, std::numeric_limits<uint32_t>::max()};
    {
        // Legacy DB: UTXO, truncated schedule, and an existing pool balance.
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {}};
        CCoinsViewCache view{&db};
        AddCoins(view, *block.vtx[0], 1);
        view.AddCoin(schedule, Coin{CTxOut{0, CScript{OP_RETURN}}, 1, false}, false, true);
        view.AddCoin(pool, Coin{CTxOut{7 * COIN, CScript{OP_RETURN}}, 201, false}, false, true);
        view.SetBestBlock(uint256{42});
        view.Flush();
    }
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true, .batch_write_bytes = 1}};
        BOOST_CHECK_THROW(db.GetCoinsByHeight(1), std::runtime_error);
        int calls{0};
        BOOST_CHECK(!db.UpgradeRecycleIndex([&] { return ++calls == 4; }));
        BOOST_CHECK(!db.RecycleStateReady());
        BOOST_CHECK(!db.HaveCoin(schedule));
    }
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true, .batch_write_bytes = 1}};
        BOOST_CHECK(!db.NeedsUpgrade());
        BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
        BOOST_CHECK(db.RecycleStateReady());
        BOOST_CHECK(!db.HaveCoin(schedule));
        BOOST_CHECK(db.GetBestBlock() == uint256{42});
        const std::set<COutPoint> expected{output, COutPoint{block.vtx[0]->GetHash(), 1}};
        BOOST_CHECK(db.GetCoinsByHeight(1) == expected);
        CCoinsViewCache view{&db};
        BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 7 * COIN);
        BOOST_REQUIRE(view.GetBestBlock() == uint256{42});
        for (const auto& outpoint : expected) BOOST_REQUIRE(view.SpendCoin(outpoint));
        view.Flush();
        BOOST_CHECK(db.GetCoinsByHeight(1).empty());
        BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
        BOOST_CHECK(db.GetCoinsByHeight(1).empty());
    }
}

BOOST_FIXTURE_TEST_CASE(index_corruption_and_rebuild, BasicTestingSetup)
{
    for (int damage : {0, 1, 2}) {
        const auto path{m_path_root / fs::PathFromString(strprintf("damaged-index-%d", damage))};
        const CBlock origin{CoinbaseBlock(COIN, 1)};
        const COutPoint output{origin.vtx[0]->GetHash(), 0};
        const COutPoint phantom{Txid::FromUint256(uint256{42}), 0};
        const std::set<COutPoint> expected{output};
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
            BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
            CCoinsViewCache view{&db};
            AddCoins(view, *origin.vtx[0], 1);
            view.SetBestBlock(uint256{1});
            view.Flush();
        }
        {
            CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
            const TestHeightEntry correct{'E', 1, output};
            BOOST_REQUIRE_EQUAL(GetSerializeSize(correct) + GetSerializeSize(uint8_t{0}), 42U);
            BOOST_REQUIRE(raw.Exists(correct));
            if (damage == 1) {
                raw.Write(TestHeightEntry{'E', 1, phantom}, uint8_t{0}, true);
            } else {
                raw.Erase(correct, true);
                if (damage == 2) raw.Write(TestHeightEntry{'E', 2, output}, uint8_t{0}, true);
            }
        }
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
            // The ready marker certifies migration completion, not completeness.
            BOOST_REQUIRE(db.RecycleStateReady());
            BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
            BOOST_CHECK(db.GetCoinsByHeight(1) != expected);
            const auto coin{db.GetCoin(output)};
            BOOST_REQUIRE(coin);
            BOOST_CHECK_EQUAL(coin->nHeight, 1U);
            BOOST_CHECK_EQUAL(coin->out.nValue, COIN);
            auto snapshot{CheckSnapshot(db)};
            BOOST_REQUIRE(snapshot);
            const auto checked{snapshot->Check(ContinueCheck)};
            BOOST_CHECK(checked.completed);
            BOOST_CHECK(!checked.Valid());
            BOOST_CHECK_EQUAL(checked.missing, damage == 1 ? 0U : 1U);
            BOOST_CHECK_EQUAL(checked.orphaned, damage == 1 ? 1U : 0U);
            BOOST_CHECK_EQUAL(checked.height_mismatches, damage == 2 ? 1U : 0U);
            snapshot.reset();
            CCoinsViewCache view{&db};
            const CBlock expiry{CoinbaseBlock(0, 201)};
            BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, 201, 200), damage == 1 ? COIN : 0);
        }
        {
            // Test-only injection of an interrupted migration. This is not a
            // supported repair procedure for an already corrupted chainstate.
            CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
            raw.Write(uint8_t{'r'}, 3, true);
        }
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true, .batch_write_bytes = 1}};
            BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
            BOOST_CHECK(db.GetCoinsByHeight(1) == expected);
            BOOST_CHECK(db.GetCoinsByHeight(2).empty());
            BOOST_CHECK(db.GetBestBlock() == uint256{1});
            CCoinsViewCache view{&db};
            const CBlock expiry{CoinbaseBlock(0, 201)};
            BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, 201, 200), COIN);
            auto snapshot{CheckSnapshot(db)};
            BOOST_REQUIRE(snapshot);
            BOOST_CHECK(snapshot->Check(ContinueCheck).Valid());
        }
    }
}

BOOST_FIXTURE_TEST_CASE(index_check_snapshot_and_cancellation, BasicTestingSetup)
{
    CCoinsViewDB db{{.path = m_path_root / "check-snapshot", .cache_bytes = 1 << 20}, {.recycle_index = true}};
    BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
    CCoinsViewCache view{&db};
    const auto block{CoinbaseBlock(COIN, 1)};
    AddCoins(view, *block.vtx[0], 1);
    view.SetBestBlock(uint256{1});
    view.Flush();
    auto snapshot{CheckSnapshot(db)};
    BOOST_REQUIRE(snapshot);
    BOOST_CHECK(!CheckSnapshot(db));
    // Advance the live DB while the immutable diagnostic snapshot is leased.
    BOOST_REQUIRE(view.SpendCoin(COutPoint{block.vtx[0]->GetHash(), 0}));
    view.SetBestBlock(uint256{2});
    view.Flush();
    auto checked{snapshot->Check(ContinueCheck)};
    BOOST_CHECK(checked.Valid());
    BOOST_CHECK_EQUAL(checked.indexed_coins, 1U);
    BOOST_CHECK(snapshot->GetBestBlock() == uint256{1});
    BOOST_CHECK(!snapshot->Check([](const auto&, const auto&) { return false; }).completed);
    snapshot.reset();
    snapshot = CheckSnapshot(db);
    BOOST_CHECK(snapshot->Check(ContinueCheck).Valid());
    BOOST_CHECK_EQUAL(snapshot->Check(ContinueCheck).indexed_coins, 0U);
    // Resize must cancel a leased scan, and reopen only after its reads finish.
    std::promise<void> resizing;
    auto resized{std::async(std::launch::async, [&] {
        LOCK(cs_main);
        resizing.set_value();
        db.ResizeCache(2 << 20);
    })};
    resizing.get_future().wait();
    bool cancelled{false};
    for (int tries{0}; tries < 100 && !cancelled; ++tries) {
        cancelled = !snapshot->Check(ContinueCheck).completed;
        if (!cancelled) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    snapshot.reset(); // Always release before waiting, including on assertion failure.
    resized.get();
    BOOST_CHECK(cancelled);
    BOOST_CHECK(db.GetBestBlock() == uint256{2});
    snapshot = CheckSnapshot(db);
    BOOST_CHECK(snapshot->Check(ContinueCheck).Valid());
}

BOOST_FIXTURE_TEST_CASE(index_check_malformed_records_and_bounded_examples, BasicTestingSetup)
{
    const auto path{m_path_root / "malformed-check"};
    const auto block{CoinbaseBlock(COIN, 1)};
    const COutPoint output{block.vtx[0]->GetHash(), 0};
    const COutPoint pool{Txid{}, std::numeric_limits<uint32_t>::max()};
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
        BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
        CCoinsViewCache view{&db};
        AddCoins(view, *block.vtx[0], 1);
        view.AddCoin(pool, Coin{CTxOut{7 * COIN, CScript{OP_RETURN}}, 201, false}, false, true);
        view.SetBestBlock(uint256{1});
        view.Flush();
        auto snapshot{CheckSnapshot(db)};
        auto checked{snapshot->Check(ContinueCheck)};
        BOOST_CHECK(checked.Valid());
        BOOST_CHECK_EQUAL(checked.coins_scanned, 2U);
        BOOST_CHECK_EQUAL(checked.indexed_coins, 1U);
    }
    {
        CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
        raw.Write(TestHeightEntry{'E', 1, output}, uint8_t{1}); // Invalid placeholder.
        raw.Write(std::pair{TestHeightEntry{'E', 1, output}, uint8_t{0}}, uint8_t{0}); // Trailing key bytes.
        raw.Write(TestHeightEntry{'E', 201, pool}, uint8_t{0}); // Internal state must not be indexed.
        raw.Write(TestCoinEntry{'C', COutPoint{Txid::FromUint256(uint256{99}), 0}}, uint8_t{0});
        for (uint32_t i{100}; i < 130; ++i) {
            raw.Write(TestHeightEntry{'E', 1, COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i)}), 0}}, uint8_t{0});
        }
    }
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_index = true}};
        auto snapshot{CheckSnapshot(db)};
        const auto checked{snapshot->Check(ContinueCheck)};
        BOOST_CHECK(checked.completed);
        BOOST_CHECK(!checked.Valid());
        BOOST_CHECK_EQUAL(checked.missing, 0U);
        BOOST_CHECK_EQUAL(checked.orphaned, 30U);
        BOOST_CHECK_EQUAL(checked.unexpected, 1U);
        BOOST_CHECK_EQUAL(checked.malformed_coins, 1U);
        BOOST_CHECK_EQUAL(checked.malformed_index_entries, 2U);
        BOOST_CHECK_EQUAL(checked.issues.size(), 20U);
    }
}

BOOST_FIXTURE_TEST_CASE(index_cache_overlays_and_overwrites, BasicTestingSetup)
{
    CCoinsViewDB db{{.path = m_path_root / "index-cache", .cache_bytes = 1 << 20}, {.recycle_index = true}};
    BOOST_REQUIRE(db.UpgradeRecycleIndex([] { return false; }));
    CCoinsViewCache parent{&db};
    const CBlock block{CoinbaseBlock(COIN, 102)};
    const COutPoint output{block.vtx[0]->GetHash(), 0};
    AddCoins(parent, *block.vtx[0], 1);
    parent.SetBestBlock(uint256{1});
    parent.Flush();
    BOOST_CHECK_EQUAL(parent.GetCoinsByHeight(1).size(), 1U);
    CoinsViewOverlay child{&parent};
    BOOST_CHECK_EQUAL(child.GetCoinsByHeight(1).size(), 1U);
    {
        auto reset{child.CreateResetGuard()};
        BOOST_REQUIRE(child.SpendCoin(output));
        BOOST_CHECK(child.GetCoinsByHeight(1).empty());
        BOOST_CHECK_EQUAL(parent.GetCoinsByHeight(1).size(), 1U);
        child.AddCoin(output, Coin{block.vtx[0]->vout[0], 2, true}, false);
        BOOST_CHECK_EQUAL(child.GetCoinsByHeight(2).size(), 1U);
    }
    BOOST_CHECK_EQUAL(child.GetCoinsByHeight(1).size(), 1U);
    BOOST_CHECK(child.GetCoinsByHeight(2).empty());
    // Same outpoint overwritten at a different height, including cached parents.
    child.AddCoin(output, Coin{block.vtx[0]->vout[0], 2, true}, true);
    child.SetBestBlock(uint256{2});
    child.Sync();
    BOOST_CHECK(parent.GetCoinsByHeight(1).empty());
    BOOST_CHECK_EQUAL(parent.GetCoinsByHeight(2).size(), 1U);
    parent.Sync();
    BOOST_CHECK(db.GetCoinsByHeight(1).empty());
    BOOST_CHECK_EQUAL(db.GetCoinsByHeight(2).size(), 1U);
    parent.Uncache(output);
    BOOST_CHECK_EQUAL(parent.GetCoinsByHeight(2).size(), 1U);
    BOOST_REQUIRE(parent.SpendCoin(output));
    parent.Sync();
    BOOST_CHECK(db.GetCoinsByHeight(2).empty());
    // Repeated create/spend does not retain historical reservations.
    for (int h{3}; h < 1003; ++h) {
        parent.AddCoin(output, Coin{block.vtx[0]->vout[0], h, true}, false);
        BOOST_CHECK_EQUAL(parent.GetCoinsByHeight(h).size(), 1U);
        parent.Sync();
        BOOST_REQUIRE(parent.SpendCoin(output));
        parent.Sync();
        BOOST_CHECK(db.GetCoinsByHeight(h).empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()
