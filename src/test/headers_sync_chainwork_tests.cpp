// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <headerssync.h>
#include <net_processing.h>
#include <pow.h>
#include <pow/asert.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <algorithm>
#include <cstddef>
#include <vector>

#include <boost/test/unit_test.hpp>

using State = HeadersSyncState::State;

// Standard set of checks common to all scenarios. Macro keeps failure lines at the call-site.
#define CHECK_RESULT(result_expression, hss, exp_state, exp_success, exp_request_more,                   \
                     exp_headers_size, exp_pow_validated_prev, exp_locator_hash)                         \
    do {                                                                                                 \
        const auto result{result_expression};                                                            \
        BOOST_REQUIRE_EQUAL(hss.GetState(), exp_state);                                                  \
        BOOST_CHECK_EQUAL(result.success, exp_success);                                                  \
        BOOST_CHECK_EQUAL(result.request_more, exp_request_more);                                        \
        BOOST_CHECK_EQUAL(result.pow_validated_headers.size(), exp_headers_size);                        \
        const std::optional<uint256> pow_validated_prev_opt{exp_pow_validated_prev};                     \
        if (pow_validated_prev_opt) {                                                                    \
            BOOST_CHECK_EQUAL(result.pow_validated_headers.at(0).hashPrevBlock, pow_validated_prev_opt); \
        } else {                                                                                         \
            BOOST_CHECK_EQUAL(exp_headers_size, 0);                                                      \
        }                                                                                                \
        const std::optional<uint256> locator_hash_opt{exp_locator_hash};                                 \
        if (locator_hash_opt) {                                                                          \
            BOOST_CHECK_EQUAL(hss.NextHeadersRequestLocator().vHave.at(0), locator_hash_opt);            \
        } else {                                                                                         \
            BOOST_CHECK_EQUAL(exp_state, State::FINAL);                                                  \
        }                                                                                                \
    } while (false)

constexpr size_t TARGET_BLOCKS{15'000};
constexpr arith_uint256 CHAIN_WORK{TARGET_BLOCKS * 2};

// Subtract MAX_HEADERS_RESULTS (2000 headers/message) + an arbitrary smaller
// value (123) so our redownload buffer is well below the number of blocks
// required to reach the CHAIN_WORK threshold, to behave similarly to mainnet.
constexpr size_t REDOWNLOAD_BUFFER_SIZE{TARGET_BLOCKS - (MAX_HEADERS_RESULTS + 123)};
constexpr size_t COMMITMENT_PERIOD{600}; // Somewhat close to mainnet.

struct HeadersGeneratorSetup : public RegTestingSetup {
    const CBlock& genesis{Params().GenesisBlock()};
    CBlockIndex& chain_start{WITH_LOCK(::cs_main, return *Assert(m_node.chainman->m_blockman.LookupBlockIndex(genesis.GetHash())))};

    // Generate headers for two different chains (using differing merkle roots
    // to ensure the headers are different).
    const std::vector<CBlockHeader>& FirstChain()
    {
        // Block header hash target is half of max uint256 (2**256 / 2), expressible
        // roughly as the coefficient 0x7fffff with the exponent 0x20 (32 bytes).
        // This implies around every 2nd hash attempt should succeed, which
        // is why CHAIN_WORK == TARGET_BLOCKS * 2.
        assert(genesis.nBits == 0x207fffff);

        // Subtract 1 since the genesis block also contributes work so we reach
        // the CHAIN_WORK target.
        static const auto first_chain{GenerateHeaders(/*count=*/TARGET_BLOCKS - 1, genesis.GetHash(),
                genesis.nVersion, genesis.nTime, /*merkle_root=*/uint256::ZERO, genesis.nBits)};
        return first_chain;
    }
    const std::vector<CBlockHeader>& SecondChain()
    {
        // Subtract 2 to keep total work below the target.
        static const auto second_chain{GenerateHeaders(/*count=*/TARGET_BLOCKS - 2, genesis.GetHash(),
                genesis.nVersion, genesis.nTime, /*merkle_root=*/uint256::ONE, genesis.nBits)};
        return second_chain;
    }

    HeadersSyncState CreateState()
    {
        return {/*id=*/0,
                Params().GetConsensus(),
                HeadersSyncParams{
                    .commitment_period = COMMITMENT_PERIOD,
                    .redownload_buffer_size = REDOWNLOAD_BUFFER_SIZE,
                },
                chain_start,
                /*minimum_required_work=*/CHAIN_WORK};
    }

private:
    /** Search for a nonce to meet (regtest) proof of work */
    void FindProofOfWork(CBlockHeader& starting_header);
    /**
     * Generate headers in a chain that build off a given starting hash, using
     * the given nVersion, advancing time by 1 second from the starting
     * prev_time, and with a fixed merkle root hash.
     */
    std::vector<CBlockHeader> GenerateHeaders(size_t count,
            uint256 prev_hash, int32_t nVersion, uint32_t prev_time,
            const uint256& merkle_root, uint32_t nBits);
};

void HeadersGeneratorSetup::FindProofOfWork(CBlockHeader& starting_header)
{
    while (!CheckProofOfWork(starting_header.GetHash(), starting_header.nBits, Params().GetConsensus())) {
        ++starting_header.nNonce;
    }
}

std::vector<CBlockHeader> HeadersGeneratorSetup::GenerateHeaders(
        const size_t count, uint256 prev_hash, const int32_t nVersion,
        uint32_t prev_time, const uint256& merkle_root, const uint32_t nBits)
{
    std::vector<CBlockHeader> headers(count);
    for (auto& next_header : headers) {
        next_header.nVersion = nVersion;
        next_header.hashPrevBlock = prev_hash;
        next_header.hashMerkleRoot = merkle_root;
        next_header.nTime = ++prev_time;
        next_header.nBits = nBits;

        FindProofOfWork(next_header);
        prev_hash = next_header.GetHash();
    }
    return headers;
}

// In this test, we construct two sets of headers from genesis, one with
// sufficient proof of work and one without.
// 1. We deliver the first set of headers and verify that the headers sync state
//    updates to the REDOWNLOAD phase successfully.
//    Then we deliver the second set of headers and verify that they fail
//    processing (presumably due to commitments not matching).
// 2. Verify that repeating with the first set of headers in both phases is
//    successful.
// 3. Repeat the second set of headers in both phases to demonstrate behavior
//    when the chain a peer provides has too little work.
BOOST_FIXTURE_TEST_SUITE(headers_sync_chainwork_tests, HeadersGeneratorSetup)

BOOST_AUTO_TEST_CASE(sneaky_redownload)
{
    const auto& first_chain{FirstChain()};
    const auto& second_chain{SecondChain()};

    // Feed the first chain to HeadersSyncState, by delivering 1 header
    // initially and then the rest.
    HeadersSyncState hss{CreateState()};

    // Just feed one header and check state.
    // Pretend the message is still "full", so we don't abort.
    CHECK_RESULT(hss.ProcessNextHeaders({{first_chain.front()}}, /*full_headers_message=*/true),
        hss, /*exp_state=*/State::PRESYNC,
        /*exp_success=*/true, /*exp_request_more=*/true,
        /*exp_headers_size=*/0, /*exp_pow_validated_prev=*/std::nullopt,
        /*exp_locator_hash=*/first_chain.front().GetHash());

    // This chain should look valid, and we should have met the proof-of-work
    // requirement during PRESYNC and transitioned to REDOWNLOAD.
    CHECK_RESULT(hss.ProcessNextHeaders(std::span{first_chain}.subspan(1), true),
        hss, /*exp_state=*/State::REDOWNLOAD,
        /*exp_success=*/true, /*exp_request_more=*/true,
        /*exp_headers_size=*/0, /*exp_pow_validated_prev=*/std::nullopt,
        /*exp_locator_hash=*/genesis.GetHash());

    // Below is the number of commitment bits that must randomly match between
    // the two chains for this test to spuriously fail. 1 / 2^25 =
    // 1 in 33'554'432 (somewhat less due to HeadersSyncState::m_commit_offset).
    static_assert(TARGET_BLOCKS / COMMITMENT_PERIOD == 25);

    // Try to sneakily feed back the second chain during REDOWNLOAD.
    CHECK_RESULT(hss.ProcessNextHeaders(second_chain, true),
        hss, /*exp_state=*/State::FINAL,
        /*exp_success=*/false, // Foiled! We detected mismatching headers.
        /*exp_request_more=*/false,
        /*exp_headers_size=*/0, /*exp_pow_validated_prev=*/std::nullopt,
        /*exp_locator_hash=*/std::nullopt);
}

BOOST_AUTO_TEST_CASE(happy_path)
{
    const auto& first_chain{FirstChain()};

    // Headers message that moves us to the next state doesn't need to be full.
    for (const bool full_headers_message : {false, true}) {
        // This time we feed the first chain twice.
        HeadersSyncState hss{CreateState()};

        // Sufficient work transitions us from PRESYNC to REDOWNLOAD:
        const auto genesis_hash{genesis.GetHash()};
        CHECK_RESULT(hss.ProcessNextHeaders(first_chain, full_headers_message),
            hss, /*exp_state=*/State::REDOWNLOAD,
            /*exp_success=*/true, /*exp_request_more=*/true,
            /*exp_headers_size=*/0, /*exp_pow_validated_prev=*/std::nullopt,
            /*exp_locator_hash=*/genesis_hash);

        // Process only so that the internal threshold isn't exceeded, meaning
        // validated headers shouldn't be returned yet:
        CHECK_RESULT(hss.ProcessNextHeaders({first_chain.begin(), REDOWNLOAD_BUFFER_SIZE}, true),
            hss, /*exp_state=*/State::REDOWNLOAD,
            /*exp_success=*/true, /*exp_request_more=*/true,
            /*exp_headers_size=*/0, /*exp_pow_validated_prev=*/std::nullopt,
            /*exp_locator_hash=*/first_chain[REDOWNLOAD_BUFFER_SIZE - 1].GetHash());

        // We start receiving headers for permanent storage before completing:
        CHECK_RESULT(hss.ProcessNextHeaders({{first_chain[REDOWNLOAD_BUFFER_SIZE]}}, true),
            hss, /*exp_state=*/State::REDOWNLOAD,
            /*exp_success=*/true, /*exp_request_more=*/true,
            /*exp_headers_size=*/1, /*exp_pow_validated_prev=*/genesis_hash,
            /*exp_locator_hash=*/first_chain[REDOWNLOAD_BUFFER_SIZE].GetHash());

        // Feed in remaining headers, meeting the work threshold again and
        // completing the REDOWNLOAD phase:
        CHECK_RESULT(hss.ProcessNextHeaders({first_chain.begin() + REDOWNLOAD_BUFFER_SIZE + 1, first_chain.end()}, full_headers_message),
            hss, /*exp_state=*/State::FINAL,
            /*exp_success=*/true, /*exp_request_more=*/false,
            // All headers except the one already returned above:
            /*exp_headers_size=*/first_chain.size() - 1, /*exp_pow_validated_prev=*/first_chain.front().GetHash(),
            /*exp_locator_hash=*/std::nullopt);
    }
}

BOOST_AUTO_TEST_CASE(too_little_work)
{
    const auto& second_chain{SecondChain()};

    // Verify that just trying to process the second chain would not succeed
    // (too little work).
    HeadersSyncState hss{CreateState()};
    BOOST_REQUIRE_EQUAL(hss.GetState(), State::PRESYNC);

    // Pretend just the first message is "full", so we don't abort.
    CHECK_RESULT(hss.ProcessNextHeaders({{second_chain.front()}}, true),
        hss, /*exp_state=*/State::PRESYNC,
        /*exp_success=*/true, /*exp_request_more=*/true,
        /*exp_headers_size=*/0, /*exp_pow_validated_prev=*/std::nullopt,
        /*exp_locator_hash=*/second_chain.front().GetHash());

    // Tell the sync logic that the headers message was not full, implying no
    // more headers can be requested. For a low-work-chain, this should cause
    // the sync to end with no headers for acceptance.
    CHECK_RESULT(hss.ProcessNextHeaders(std::span{second_chain}.subspan(1), false),
        hss, /*exp_state=*/State::FINAL,
        // Nevertheless, no validation errors should have been detected with the
        // chain:
        /*exp_success=*/true,
        /*exp_request_more=*/false,
        /*exp_headers_size=*/0, /*exp_pow_validated_prev=*/std::nullopt,
        /*exp_locator_hash=*/std::nullopt);
}

// The formula below is the pre-refactor full-validation calculation, kept
// independent of the shared helper so agreement cannot be tautological.
struct AsertHeaderChain {
    Consensus::Params params;
    std::vector<CBlockHeader> headers{401};
    std::vector<CBlockIndex> indexes{401};
    std::vector<uint256> hashes{401};

    explicit AsertHeaderChain(const CChainParams& chain_params) : params{chain_params.GetConsensus()}
    {
        params.nASERTHalfLife = 172800;
        params.fPowNoRetargeting = false;
        params.powLimit = ArithToUint256(arith_uint256{}.SetCompact(0x1d00ffff));
        headers[0] = chain_params.GenesisBlock();
        headers[0].nBits = 0x1c70ffff;
        for (size_t h{0}; h < headers.size(); ++h) {
            auto& header{headers[h]};
            if (h > 0) {
                const auto& previous{headers[h - 1]};
                header.hashPrevBlock = previous.GetHash();
                const int64_t intervals[]{600, 601, 599, 900, 300, 1, 1199};
                header.nTime = previous.nTime + intervals[h % 7];
                header.nBits = CalculateASERT(arith_uint256{}.SetCompact(headers[0].nBits),
                    params.nPowTargetSpacing,
                    int64_t{previous.nTime} - (int64_t{headers[0].nTime} - params.nPowTargetSpacing),
                    h - 1, UintToArith256(params.powLimit), params.nASERTHalfLife).GetCompact();
            }
            // Synthetic headers exercise target transitions only. The caller
            // checks proof of work before entering HeadersSyncState.
            hashes[h] = header.GetHash();
            indexes[h].nVersion = header.nVersion;
            indexes[h].hashMerkleRoot = header.hashMerkleRoot;
            indexes[h].nTime = header.nTime;
            indexes[h].nBits = header.nBits;
            indexes[h].nNonce = header.nNonce;
            indexes[h].phashBlock = &hashes[h];
            indexes[h].nHeight = h;
            indexes[h].pprev = h ? &indexes[h - 1] : nullptr;
            indexes[h].BuildSkip();
            indexes[h].nChainWork = GetBlockProof(indexes[h]) + (h ? indexes[h - 1].nChainWork : arith_uint256{});
            if (h) BOOST_REQUIRE_EQUAL(GetNextWorkRequired(&indexes[h - 1], &header, params), header.nBits);
        }
    }
};

BOOST_AUTO_TEST_CASE(asert_exact_sync_boundaries)
{
    AsertHeaderChain chain{Params()};
    // Fork-point anchoring, single-header and large batches, including an
    // empty redownload queue between calls. Min-difficulty flags must not
    // bypass ASERT, matching GetNextWorkRequired's precedence.
    for (const bool min_difficulty : {false, true}) {
        chain.params.fPowAllowMinDifficultyBlocks = min_difficulty;
        for (const size_t start : {0, 1, 255}) {
            for (const size_t batch : {1, 17, 2000}) {
                for (const size_t buffer : {0, 1, 11}) {
                    HeadersSyncState sync{0, chain.params, {7, buffer}, chain.indexes[start], chain.indexes.back().nChainWork};
                    std::vector<CBlockHeader> accepted;
                    for (int phase{0}; phase < 2; ++phase) {
                        for (size_t next{start + 1}; next < chain.headers.size(); next += batch) {
                            const auto result{sync.ProcessNextHeaders(std::span{chain.headers}.subspan(next, std::min(batch, chain.headers.size() - next)), true)};
                            BOOST_REQUIRE(result.success);
                            accepted.insert(accepted.end(), result.pow_validated_headers.begin(), result.pow_validated_headers.end());
                        }
                        BOOST_CHECK(sync.GetState() == (phase ? State::FINAL : State::REDOWNLOAD));
                    }
                    BOOST_REQUIRE_EQUAL(accepted.size(), chain.headers.size() - start - 1);
                    for (size_t n{0}; n < accepted.size(); ++n) BOOST_CHECK(accepted[n].GetHash() == chain.hashes[start + n + 1]);
                }
            }
        }
    }
}

class InspectableHeadersSync : public HeadersSyncState {
public:
    using HeadersSyncState::HeadersSyncState;
    size_t CommitmentOffset() const { return m_commit_offset; }
};

BOOST_AUTO_TEST_CASE(asert_reject_in_range_incorrect_targets)
{
    AsertHeaderChain chain{Params()};
    const auto incorrect = [&](size_t height) {
        CBlockHeader bad{chain.headers[height]};
        bad.nBits = 0x1d00ffff;
        BOOST_REQUIRE(bad.nBits != chain.headers[height].nBits);
        BOOST_REQUIRE(DeriveTarget(bad.nBits, chain.params.powLimit));
        return bad;
    };
    for (const size_t bad_height : {1, 17, 255, 400}) {
        HeadersSyncState sync{0, chain.params, {7, 11}, chain.indexes[0], chain.indexes.back().nChainWork};
        if (bad_height > 1) BOOST_REQUIRE(sync.ProcessNextHeaders(std::span{chain.headers}.subspan(1, bad_height - 1), true).success);
        const CBlockHeader bad{incorrect(bad_height)};
        BOOST_CHECK(!sync.ProcessNextHeaders(std::span{&bad, 1}, true).success);
        BOOST_CHECK(sync.GetState() == State::FINAL);
    }
    InspectableHeadersSync sync{0, chain.params, {31, 0}, chain.indexes[0], chain.indexes.back().nChainWork};
    BOOST_REQUIRE(sync.ProcessNextHeaders(std::span{chain.headers}.subspan(1), true).success);
    BOOST_REQUIRE(sync.GetState() == State::REDOWNLOAD);
    // No commitment is checked at the corrupted height: failure must come
    // from exact difficulty validation, not a random commitment mismatch.
    const size_t bad_height{sync.CommitmentOffset() == 1 ? 2U : 1U};
    if (bad_height > 1) BOOST_REQUIRE(sync.ProcessNextHeaders(std::span{chain.headers}.subspan(1, bad_height - 1), true).success);
    const CBlockHeader bad{incorrect(bad_height)};
    BOOST_CHECK(!sync.ProcessNextHeaders(std::span{&bad, 1}, true).success);
    BOOST_CHECK(sync.GetState() == State::FINAL);
}

BOOST_AUTO_TEST_CASE(asert_disabled_retains_no_retarget_behavior)
{
    auto params{Params().GetConsensus()};
    params.nASERTHalfLife = 1;
    BOOST_REQUIRE(params.fPowNoRetargeting);
    const auto& headers{FirstChain()};
    HeadersSyncState sync{0, params, {COMMITMENT_PERIOD, REDOWNLOAD_BUFFER_SIZE}, chain_start, CHAIN_WORK};
    BOOST_REQUIRE(sync.ProcessNextHeaders(headers, true).success);
    BOOST_REQUIRE(sync.ProcessNextHeaders(headers, true).success);
    BOOST_CHECK(sync.GetState() == State::FINAL);
}

BOOST_AUTO_TEST_SUITE_END()
