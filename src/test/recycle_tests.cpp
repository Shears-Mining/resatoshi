// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <node/recycle.h>
#include <node/chainstatemanager_args.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <primitives/block.h>
#include <rpc/server.h>
#include <script/script.h>
#include <undo.h>
#include <univalue.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <fstream>
#include <type_traits>
#include <string>
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

Consensus::Params RecycleParams(int lifetime = Consensus::Params{}.recycle_expiry_blocks)
{
    Consensus::Params params;
    params.recycle_enabled = true;
    params.recycle_expiry_blocks = lifetime;
    return params;
}

bool ContinueCheck(const std::string&, const RecycleIndexCheckResult&) { return true; }

} // namespace

// Neither an omitted source nor a nullable block pointer is a valid call.
static_assert(!std::is_default_constructible_v<node::recycle::ExpirySource>);
static_assert(!std::is_constructible_v<node::recycle::ExpirySource, const CBlock*>);

BOOST_AUTO_TEST_SUITE(recycle_tests)

BOOST_FIXTURE_TEST_CASE(marker_failure_preserves_committed_value, BasicTestingSetup)
{
    const auto marker{m_path_root / "recycle-format"};
    const auto temporary{m_path_root / "recycle-format.tmp"};
    BOOST_REQUIRE(node::WriteRecycleMarker(marker, "1\n"));
    // A blocked temporary path models a failed write before publication.
    fs::create_directory(temporary);
    BOOST_CHECK(!node::WriteRecycleMarker(marker, "incomplete\n"));
    std::ifstream stream{marker.std_path()};
    std::string contents;
    std::getline(stream, contents);
    BOOST_CHECK_EQUAL(contents, "1");
    stream.close();
    fs::remove(temporary);
    // An abandoned partial temporary file is replaced on retry.
    { std::ofstream partial{temporary.std_path()}; partial << "partial"; }
    BOOST_REQUIRE(node::WriteRecycleMarker(marker, "1\n"));
    BOOST_CHECK(!fs::exists(temporary));
}

BOOST_AUTO_TEST_CASE(automatic_check_distinguishes_busy_and_shutdown)
{
    TestingSetup setup{ChainType::REGTEST, {.extra_args = {"-test=recycle"}, .coins_db_in_memory = false}};
    auto& chainman{*setup.m_node.chainman};
    using Result = ChainstateManager::RecycleCheckResult;
    BOOST_REQUIRE(chainman.CheckRecycleIndexAutomatically() == Result::VALID);
    std::unique_ptr<RecycleIndexSnapshot> manual;
    {
        LOCK(cs_main);
        manual = chainman.ActiveChainstate().CoinsDB().GetRecycleIndexSnapshot();
    }
    BOOST_REQUIRE(manual);
    BOOST_CHECK(chainman.CheckRecycleIndexAutomatically() == Result::SKIPPED);
    BOOST_CHECK(!chainman.m_recycle_failed);
    manual.reset();
    BOOST_REQUIRE(setup.m_interrupt());
    BOOST_CHECK(chainman.CheckRecycleIndexAutomatically() == Result::INTERRUPTED);
    BOOST_CHECK(!chainman.m_recycle_failed);
    BOOST_REQUIRE(setup.m_interrupt.reset());
    BOOST_CHECK(chainman.CheckRecycleIndexAutomatically() == Result::VALID);
}

BOOST_AUTO_TEST_CASE(typed_undo_boundaries_and_transaction_separation)
{
    const auto consensus{RecycleParams(200)};
    CBlockUndo undo;
    BOOST_CHECK(node::recycle::HasExpectedUndo(undo, 200, 1, consensus));
    BOOST_CHECK(!node::recycle::HasExpectedUndo(undo, 201, 1, consensus));
    undo.recycle.active = true;
    BOOST_CHECK(node::recycle::HasExpectedUndo(undo, 201, 1, consensus));
    BOOST_CHECK(!node::recycle::HasExpectedUndo(undo, 200, 1, consensus));
    CCoinsView base;
    CCoinsViewCache view{&base};
    const auto origin{CoinbaseBlock(2 * COIN, 1)};
    AddCoins(view, *origin.vtx[0], 1);
    const auto expiry{CoinbaseBlock(COIN, 201)};
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, 201, 0, undo.recycle, consensus, node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
    BOOST_CHECK(undo.vtxundo.empty());
    undo.vtxundo.emplace_back().vprevout.emplace_back(origin.vtx[0]->vout[0], 1, true);
    BOOST_CHECK(node::recycle::HasExpectedUndo(undo, 201, 2, consensus));
    DataStream stream;
    stream << undo;
    CBlockUndo disk;
    stream >> disk;
    BOOST_CHECK(stream.empty());
    BOOST_CHECK_EQUAL(disk.vtxundo.size(), 1U);
    BOOST_CHECK_EQUAL(disk.vtxundo[0].vprevout.size(), 1U);
    BOOST_CHECK_EQUAL(disk.recycle.expired.size(), 1U);
    BOOST_CHECK(node::recycle::HasExpectedUndo(disk, 201, 2, consensus));
    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, 201, &disk.recycle, consensus) == node::recycle::UndoResult::CLEAN);
    BOOST_CHECK(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    disk.recycle.expired.push_back(disk.recycle.expired[0]);
    BOOST_CHECK(!node::recycle::HasExpectedUndo(disk, 201, 2, consensus));
}

BOOST_FIXTURE_TEST_CASE(expiry_undo_replay_preserves_persisted_coin_deletion, BasicTestingSetup)
{
    // A partial reorg flush can leave an origin output already restored on disk.
    // Rolling back the old branch and then expiring it on the new branch must
    // still erase that persisted coin, rather than treat it as cache-only FRESH.
    for (const auto mode : {RecycleMode::INDEX, RecycleMode::BLOCK}) for (bool cached : {false, true}) {
        const auto path{m_path_root / fs::PathFromString(strprintf("reorg-undo-%d-%d", static_cast<int>(mode), cached))};
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = mode}};
        const CBlock origin{CoinbaseBlock(2 * COIN, 1)};
        const COutPoint outpoint{origin.vtx[0]->GetHash(), 0};
        {
            CCoinsViewCache persisted{&db};
            AddCoins(persisted, *origin.vtx[0], 1);
            persisted.SetBestBlock(uint256{1});
            persisted.Flush();
        }
        CRecycleUndo undo;
        undo.active = true;
        undo.expired.emplace_back(outpoint, Coin{origin.vtx[0]->vout[0], 1, true});
        const auto consensus{RecycleParams(200)};
        const CBlock expiry{CoinbaseBlock(COIN, 201)};
        {
            CCoinsViewCache replay{&db};
            if (cached) BOOST_REQUIRE(replay.HaveCoin(outpoint));
            BOOST_REQUIRE(node::recycle::DisconnectBlock(replay, 201, &undo, consensus) == node::recycle::UndoResult::UNCLEAN);
            BOOST_REQUIRE(node::recycle::DisconnectBlock(replay, 201, &undo, consensus) == node::recycle::UndoResult::UNCLEAN);
            BOOST_REQUIRE(node::recycle::RollforwardBlock(replay, expiry, 201, 0, &undo, consensus));
            replay.SetBestBlock(uint256{2});
            replay.Flush();
        }
        BOOST_CHECK(!db.HaveCoin(outpoint));
        BOOST_CHECK_EQUAL(db.GetRecyclePool(), COIN);
        if (mode == RecycleMode::INDEX) BOOST_CHECK(db.GetCoinsByHeight(1).empty());
    }
}

BOOST_FIXTURE_TEST_CASE(pool_cache_rollback_flush_and_reopen, BasicTestingSetup)
{
    const auto path{m_path_root / "pool-cache"};
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX, .batch_write_bytes = 1}};
        CCoinsViewCache parent{&db};
        parent.SetBestBlock(uint256{1});
        parent.SetRecyclePool(3 * COIN);
        {
            CCoinsViewCache discarded{&parent};
            BOOST_CHECK_EQUAL(discarded.GetRecyclePool(), 3 * COIN);
            discarded.SetRecyclePool(7 * COIN);
        }
        BOOST_CHECK_EQUAL(parent.GetRecyclePool(), 3 * COIN);
        CCoinsViewCache child{&parent};
        child.SetBestBlock(uint256{2});
        child.SetRecyclePool(5 * COIN);
        child.Sync();
        BOOST_CHECK_EQUAL(parent.GetRecyclePool(), 5 * COIN);
        BOOST_CHECK_EQUAL(db.GetRecyclePool(), 0);
        parent.Flush();
        BOOST_CHECK_EQUAL(db.GetRecyclePool(), 5 * COIN);
        BOOST_CHECK(!db.Cursor()->Valid());
        BOOST_CHECK_EQUAL(db.Cursor()->GetRecyclePool(), 5 * COIN);
        child.SetRecyclePool(0);
        child.Flush();
        parent.Sync();
        BOOST_CHECK_EQUAL(db.GetRecyclePool(), 0);
        parent.SetRecyclePool(9 * COIN);
        parent.Sync();
    }
    CCoinsViewDB reopened{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
    BOOST_CHECK_EQUAL(reopened.GetRecyclePool(), 9 * COIN);
    BOOST_CHECK(reopened.GetBestBlock() == uint256{2});
    BOOST_CHECK(!reopened.Cursor()->Valid());
}

BOOST_FIXTURE_TEST_CASE(missing_or_corrupt_pool_fails_closed, BasicTestingSetup)
{
    for (int damage : {0, 1, 2, 3}) {
        const auto path{m_path_root / fs::PathFromString(strprintf("pool-damage-%d", damage))};
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
            BOOST_CHECK_EQUAL(db.GetRecyclePool(), 0);
        }
        {
            CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
            if (damage == 0) raw.Erase(uint8_t{'P'});
            if (damage == 1) raw.Write(uint8_t{'P'}, uint8_t{0});
            if (damage == 2) raw.Write(uint8_t{'P'}, CAmount{-1});
            if (damage == 3) raw.Write(uint8_t{'P'}, CAmount{MAX_MONEY + 1});
        }
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
        BOOST_CHECK_THROW(db.GetRecyclePool(), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(rewards_before_first_expiry)
{
    constexpr CAmount BASE_REWARD{50 * COIN};
    for (const int height : {1, RecycleParams().recycle_expiry_blocks}) {
        CCoinsView base;
        CCoinsViewCache view{&base};
        CRecycleUndo undo;
        const auto excessive{CoinbaseBlock(BASE_REWARD + 1, height)};
        BOOST_CHECK(node::recycle::ConnectBlock(view, excessive, height, BASE_REWARD, undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::PREMATURE_REWARD);
        BOOST_CHECK(!undo.active);
        const auto valid{CoinbaseBlock(BASE_REWARD, height)};
        BOOST_CHECK(node::recycle::ConnectBlock(view, valid, height, BASE_REWARD, undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
        BOOST_CHECK(!undo.active);
        BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);
    }
}

BOOST_AUTO_TEST_CASE(expiry_reward_and_disconnect)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    constexpr CAmount BASE_REWARD{50 * COIN};
    constexpr CAmount EXPIRED_VALUE{COIN / 2};

    CBlock origin{CoinbaseBlock(EXPIRED_VALUE, 1)};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CRecycleUndo early_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, EXPIRED_VALUE, early_undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
    BOOST_CHECK(!view.HaveCoin(COutPoint{Txid{}, 1}));
    BOOST_CHECK(!early_undo.active);

    const int expiry_height{1 + RecycleParams().recycle_expiry_blocks};
    CBlock expiry{CoinbaseBlock(BASE_REWARD + EXPIRED_VALUE, 2)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height, RecycleParams(), node::recycle::ExpirySource::Index()), EXPIRED_VALUE);
    AddCoins(view, *expiry.vtx[0], expiry_height);

    CRecycleUndo recycle_undo;
    std::vector<COutPoint> expired;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD, recycle_undo, RecycleParams(), node::recycle::ExpirySource::Index(), &expired) == node::recycle::Error::NONE);
    BOOST_CHECK_EQUAL(expired.size(), 1U);
    BOOST_CHECK(!view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);

    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &recycle_undo, RecycleParams()) == node::recycle::UndoResult::CLEAN);
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
    CRecycleUndo unused;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, 2 * COIN, unused, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);

    const int expiry_height{1 + RecycleParams().recycle_expiry_blocks};
    CBlock expiry{CoinbaseBlock(BASE_REWARD, 4)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height, RecycleParams(), node::recycle::ExpirySource::Index()), COIN);
    AddCoins(view, *expiry.vtx[0], expiry_height);
    CRecycleUndo recycle_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD, recycle_undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
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
    CRecycleUndo unused;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, REMAINDER, unused, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);

    const int expiry_height{1 + RecycleParams().recycle_expiry_blocks};
    CBlock exact{CoinbaseBlock(BASE_REWARD + REMAINDER, 11)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, exact, expiry_height, RecycleParams(), node::recycle::ExpirySource::Index()), REMAINDER);

    CBlock excessive{CoinbaseBlock(BASE_REWARD + REMAINDER + 1, 12)};
    AddCoins(view, *excessive.vtx[0], expiry_height);
    CRecycleUndo undo;
    BOOST_CHECK(node::recycle::ConnectBlock(view, excessive, expiry_height, BASE_REWARD, undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::EXCESSIVE_REWARD);
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
        CRecycleUndo unused;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, origin_height, expiring, unused, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);

        const int expiry_height{origin_height + RecycleParams().recycle_expiry_blocks};
        const CAmount available{expected_pool + expiring};
        const CAmount claimed{std::min(node::recycle::MAX_REWARD, available)};
        CBlock expiry{CoinbaseBlock(BASE_REWARD + claimed, 10'000 + iteration)};
        BOOST_REQUIRE_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height, RecycleParams(), node::recycle::ExpirySource::Index()), claimed);
        AddCoins(view, *expiry.vtx[0], expiry_height);
        CRecycleUndo undo;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD, undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
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
    CRecycleUndo unused;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, EXPIRED, unused, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);

    const int expiry_height{1 + RecycleParams().recycle_expiry_blocks};
    CBlock expiry{CoinbaseBlock(SUBSIDY + FEES + PARTIAL_REWARD, 20'001)};
    AddCoins(view, *expiry.vtx[0], expiry_height);
    CRecycleUndo expiry_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, SUBSIDY + FEES, expiry_undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED - PARTIAL_REWARD);

    // Crash recovery may replay a transition whose UTXO and recycle changes
    // were already written. Reapplying it must be idempotent.
    BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, expiry_height, SUBSIDY + FEES, &expiry_undo, RecycleParams()));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED - PARTIAL_REWARD);

    // Reorg the partially rewarded expiry block out and replace it with a block
    // that claims no recycle reward. The expiry must be applied exactly once.
    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &expiry_undo, RecycleParams()) == node::recycle::UndoResult::CLEAN);
    BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), 0);
    BOOST_REQUIRE(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    CBlock alternate{CoinbaseBlock(SUBSIDY + FEES, 20'002)};
    AddCoins(view, *alternate.vtx[0], expiry_height);
    CRecycleUndo alternate_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, alternate, expiry_height, SUBSIDY + FEES, alternate_undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
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
    CRecycleUndo unused;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, VALUE, unused, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);

    CMutableTransaction spend;
    spend.vin.emplace_back(origin_out);
    spend.vout.emplace_back(VALUE, CScript{} << OP_TRUE);
    const int expiry_height{1 + RecycleParams().recycle_expiry_blocks};
    CBlock expiry{CoinbaseBlock(BASE_REWARD, 30'001)};
    expiry.vtx.push_back(MakeTransactionRef(std::move(spend)));

    // Template accounting must exclude an output spent by the candidate block.
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height, RecycleParams(), node::recycle::ExpirySource::Index()), 0);

    // ConnectBlock calls recycle accounting after normal transaction updates.
    Coin spent;
    BOOST_REQUIRE(view.SpendCoin(origin_out, &spent));
    AddCoins(view, *expiry.vtx[0], expiry_height);
    AddCoins(view, *expiry.vtx[1], expiry_height);
    CRecycleUndo recycle_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD, recycle_undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
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
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
            CCoinsViewCache view{&db};
            AddCoins(view, *origin.vtx[0], 1);
            CRecycleUndo unused;
            BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, 1, count, unused, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
            view.SetBestBlock(uint256{1});
            view.Flush();
        }
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
            CCoinsViewCache view{&db};
            BOOST_CHECK(!view.HaveCoin(schedule_key));
            BOOST_CHECK_EQUAL(view.GetCoinsByHeight(1).size(), count);
            const int height{1 + RecycleParams().recycle_expiry_blocks};
            CBlock expiry{CoinbaseBlock(50 * COIN + count, count + 1)};
            BOOST_REQUIRE_EQUAL(node::recycle::AvailableReward(view, expiry, height, RecycleParams(), node::recycle::ExpirySource::Index()), count);
            AddCoins(view, *expiry.vtx[0], height);
            CBlockUndo block_undo;
            BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, height, 50 * COIN, block_undo.recycle, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
            DataStream bytes;
            bytes << block_undo;
            CBlockUndo restored;
            bytes >> restored;
            BOOST_REQUIRE_EQUAL(restored.recycle.expired.size(), count);
            BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, height, 50 * COIN, &restored.recycle, RecycleParams()));
            BOOST_REQUIRE(node::recycle::DisconnectBlock(view, height, &restored.recycle, RecycleParams()) == node::recycle::UndoResult::CLEAN);
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
    const int expiry_height{1 + RecycleParams().recycle_expiry_blocks};
    const CBlock expiry{CoinbaseBlock(50 * COIN, 40'001)};
    CRecycleUndo expiry_undo;

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
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
        BOOST_REQUIRE(db.RebuildRecycleIndex([] { return false; }));
        CCoinsViewCache view{&db};
        AddCoins(view, *origin.vtx[0], 1);
        CRecycleUndo unused;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, 1, VALUE, unused, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
        BOOST_CHECK_EQUAL(view.GetCoinsByHeight(1).size(), COUNT);
        view.SetBestBlock(uint256{1});
        view.Flush();
        original_records = snapshot(db);
        compact_and_resize(db);

        AddCoins(view, *expiry.vtx[0], expiry_height);
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, 50 * COIN, expiry_undo, RecycleParams(), node::recycle::ExpirySource::Index()) == node::recycle::Error::NONE);
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
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
        BOOST_CHECK(same_records(snapshot(db), expired_records));
        BOOST_CHECK(db.GetBestBlock() == uint256{2});
        BOOST_REQUIRE(db.RecycleStateReady());
        BOOST_REQUIRE(db.RebuildRecycleIndex([] { return false; }));
        CCoinsViewCache view{&db};
        BOOST_REQUIRE(view.GetBestBlock() == uint256{2});
        BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), VALUE);

        // Recovery replay after compaction must not recycle the value twice.
        BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, expiry_height, 50 * COIN, &expiry_undo, RecycleParams()));
        view.Flush();
        BOOST_CHECK(same_records(snapshot(db), expired_records));

        BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &expiry_undo, RecycleParams()) == node::recycle::UndoResult::CLEAN);
        BOOST_REQUIRE(view.SpendCoin(COutPoint{expiry.vtx[0]->GetHash(), 0}));
        BOOST_CHECK_EQUAL(view.GetCoinsByHeight(1).size(), COUNT);
        BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), 0);
        view.SetBestBlock(uint256{1});
        view.Flush();
        BOOST_CHECK(same_records(snapshot(db), original_records));
        compact_and_resize(db);
    }
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
        BOOST_CHECK(same_records(snapshot(db), original_records));
        BOOST_CHECK(db.GetBestBlock() == uint256{1});
        BOOST_CHECK(db.RecycleStateReady());
    }
}

BOOST_FIXTURE_TEST_CASE(old_chainstate_requires_fresh_sync, BasicTestingSetup)
{
    for (int codec : {1, 2, 3, 4}) {
        const auto path{m_path_root / fs::PathFromString(strprintf("old-%d", codec))};
        {
            CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
            raw.Write(uint8_t{'r'}, codec);
            raw.Write(uint8_t{'B'}, uint256{42});
        }
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
        BOOST_CHECK(db.NeedsUpgrade());
        BOOST_CHECK_THROW(db.RebuildRecycleIndex([] { return false; }), std::runtime_error);
    }
}

BOOST_FIXTURE_TEST_CASE(index_iterator_io_error_and_restart, BasicTestingSetup)
{
    // Exercise deletion, coin rebuilding and height lookup independently. Put
    // only the affected prefix in an SST, keeping metadata in the write log.
    // Replacing that SST with a directory after opening produces an I/O error
    // at iterator access without adding fault-injection hooks to production.
    for (int phase : {0, 1, 2}) {
        const auto path{m_path_root / fs::PathFromString(strprintf("iterator-io-%d", phase))};
        const CBlock origin{CoinbaseBlock(COIN, 1)};
        const COutPoint output{origin.vtx[0]->GetHash(), 0};
        const Coin coin{origin.vtx[0]->vout[0], 1, true};
        fs::path table;
        {
            CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
            if (phase == 1) {
                raw.Write(TestCoinEntry{'C', output}, coin, true);
            } else {
                raw.Write(TestHeightEntry{'E', 1, output}, uint8_t{0}, true);
            }
            raw.CompactFull();
            for (const auto& file : fs::directory_iterator(path)) {
                if (file.path().extension() == ".ldb" || file.path().extension() == ".sst") {
                    BOOST_REQUIRE(table.empty());
                    table = file.path();
                }
            }
            BOOST_REQUIRE(!table.empty());
            if (phase != 1) raw.Write(TestCoinEntry{'C', output}, coin, true);
            raw.Write(uint8_t{'B'}, uint256{42}, true);
            raw.Write(uint8_t{'r'}, 5, true);
            raw.Write(uint8_t{'m'}, static_cast<int>(RecycleMode::INDEX), true);
            raw.Write(uint8_t{'P'}, CAmount{0}, true);
            if (phase != 2) raw.Write(uint8_t{'e'}, true, true);
        }
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX, .batch_write_bytes = 1}};
            const auto hidden{fs::PathFromString(fs::PathToString(table) + ".unavailable")};
            fs::rename(table, hidden);
            fs::create_directory(table);
            const auto io_error = [](const dbwrapper_error& e) {
                return std::string{e.what()}.find("IO error") != std::string::npos;
            };
            if (phase == 2) {
                BOOST_CHECK_EXCEPTION(db.GetCoinsByHeight(1), dbwrapper_error, io_error);
            } else {
                BOOST_CHECK_EXCEPTION(db.RebuildRecycleIndex([] { return false; }), dbwrapper_error, io_error);
                BOOST_CHECK(!db.RecycleStateReady());
            }
            fs::remove(table);
            fs::rename(hidden, table);
        }
        // Restore the file, reopen and require a complete, correct rebuild.
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX, .batch_write_bytes = 1}};
            if (phase != 2) BOOST_CHECK(!db.RecycleStateReady());
            BOOST_REQUIRE(db.RebuildRecycleIndex([] { return false; }));
            BOOST_CHECK(db.RecycleStateReady());
            const std::set<COutPoint> expected{output};
            BOOST_CHECK(db.GetCoinsByHeight(1) == expected);
            BOOST_CHECK(db.GetBestBlock() == uint256{42});
            const auto restored{db.GetCoin(output)};
            BOOST_REQUIRE(restored);
            BOOST_CHECK(restored->out == coin.out);
            BOOST_CHECK_EQUAL(restored->nHeight, coin.nHeight);
            BOOST_CHECK_EQUAL(restored->fCoinBase, coin.fCoinBase);
            auto snapshot{CheckSnapshot(db)};
            BOOST_REQUIRE(snapshot);
            BOOST_CHECK(snapshot->Check(ContinueCheck).Valid());
        }
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
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
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
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
            // The ready marker certifies migration completion, not completeness.
            BOOST_REQUIRE(db.RecycleStateReady());
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
            BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, 201, RecycleParams(200), node::recycle::ExpirySource::Index()), damage == 1 ? COIN : 0);
        }
        {
            // Test-only injection of an interrupted migration. This is not a
            // supported repair procedure for an already corrupted chainstate.
            CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
            raw.Write(uint8_t{'e'}, true, true);
        }
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX, .batch_write_bytes = 1}};
            BOOST_REQUIRE(db.RebuildRecycleIndex([] { return false; }));
            BOOST_CHECK(db.GetCoinsByHeight(1) == expected);
            BOOST_CHECK(db.GetCoinsByHeight(2).empty());
            BOOST_CHECK(db.GetBestBlock() == uint256{1});
            CCoinsViewCache view{&db};
            const CBlock expiry{CoinbaseBlock(0, 201)};
            BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, 201, RecycleParams(200), node::recycle::ExpirySource::Index()), COIN);
            auto snapshot{CheckSnapshot(db)};
            BOOST_REQUIRE(snapshot);
            BOOST_CHECK(snapshot->Check(ContinueCheck).Valid());
        }
    }
}

BOOST_FIXTURE_TEST_CASE(index_check_snapshot_and_cancellation, BasicTestingSetup)
{
    CCoinsViewDB db{{.path = m_path_root / "check-snapshot", .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
    BOOST_REQUIRE(db.RebuildRecycleIndex([] { return false; }));
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
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
        BOOST_REQUIRE(db.RebuildRecycleIndex([] { return false; }));
        CCoinsViewCache view{&db};
        AddCoins(view, *block.vtx[0], 1);
        view.SetRecyclePool(7 * COIN);
        view.SetBestBlock(uint256{1});
        view.Flush();
        auto snapshot{CheckSnapshot(db)};
        auto checked{snapshot->Check(ContinueCheck)};
        BOOST_CHECK(checked.Valid());
        BOOST_CHECK_EQUAL(checked.coins_scanned, 1U);
        BOOST_CHECK_EQUAL(checked.indexed_coins, 1U);
    }
    {
        CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
        raw.Write(TestHeightEntry{'E', 1, output}, uint8_t{1}); // Invalid placeholder.
        raw.Write(std::pair{TestHeightEntry{'E', 1, output}, uint8_t{0}}, uint8_t{0}); // Trailing key bytes.
        const COutPoint unspendable{Txid::FromUint256(uint256{98}), 0};
        raw.Write(TestCoinEntry{'C', unspendable}, Coin{CTxOut{0, CScript{OP_RETURN}}, 201, false});
        raw.Write(TestHeightEntry{'E', 201, unspendable}, uint8_t{0}); // Unspendable outputs must not be indexed.
        raw.Write(TestCoinEntry{'C', COutPoint{Txid::FromUint256(uint256{99}), 0}}, uint8_t{0});
        for (uint32_t i{100}; i < 130; ++i) {
            raw.Write(TestHeightEntry{'E', 1, COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i)}), 0}}, uint8_t{0});
        }
    }
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
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
    CCoinsViewDB db{{.path = m_path_root / "index-cache", .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
    BOOST_REQUIRE(db.RebuildRecycleIndex([] { return false; }));
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

BOOST_FIXTURE_TEST_CASE(database_mode_codecs_and_indexing, BasicTestingSetup)
{
    struct Case { const char* name; RecycleMode mode; };
    const Case cases[]{{"disabled", RecycleMode::DISABLED},
                       {"index", RecycleMode::INDEX},
                       {"block", RecycleMode::BLOCK}};
    const auto origin{CoinbaseBlock(COIN, 1)};
    const COutPoint output{origin.vtx[0]->GetHash(), 0};
    for (const auto& test : cases) {
        const auto path{m_path_root / test.name};
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = test.mode}};
            CCoinsViewCache view{&db};
            AddCoins(view, *origin.vtx[0], 1);
            view.SetBestBlock(uint256{1});
            view.Flush();
            BOOST_CHECK(!db.NeedsUpgrade());
            if (test.mode == RecycleMode::INDEX) {
                BOOST_CHECK(db.GetCoinsByHeight(1).contains(output));
            } else {
                BOOST_CHECK_THROW(db.GetCoinsByHeight(1), std::runtime_error);
            }
        }
        CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
        int version{0}, mode{-1};
        BOOST_REQUIRE(raw.Read(uint8_t{'r'}, version));
        BOOST_REQUIRE(raw.Read(uint8_t{'m'}, mode));
        BOOST_CHECK_EQUAL(version, 5);
        BOOST_CHECK_EQUAL(mode, static_cast<int>(test.mode));
        BOOST_CHECK_EQUAL(raw.Exists(TestHeightEntry{'E', 1, output}), test.mode == RecycleMode::INDEX);
    }
}

BOOST_FIXTURE_TEST_CASE(block_source_without_height_index, BasicTestingSetup)
{
    const auto path{m_path_root / "block-source"};
    const CBlock origin{CoinbaseBlock(2 * COIN, 1)};
    const COutPoint output{origin.vtx[0]->GetHash(), 0};
    {
        CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::BLOCK}};
        BOOST_CHECK(!db.NeedsUpgrade());
        BOOST_CHECK(!db.RecycleStateReady());
        CCoinsViewCache view{&db};
        AddCoins(view, *origin.vtx[0], 1);
        view.SetBestBlock(uint256{1});
        view.Flush();
        CBlock expiry{CoinbaseBlock(COIN, 201)};
        BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, 201, RecycleParams(200), node::recycle::ExpirySource::Block(origin)), COIN);
        // A spend in the candidate block excludes the output from template rewards.
        CMutableTransaction spend;
        spend.vin.emplace_back(output);
        spend.vout.emplace_back(COIN, CScript{OP_TRUE});
        expiry.vtx.push_back(MakeTransactionRef(spend));
        BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, 201, RecycleParams(200), node::recycle::ExpirySource::Block(origin)), 0);
        expiry.vtx.pop_back();
        CRecycleUndo undo;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, 201, 0, undo, RecycleParams(200), node::recycle::ExpirySource::Block(origin)) == node::recycle::Error::NONE);
        BOOST_CHECK(view.AccessCoin(output).IsSpent());
        BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), COIN);
        BOOST_REQUIRE(node::recycle::DisconnectBlock(view, 201, &undo, RecycleParams(200)) == node::recycle::UndoResult::CLEAN);
        BOOST_CHECK(!view.AccessCoin(output).IsSpent());
        // A stale outpoint at a different creation height must not expire.
        view.SpendCoin(output);
        view.AddCoin(output, Coin{origin.vtx[0]->vout[0], 2, true}, false);
        BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, 201, RecycleParams(200), node::recycle::ExpirySource::Block(origin)), 0);
        view.Flush();
    }
    {
        CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
        int codec{0};
        BOOST_REQUIRE(raw.Read(uint8_t{'r'}, codec));
        BOOST_CHECK_EQUAL(codec, 5);
        std::unique_ptr<CDBIterator> cursor{raw.NewIterator()};
        cursor->Seek(uint8_t{'E'});
        uint8_t prefix{0};
        BOOST_CHECK(!cursor->Valid() || !cursor->GetKey(prefix) || prefix != 'E');
        cursor->CheckStatus();
    }
    // The persisted mode must prevent opening block state as an indexed database.
    CCoinsViewDB indexed{{.path = path, .cache_bytes = 1 << 20}, {.recycle_mode = RecycleMode::INDEX}};
    BOOST_CHECK(indexed.NeedsUpgrade());
}

BOOST_AUTO_TEST_CASE(automatic_and_manual_checks_stop_on_missing_entry)
{
    for (bool manual : {false, true}) {
        TestingSetup setup{ChainType::REGTEST, {.extra_args = {"-test=recycle"}, .coins_db_in_memory = false}};
        auto& chainman{*setup.m_node.chainman};
        auto& chainstate{chainman.ActiveChainstate()};
        const CBlock origin{CoinbaseBlock(COIN, 1)};
        const COutPoint output{origin.vtx[0]->GetHash(), 0};
        {
            LOCK(cs_main);
            AddCoins(chainstate.CoinsTip(), *origin.vtx[0], 1);
        }
        BOOST_REQUIRE(chainman.CheckRecycleIndexAutomatically() == ChainstateManager::RecycleCheckResult::VALID);
        BOOST_CHECK(!chainman.m_recycle_failed);
        const auto path{chainstate.StoragePath()};
        {
            LOCK(cs_main);
            chainstate.ResetCoinsViews();
        }
        {
            CDBWrapper raw{{.path = path, .cache_bytes = 1 << 20}};
            BOOST_REQUIRE(raw.Exists(TestHeightEntry{'E', 1, output}));
            raw.Erase(TestHeightEntry{'E', 1, output}, true);
        }
        {
            LOCK(cs_main);
            chainstate.InitCoinsDB(1 << 20, false, false);
            chainstate.InitCoinsCache(1 << 20);
        }
        if (manual) {
            JSONRPCRequest request;
            request.context = &setup.m_node;
            request.strMethod = "checkrecycleindex";
            request.params = UniValue{UniValue::VARR};
            request.params.push_back("start");
            if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
            // Cancel at the final callback, after the missing entry was counted.
            // Partial diagnostic results must not trigger the recovery latch.
            int interruption_points{0};
            setup.m_node.rpc_interruption_point = [&] {
                if (++interruption_points != 3) return;
                auto abort_request{request};
                abort_request.params = UniValue{UniValue::VARR};
                abort_request.params.push_back("abort");
                BOOST_CHECK(tableRPC.execute(abort_request).get_bool());
            };
            const auto cancelled{tableRPC.execute(request)};
            BOOST_CHECK(!cancelled["success"].get_bool());
            BOOST_CHECK(cancelled.find_value("valid").isNull());
            BOOST_CHECK_EQUAL(cancelled["missing"].getInt<int>(), 1);
            BOOST_CHECK(!chainman.m_recycle_failed);
            BOOST_CHECK(!fs::exists(setup.m_args.GetDataDirNet() / "recycle-corrupt"));
            // At the final progress callback, let cache resizing acquire
            // cs_main. Check() has already tested cancellation at this point,
            // so it will complete with the missing entry. The RPC must release
            // its snapshot before failure handling tries to acquire cs_main.
            std::promise<void> resizing;
            auto resizing_started{resizing.get_future()};
            std::future<void> resized;
            interruption_points = 0;
            setup.m_node.rpc_interruption_point = [&] {
                if (++interruption_points != 3) return;
                resized = std::async(std::launch::async, [&] {
                    LOCK(cs_main);
                    resizing.set_value();
                    chainstate.CoinsDB().ResizeCache(2 << 20);
                });
                resizing_started.wait();
            };
            const auto checked{tableRPC.execute(request)};
            BOOST_REQUIRE(resized.valid());
            resized.get();
            setup.m_node.rpc_interruption_point = [] {};
            BOOST_CHECK(checked["success"].get_bool());
            BOOST_CHECK(!checked["valid"].get_bool());
            BOOST_CHECK_EQUAL(checked["missing"].getInt<int>(), 1);
        } else {
            BOOST_CHECK(chainman.CheckRecycleIndexAutomatically() == ChainstateManager::RecycleCheckResult::CORRUPT);
        }
        BOOST_CHECK(chainman.m_recycle_failed);
        {
            LOCK(cs_main);
            BlockValidationState state;
            BOOST_CHECK(!chainstate.ConnectBlock(chainman.GetParams().GenesisBlock(), state, chainstate.m_chain.Tip(), chainstate.CoinsTip()));
            BOOST_CHECK(state.IsError());
        }
        BOOST_CHECK(fs::exists(setup.m_args.GetDataDirNet() / "recycle-corrupt"));
    }
}

BOOST_AUTO_TEST_SUITE_END()
