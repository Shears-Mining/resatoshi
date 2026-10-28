// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <coins.h>
#include <dbwrapper.h>
#include <logging/timer.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/log.h>
#include <util/threadnames.h>
#include <util/vector.h>

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>

static constexpr uint8_t DB_COIN{'C'};
static constexpr uint8_t DB_BEST_BLOCK{'B'};
static constexpr uint8_t DB_HEAD_BLOCKS{'H'};
// Keys used in previous version that might still be found in the DB:
static constexpr uint8_t DB_COINS{'c'};
// This format has an explicit pool key and typed block undo. Old chainstate
// and undo data must be replaced by a fresh synchronization, never migrated.
static constexpr uint8_t DB_RECYCLE_FORMAT{'r'};
static constexpr uint8_t DB_RECYCLE_MODE{'m'};
static constexpr uint8_t DB_RECYCLE_POOL{'P'};
static constexpr uint8_t DB_RECYCLE_HEIGHT{'E'};
static constexpr uint8_t DB_RECYCLE_BUILDING{'e'};
static constexpr int RECYCLE_FORMAT_VERSION{5};

// Threshold for warning when writing this many dirty cache entries to disk.
static constexpr size_t WARN_FLUSH_COINS_COUNT{10'000'000};

std::optional<RecycleMode> ReadRecycleMode(CDBWrapper& db)
{
    std::unique_ptr<CDBIterator> cursor{db.NewIterator()};
    cursor->Seek(std::make_pair(DB_COINS, uint256{}));
    uint8_t key{0};
    if (cursor->Valid() && cursor->GetKey(key) && key == DB_COINS) return std::nullopt;
    cursor->CheckStatus();
    int version{0}, mode{-1};
    if (!db.Read(DB_RECYCLE_FORMAT, version) || version != RECYCLE_FORMAT_VERSION ||
        !db.Read(DB_RECYCLE_MODE, mode) || mode < static_cast<int>(RecycleMode::DISABLED) ||
        mode > static_cast<int>(RecycleMode::BLOCK)) return std::nullopt;
    return static_cast<RecycleMode>(mode);
}

bool CCoinsViewDB::HasUnsupportedFormat()
{
    return ReadRecycleMode(*m_db) != m_options.recycle_mode;
}

bool CCoinsViewDB::RecycleStateReady() const
{
    int version{0}, mode{-1};
    return m_db->Read(DB_RECYCLE_FORMAT, version) && version == RECYCLE_FORMAT_VERSION &&
        m_db->Read(DB_RECYCLE_MODE, mode) && mode == static_cast<int>(RecycleMode::INDEX) &&
        !m_db->Exists(DB_RECYCLE_BUILDING);
}

namespace {

struct CoinEntry {
    COutPoint* outpoint;
    uint8_t key{DB_COIN};
    explicit CoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)) {}

    SERIALIZE_METHODS(CoinEntry, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};

// Exact-height lookups only: fixed-width height followed by the full outpoint.
struct HeightEntry {
    uint8_t key{DB_RECYCLE_HEIGHT};
    uint32_t height{0};
    COutPoint outpoint{Txid{}, 0};
    SERIALIZE_METHODS(HeightEntry, obj) { READWRITE(obj.key, obj.height, obj.outpoint); }
};

} // namespace

struct RecycleIndexScanControl {
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::condition_variable released_cv;
    bool released{false};
};

struct RecycleIndexSnapshot::Impl {
    std::shared_ptr<CDBWrapper> db;
    std::shared_ptr<RecycleIndexScanControl> control;
    std::unique_ptr<CDBSnapshot> snapshot;
    Impl(std::shared_ptr<CDBWrapper> parent, std::shared_ptr<RecycleIndexScanControl> state)
        : db{std::move(parent)}, control{std::move(state)}, snapshot{std::make_unique<CDBSnapshot>(*db)} {}
    ~Impl()
    {
        snapshot.reset();
        db.reset();
        {
            std::lock_guard lock{control->mutex};
            control->released = true;
        }
        control->released_cv.notify_all();
    }
};

bool RecycleIndexCheckResult::Valid() const
{
    return completed && missing == 0 && orphaned == 0 && height_mismatches == 0 &&
        unexpected == 0 && malformed_coins == 0 && malformed_index_entries == 0;
}

RecycleIndexSnapshot::RecycleIndexSnapshot(std::shared_ptr<CDBWrapper> db, std::shared_ptr<RecycleIndexScanControl> control)
    : m_impl{std::make_unique<Impl>(std::move(db), std::move(control))}
{
    int version{0}, mode{-1};
    if (!m_impl->snapshot->Read(DB_RECYCLE_FORMAT, version) || version != RECYCLE_FORMAT_VERSION ||
        !m_impl->snapshot->Read(DB_RECYCLE_MODE, mode) || mode != static_cast<int>(RecycleMode::INDEX) ||
        m_impl->snapshot->Exists(DB_RECYCLE_BUILDING) || m_impl->snapshot->Exists(DB_HEAD_BLOCKS) || GetBestBlock().IsNull()) {
        throw std::runtime_error("Recycle index check requires a ready, fully flushed chainstate");
    }
}

RecycleIndexSnapshot::~RecycleIndexSnapshot() = default;

uint256 RecycleIndexSnapshot::GetBestBlock() const
{
    uint256 block;
    if (!m_impl->snapshot->Read(DB_BEST_BLOCK, block)) throw std::runtime_error("Unreadable chainstate best block");
    return block;
}

RecycleIndexCheckResult RecycleIndexSnapshot::Check(
    const std::function<bool(const std::string&, const RecycleIndexCheckResult&)>& progress) const
{
    RecycleIndexCheckResult result;
    const auto& snapshot{*m_impl->snapshot};
    auto issue = [&](std::string type, std::optional<COutPoint> outpoint = {},
                     std::optional<uint32_t> index_height = {}, std::optional<uint32_t> coin_height = {}) {
        if (result.issues.size() < 20) result.issues.push_back({std::move(type), outpoint, index_height, coin_height});
    };
    auto proceed = [&](const std::string& phase, uint64_t count) {
        return !m_impl->control->cancel.load() && (count % 256 != 0 || progress(phase, result));
    };
    auto cursor{snapshot.NewIterator()};
    for (cursor->Seek(DB_COIN); cursor->Valid(); cursor->Next()) {
        uint8_t prefix{0};
        if (!cursor->GetKey(prefix) || prefix != DB_COIN) break;
        if (!proceed("coins", result.coins_scanned)) return result;
        ++result.coins_scanned;
        COutPoint outpoint;
        CoinEntry key{&outpoint};
        Coin coin;

        if (!cursor->GetKey(key, true) || !cursor->GetValue(coin, true) || coin.IsSpent()) {
            ++result.malformed_coins;
            issue("malformed_coin");
            continue;
        }
        if (coin.out.scriptPubKey.IsUnspendable()) continue;
        ++result.indexed_coins;
        if (!snapshot.Exists(HeightEntry{DB_RECYCLE_HEIGHT, coin.nHeight, outpoint})) {
            ++result.missing;
            issue("missing", outpoint, {}, uint32_t{coin.nHeight});
        }
    }
    cursor->CheckStatus();
    for (cursor->Seek(DB_RECYCLE_HEIGHT); cursor->Valid(); cursor->Next()) {
        uint8_t prefix{0};
        if (!cursor->GetKey(prefix) || prefix != DB_RECYCLE_HEIGHT) break;
        if (!proceed("index", result.index_entries)) return result;
        ++result.index_entries;
        HeightEntry entry;
        uint8_t placeholder{1};
        if (!cursor->GetKey(entry, true) || !cursor->GetValue(placeholder, true) || placeholder != 0) {
            ++result.malformed_index_entries;
            issue("malformed_index_entry");
            continue;
        }
        const CoinEntry key{&entry.outpoint};
        Coin coin;
        const auto read{snapshot.ReadStatus(key, coin)};
        if (read == CDBSnapshot::ReadResult::NOT_FOUND) {
            ++result.orphaned;
            issue("orphaned", entry.outpoint, entry.height);
            continue;
        }
        if (read != CDBSnapshot::ReadResult::OK || coin.IsSpent()) continue; // Counted by the coin pass.
        if (coin.out.scriptPubKey.IsUnspendable()) {
            ++result.unexpected;
            issue("unexpected", entry.outpoint, entry.height, uint32_t{coin.nHeight});
        } else if (coin.nHeight != entry.height) {
            ++result.height_mismatches;
            issue("height_mismatch", entry.outpoint, entry.height, uint32_t{coin.nHeight});
        }
    }
    cursor->CheckStatus();
    if (m_impl->control->cancel.load() || !progress("complete", result)) return result;
    result.completed = true;
    return result;
}

std::unique_ptr<RecycleIndexSnapshot> CCoinsViewDB::GetRecycleIndexSnapshot()
{
    AssertLockHeld(cs_main);
    if (m_options.recycle_mode != RecycleMode::INDEX) throw std::runtime_error("Recycle height index is disabled");
    if (!m_recycle_check.expired()) return {};
    auto control{std::make_shared<RecycleIndexScanControl>()};
    auto snapshot{std::unique_ptr<RecycleIndexSnapshot>(new RecycleIndexSnapshot{m_db, control})};
    m_recycle_check = control;
    return snapshot;
}

void CCoinsViewDB::CancelRecycleIndexCheck()
{
    if (auto control{m_recycle_check.lock()}) {
        control->cancel = true;
        std::unique_lock lock{control->mutex};
        control->released_cv.wait(lock, [&] { return control->released; });
    }
}

bool CCoinsViewDB::RebuildRecycleIndex(const std::function<bool()>& interrupted)
{
    if (m_options.recycle_mode != RecycleMode::INDEX) return true;
    if (HasUnsupportedFormat()) throw std::runtime_error("Unsupported chainstate format; synchronize a fresh data directory");
    m_db->Write(DB_RECYCLE_BUILDING, true, /*fSync=*/true);
    CDBBatch batch{*m_db};
    auto flush = [&] {
        if (batch.ApproximateSize() > m_options.batch_write_bytes) {
            m_db->WriteBatch(batch);
            batch.Clear();
        }
    };
    // Restartable rebuild; never trust an incomplete prior index.
    std::unique_ptr<CDBIterator> cursor{m_db->NewIterator()};
    for (cursor->Seek(DB_RECYCLE_HEIGHT); cursor->Valid(); cursor->Next()) {
        uint8_t prefix;
        if (!cursor->GetKey(prefix)) throw std::runtime_error("Unreadable recycle index key");
        if (prefix != DB_RECYCLE_HEIGHT) break;
        if (interrupted()) return false;
        HeightEntry entry;
        if (!cursor->GetKey(entry)) throw std::runtime_error("Corrupt recycle index key");
        batch.Erase(entry);
        flush();
    }
    cursor->CheckStatus();
    m_db->WriteBatch(batch);
    batch.Clear();
    cursor.reset(m_db->NewIterator());
    for (cursor->Seek(DB_COIN); cursor->Valid(); cursor->Next()) {
        uint8_t prefix;
        if (!cursor->GetKey(prefix)) throw std::runtime_error("Unreadable coin key");
        if (prefix != DB_COIN) break;
        if (interrupted()) return false;
        COutPoint outpoint;
        CoinEntry entry{&outpoint};
        if (!cursor->GetKey(entry)) throw std::runtime_error("Corrupt coin key");
        Coin coin;
        if (!cursor->GetValue(coin)) throw std::runtime_error("Corrupt coin during recycle index rebuild");
        if (!coin.out.scriptPubKey.IsUnspendable()) {
            batch.Write(HeightEntry{DB_RECYCLE_HEIGHT, coin.nHeight, outpoint}, uint8_t{0});
        }
        flush();
    }
    cursor->CheckStatus();
    batch.Erase(DB_RECYCLE_BUILDING);
    m_db->WriteBatch(batch, /*fSync=*/true);
    LogInfo("Rebuilt recycle height index from current UTXOs");
    return true;
}

std::set<COutPoint> CCoinsViewDB::GetCoinsByHeight(uint32_t height) const
{
    if (m_options.recycle_mode != RecycleMode::INDEX || !RecycleStateReady()) {
        throw std::runtime_error("Recycle height index is not ready");
    }
    std::set<COutPoint> outputs;
    std::unique_ptr<CDBIterator> cursor{m_db->NewIterator()};
    for (cursor->Seek(HeightEntry{DB_RECYCLE_HEIGHT, height}); cursor->Valid(); cursor->Next()) {
        uint8_t prefix;
        if (!cursor->GetKey(prefix)) throw std::runtime_error("Unreadable recycle index key");
        if (prefix != DB_RECYCLE_HEIGHT) break;
        HeightEntry entry;
        if (!cursor->GetKey(entry)) throw std::runtime_error("Corrupt recycle index key");
        if (entry.height != height) break;
        outputs.insert(entry.outpoint);
    }
    cursor->CheckStatus();
    return outputs;
}

CCoinsViewDB::CCoinsViewDB(DBParams db_params, CoinsViewOptions options) :
    m_db_params{std::move(db_params)},
    m_options{std::move(options)},
    m_db{std::make_shared<CDBWrapper>(m_db_params)}
{
    if (!m_db->Exists(DB_RECYCLE_FORMAT)) {
        auto cursor{std::unique_ptr<CDBIterator>{m_db->NewIterator()}};
        cursor->SeekToFirst();
        std::string key;
        if (cursor->Valid() && cursor->GetKey(key, true) && key == std::string{"\000obfuscate_key", 14}) cursor->Next();
        cursor->CheckStatus();
        if (!cursor->Valid()) {
            CDBBatch batch{*m_db};
            batch.Write(DB_RECYCLE_FORMAT, RECYCLE_FORMAT_VERSION);
            batch.Write(DB_RECYCLE_MODE, static_cast<int>(m_options.recycle_mode));
            batch.Write(DB_RECYCLE_POOL, CAmount{0});
            m_db->WriteBatch(batch, /*fSync=*/true);
        }
    }
}

CCoinsViewDB::~CCoinsViewDB()
{
    CancelRecycleIndexCheck();
    if (m_compaction.valid()) {
        if (m_compaction.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            LogInfo("Waiting for background chainstate compaction of %s", fs::PathToString(m_db_params.path));
        }
        m_compaction.wait();
    }
}

void CCoinsViewDB::ResizeCache(size_t new_cache_size)
{
    // We can't do this operation with an in-memory DB since we'll lose all the coins upon
    // reset.
    if (!m_db_params.memory_only) {
        CancelRecycleIndexCheck();
        LOCK(m_db_mutex);
        // Have to do a reset first to get the original `m_db` state to release its
        // filesystem lock.
        m_db.reset();
        m_db_params.cache_bytes = new_cache_size;
        m_db_params.wipe_data = false;
        m_db = std::make_shared<CDBWrapper>(m_db_params);
    }
}

std::optional<Coin> CCoinsViewDB::GetCoin(const COutPoint& outpoint) const
{
    Coin coin;
    const CDBWrapper::ReadStatus res = m_db->TryRead(CoinEntry(&outpoint), coin);
    if (!res) {
        // Propagate errors so CCoinsViewErrorCatcher triggers a clean shutdown.
        switch (const auto& [err_code, err_msg] = res.error(); err_code) {
            case CDBWrapper::ReadFailure::Code::DeserializationError:
                throw dbwrapper_error{strprintf("Coin deserialization failure: %s", err_msg)};
            case CDBWrapper::ReadFailure::Code::DatabaseError:
                throw dbwrapper_error{strprintf("Coin DB read failure: %s", err_msg)};
        } // no default case, so the compiler can warn about missing cases
        std::abort(); // unreachable
    }

    // Check whether the coin exists
    if (!res.value()) return std::nullopt;
    // Coin found, ensure UTXO database never contains spent coins
    Assert(!coin.IsSpent());
    return coin;
}

std::optional<Coin> CCoinsViewDB::PeekCoin(const COutPoint& outpoint) const
{
    return GetCoin(outpoint);
}

bool CCoinsViewDB::HaveCoin(const COutPoint& outpoint) const
{
    return m_db->Exists(CoinEntry(&outpoint));
}

CAmount CCoinsViewDB::GetRecyclePool() const
{
    CAmount pool{0};
    if (!m_db->Read(DB_RECYCLE_POOL, pool) || !MoneyRange(pool)) {
        throw std::runtime_error("Missing or invalid recycle pool balance");
    }
    return pool;
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!m_db->Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

std::vector<uint256> CCoinsViewDB::GetHeadBlocks() const {
    std::vector<uint256> vhashHeadBlocks;
    if (!m_db->Read(DB_HEAD_BLOCKS, vhashHeadBlocks)) {
        return std::vector<uint256>();
    }
    return vhashHeadBlocks;
}

void CCoinsViewDB::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    CDBBatch batch(*m_db);
    size_t count = 0;
    const size_t dirty_count{cursor.GetDirtyCount()};
    assert(!block_hash.IsNull());

    uint256 old_tip = GetBestBlock();
    if (old_tip.IsNull()) {
        // We may be in the middle of replaying.
        std::vector<uint256> old_heads = GetHeadBlocks();
        if (old_heads.size() == 2) {
            if (old_heads[0] != block_hash) {
                LogError("The coins database detected an inconsistent state, likely due to a previous crash or shutdown. You will need to restart bitcoind with the -reindex-chainstate or -reindex configuration option.\n");
            }
            assert(old_heads[0] == block_hash);
            old_tip = old_heads[1];
        }
    }

    if (dirty_count > WARN_FLUSH_COINS_COUNT) LogWarning("Flushing large (%d entries) UTXO set to disk, it may take several minutes", dirty_count);
    LOG_TIME_MILLIS_WITH_CATEGORY(strprintf("write coins cache to disk (%d out of %d cached coins)",
        dirty_count, cursor.GetTotalCount()), BCLog::BENCH);

    // In the first batch, mark the database as being in the middle of a
    // transition from old_tip to block_hash.
    // A vector is used for future extensibility, as we may want to support
    // interrupting after partial writes from multiple independent reorgs.
    batch.Erase(DB_BEST_BLOCK);
    batch.Write(DB_HEAD_BLOCKS, Vector(block_hash, old_tip));

    for (auto it{cursor.Begin()}; it != cursor.End();) {
        if (it->second.IsDirty()) {
            CoinEntry entry(&it->first);
            if (m_options.recycle_mode == RecycleMode::INDEX) {
                // The old height is no longer present in a spent cache entry.
                // Read the persisted coin and update both keys in the SAME batch,
                // including across interrupted flushes and BIP30 overwrites.
                if (const auto old{GetCoin(it->first)}; old && !old->out.scriptPubKey.IsUnspendable()) {
                    batch.Erase(HeightEntry{DB_RECYCLE_HEIGHT, old->nHeight, it->first});
                }
                if (!it->second.coin.IsSpent() && !it->second.coin.out.scriptPubKey.IsUnspendable()) {
                    batch.Write(HeightEntry{DB_RECYCLE_HEIGHT, it->second.coin.nHeight, it->first}, uint8_t{0});
                }
            }
            if (it->second.coin.IsSpent()) {
                batch.Erase(entry);
            } else {
                batch.Write(entry, it->second.coin);
            }
        }
        count++;
        it = cursor.NextAndMaybeErase(*it);
        if (batch.ApproximateSize() > m_options.batch_write_bytes) {
            LogDebug(BCLog::COINDB, "Writing partial batch of %.2f MiB\n", batch.ApproximateSize() / double(1_MiB));

            m_db->WriteBatch(batch);
            batch.Clear();
            if (m_options.simulate_crash_ratio) {
                static FastRandomContext rng;
                if (rng.randrange(m_options.simulate_crash_ratio) == 0) {
                    LogError("Simulating a crash. Goodbye.");
                    _Exit(0);
                }
            }
        }
    }

    // In the last batch, mark the database as consistent with block_hash again.
    batch.Erase(DB_HEAD_BLOCKS);
    batch.Write(DB_BEST_BLOCK, block_hash);
    // Publish the pool with the final tip marker. Partial coin batches remain
    // covered by DB_HEAD_BLOCKS and idempotent undo replay after a crash.
    if (const auto pool{cursor.GetRecyclePool()}) batch.Write(DB_RECYCLE_POOL, *pool);

    LogDebug(BCLog::COINDB, "Writing final batch of %.2f MiB\n", batch.ApproximateSize() / double(1_MiB));
    m_db->WriteBatch(batch);
    LogDebug(BCLog::COINDB, "Committed %u changed transaction outputs (out of %u) to coin database...", (unsigned int)dirty_count, (unsigned int)count);
}

size_t CCoinsViewDB::EstimateSize() const
{
    return m_db->EstimateSize(DB_COIN, uint8_t(DB_COIN + 1)) +
        m_db->EstimateSize(DB_RECYCLE_HEIGHT, uint8_t(DB_RECYCLE_HEIGHT + 1));
}

std::optional<std::string> CCoinsViewDB::GetDBProperty(const std::string& property)
{
    return m_db->GetProperty(property);
}

std::shared_future<void> CCoinsViewDB::CompactFullAsync()
{
    AssertLockHeld(::cs_main);
    if (m_compaction.valid() && m_compaction.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return m_compaction;
    m_compaction = std::async(std::launch::async, [this] {
        try {
            util::ThreadRename("utxocompact");
            LOCK(m_db_mutex);

            LogDebug(BCLog::COINDB, "Starting chainstate compaction of %s", fs::PathToString(m_db_params.path));
            m_db->CompactFull();
            LogDebug(BCLog::COINDB, "Finished chainstate compaction of %s", fs::PathToString(m_db_params.path));
        } catch (const std::exception& e) {
            LogWarning("Failed chainstate compaction (%s)", e.what());
        }
    }).share();
    return m_compaction;
}

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    // Prefer using CCoinsViewDB::Cursor() since we want to perform some
    // cache warmup on instantiation.
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256&in_block_hash, CAmount pool):
        CCoinsViewCursor(in_block_hash), pcursor(pcursorIn), m_recycle_pool(pool) {}
    ~CCoinsViewDBCursor() = default;

    CAmount GetRecyclePool() const override { return m_recycle_pool; }
    bool GetKey(COutPoint &key) const override;
    bool GetValue(Coin &coin) const override;

    bool Valid() const override;
    void Next() override;

private:
    std::unique_ptr<CDBIterator> pcursor;
    CAmount m_recycle_pool;
    std::pair<char, COutPoint> keyTmp;

    friend class CCoinsViewDB;
};

std::unique_ptr<CCoinsViewCursor> CCoinsViewDB::Cursor() const
{
    auto i = std::make_unique<CCoinsViewDBCursor>(
        const_cast<CDBWrapper&>(*m_db).NewIterator(), GetBestBlock(), GetRecyclePool());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COIN);
    // Cache key of first record
    if (i->pcursor->Valid()) {
        CoinEntry entry(&i->keyTmp.second);
        i->pcursor->GetKey(entry);
        i->keyTmp.first = entry.key;
    } else {
        i->keyTmp.first = 0; // Make sure Valid() and GetKey() return false
    }
    return i;
}

bool CCoinsViewDBCursor::GetKey(COutPoint &key) const
{
    // Return cached key
    if (keyTmp.first == DB_COIN) {
        key = keyTmp.second;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(Coin &coin) const
{
    return pcursor->GetValue(coin);
}

bool CCoinsViewDBCursor::Valid() const
{
    return keyTmp.first == DB_COIN;
}

void CCoinsViewDBCursor::Next()
{
    pcursor->Next();
    CoinEntry entry(&keyTmp.second);
    if (!pcursor->Valid() || !pcursor->GetKey(entry)) {
        keyTmp.first = 0; // Invalidate cached key after last record so that Valid() and GetKey() return false
    } else {
        keyTmp.first = entry.key;
    }
}
