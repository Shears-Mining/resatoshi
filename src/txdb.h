// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/cs_main.h>
#include <sync.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class COutPoint;
class uint256;
struct RecycleIndexScanControl;

struct RecycleIndexIssue {
    std::string type;
    std::optional<COutPoint> outpoint;
    std::optional<uint32_t> index_height;
    std::optional<uint32_t> coin_height;
};

struct RecycleIndexCheckResult {
    bool completed{false};
    uint64_t coins_scanned{0};
    uint64_t indexed_coins{0};
    uint64_t index_entries{0};
    uint64_t missing{0};
    uint64_t orphaned{0};
    uint64_t height_mismatches{0};
    uint64_t unexpected{0};
    uint64_t malformed_coins{0};
    uint64_t malformed_index_entries{0};
    //! Bounded examples; counters always cover the entire completed scan.
    std::vector<RecycleIndexIssue> issues;
    bool Valid() const;
};

/** Pins the database and one coherent read snapshot without holding cs_main.
 * Cache resizing/shutdown cancel the check and wait for this lease to release.
 */
class RecycleIndexSnapshot {
    friend class CCoinsViewDB;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    RecycleIndexSnapshot(std::shared_ptr<CDBWrapper> db, std::shared_ptr<RecycleIndexScanControl> control);
public:
    ~RecycleIndexSnapshot();
    uint256 GetBestBlock() const;
    //! Run without cs_main; global mutexes use lock exclusion rather than negative capabilities.
    //! The progress callback must not acquire cs_main: resizing may wait under that lock.
    RecycleIndexCheckResult Check(const std::function<bool(const std::string&, const RecycleIndexCheckResult&)>& progress) const LOCKS_EXCLUDED(cs_main);
};

//! In-memory DB behavior; does not change persisted codec values.
enum class RecycleMode { DISABLED, INDEX, BLOCK };

/** Read the mode only from a supported, existing DB. Never initialize metadata. */
std::optional<RecycleMode> ReadRecycleMode(CDBWrapper& db);

//! User-controlled performance and debug options.
struct CoinsViewOptions {
    RecycleMode recycle_mode{RecycleMode::DISABLED};
    //! Maximum database write batch size in bytes.
    uint64_t batch_write_bytes{DEFAULT_DB_CACHE_BATCH};
    //! If non-zero, randomly exit when the database is flushed with (1/ratio) probability.
    int simulate_crash_ratio{0};
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView
{
protected:
    DBParams m_db_params;
    CoinsViewOptions m_options;
    //! Prevents CompactFull() from using m_db while ResizeCache() replaces it.
    Mutex m_db_mutex;
    std::shared_ptr<CDBWrapper> m_db;
    std::weak_ptr<RecycleIndexScanControl> m_recycle_check;
    void CancelRecycleIndexCheck();
    std::shared_future<void> m_compaction;
public:
    explicit CCoinsViewDB(DBParams db_params, CoinsViewOptions options);
    ~CCoinsViewDB() override;

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    std::set<COutPoint> GetCoinsByHeight(uint32_t height) const override;
    CAmount GetRecyclePool() const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;
    //! Get a cursor to iterate over the whole state.
    std::unique_ptr<CCoinsViewCursor> Cursor() const override;

    //! Whether the database format or persisted mode differs from this view.
    bool HasUnsupportedFormat();
    bool RecycleStateReady() const;
    /** Rebuild the derived index from current-format UTXOs after detected damage. */
    bool RebuildRecycleIndex(const std::function<bool()>& interrupted);
    //! Capture only after a complete flush. Returns null if a check is already leased.
    std::unique_ptr<RecycleIndexSnapshot> GetRecycleIndexSnapshot() EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    size_t EstimateSize() const override;

    //! Dynamically alter the underlying leveldb cache size.
    void ResizeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_db_mutex);

    //! Perform a full compaction of the underlying LevelDB on a one-shot background thread.
    std::shared_future<void> CompactFullAsync() EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_db_mutex);

    //! Return an underlying LevelDB property value, if available.
    std::optional<std::string> GetDBProperty(const std::string& property);
};

#endif // BITCOIN_TXDB_H
