// Copyright (c) 2022-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <headerssync.h>
#include <pow.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <iterator>
#include <span>
#include <vector>

static void initialize_headers_sync_state_fuzz()
{
    static const auto testing_setup = MakeNoLogFileContext<>(
        /*chain_type=*/ChainType::MAIN);
}

void MakeHeadersContinuous(
    const CBlockHeader& genesis_header,
    const std::vector<CBlockHeader>& all_headers,
    std::vector<CBlockHeader>& new_headers)
{
    Assume(!new_headers.empty());

    const CBlockHeader* prev_header{
        all_headers.empty() ? &genesis_header : &all_headers.back()};

    for (auto& header : new_headers) {
        header.hashPrevBlock = prev_header->GetHash();

        prev_header = &header;
    }
}

class FuzzedHeadersSyncState : public HeadersSyncState
{
public:
    FuzzedHeadersSyncState(const HeadersSyncParams& sync_params, const size_t commit_offset,
                           const CBlockIndex& chain_start, const arith_uint256& minimum_required_work)
        : HeadersSyncState(/*id=*/0, Params().GetConsensus(), sync_params, chain_start, minimum_required_work)
    {
        const_cast<size_t&>(m_commit_offset) = commit_offset;
    }
};

FUZZ_TARGET(headers_sync_state, .init = initialize_headers_sync_state_fuzz)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    CBlockHeader genesis_header{Params().GenesisBlock()};
    CBlockIndex start_index(genesis_header);

    FakeNodeClock clock{ConsumeTime(fuzzed_data_provider, /*min=*/start_index.GetMedianTimePast())};

    const uint256 genesis_hash = genesis_header.GetHash();
    start_index.phashBlock = &genesis_hash;

    const HeadersSyncParams params{
        .commitment_period = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(1, Params().HeadersSync().commitment_period * 2),
        .redownload_buffer_size = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, Params().HeadersSync().redownload_buffer_size * 2),
    };
    arith_uint256 min_work{UintToArith256(ConsumeUInt256(fuzzed_data_provider))};
    FuzzedHeadersSyncState headers_sync(
        params,
        /*commit_offset=*/fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, params.commitment_period - 1),
        /*chain_start=*/start_index,
        /*minimum_required_work=*/min_work);

    // Store headers for potential redownload phase.
    std::vector<CBlockHeader> all_headers;
    std::vector<CBlockHeader>::const_iterator redownloaded_it;
    bool presync{true};
    bool requested_more{true};

    while (requested_more) {
        std::vector<CBlockHeader> headers;

        // Consume headers from fuzzer or maybe replay headers if we got to the
        // redownload phase.
        if (presync || fuzzed_data_provider.ConsumeBool()) {
            auto deser_headers = ConsumeDeserializable<std::vector<CBlockHeader>>(fuzzed_data_provider);
            if (!deser_headers || deser_headers->empty()) return;

            if (fuzzed_data_provider.ConsumeBool()) {
                MakeHeadersContinuous(genesis_header, all_headers, *deser_headers);
            }

            headers.swap(*deser_headers);
        } else if (auto num_headers_left{std::distance(redownloaded_it, all_headers.cend())}; num_headers_left > 0) {
            // Consume some headers from the redownload buffer (At least one
            // header is consumed).
            auto begin_it{redownloaded_it};
            std::advance(redownloaded_it, fuzzed_data_provider.ConsumeIntegralInRange<int>(1, num_headers_left));
            headers.insert(headers.cend(), begin_it, redownloaded_it);
        }

        if (headers.empty()) return;
        auto result = headers_sync.ProcessNextHeaders(headers, fuzzed_data_provider.ConsumeBool());
        requested_more = result.request_more;

        if (result.request_more) {
            if (presync) {
                all_headers.insert(all_headers.cend(), headers.cbegin(), headers.cend());

                if (headers_sync.GetState() == HeadersSyncState::State::REDOWNLOAD) {
                    presync = false;
                    redownloaded_it = all_headers.cbegin();

                    // If we get to redownloading, the presynced headers need
                    // to have the min amount of work on them.
                    assert(CalculateClaimedHeadersWork(all_headers) >= min_work);
                }
            }

            (void)headers_sync.NextHeadersRequestLocator();
        }
    }
}

// Exercise deep ASERT state transitions on every input. The raw-header target
// above retains malformed-input exploration; this target supplies exact targets
// and a reachable work threshold, then checks an explicitly corrupted batch.
FUZZ_TARGET(headers_sync_asert, .init = initialize_headers_sync_state_fuzz)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    const auto& consensus = Params().GetConsensus();
    const CBlockHeader genesis{Params().GenesisBlock()};
    const auto genesis_hash = genesis.GetHash();
    CBlockIndex anchor{genesis};
    anchor.phashBlock = &genesis_hash;
    SetMockTime(int64_t{genesis.nTime} + 86400 * 2);

    const HeadersSyncParams params{
        .commitment_period = provider.ConsumeIntegralInRange<size_t>(1, 16),
        .redownload_buffer_size = provider.ConsumeIntegralInRange<size_t>(0, 32),
    };
    const size_t count = provider.ConsumeIntegralInRange<size_t>(65, 128);
    std::vector<CBlockHeader> headers;
    arith_uint256 work;
    for (size_t i = 0; i < count; ++i) {
        const auto& previous = headers.empty() ? genesis : headers.back();
        CBlockHeader header;
        header.hashPrevBlock = previous.GetHash();
        // Vary timestamps independently while avoiding chainwork overflow. MTP
        // and real proof of work belong to the caller, not HeadersSyncState.
        header.nTime = genesis.nTime + provider.ConsumeIntegralInRange<uint32_t>(0, 86400);
        header.nBits = GetNextASERTWorkRequired(anchor, i, previous.nTime, consensus);
        header.hashMerkleRoot = ConsumeUInt256(provider);
        work += GetBlockProof(CBlockIndex{header});
        headers.push_back(header);
    }

    HeadersSyncState state{0, consensus, params, anchor, work};
    for (size_t pos = 0; pos < count;) {
        const size_t batch = provider.ConsumeIntegralInRange<size_t>(1, std::min<size_t>(16, count - pos));
        const auto result = state.ProcessNextHeaders(std::span{headers}.subspan(pos, batch), true);
        assert(result.success && result.request_more && result.pow_validated_headers.empty());
        pos += batch;
        assert(state.GetState() == (pos == count ? HeadersSyncState::State::REDOWNLOAD : HeadersSyncState::State::PRESYNC));
    }
    size_t released{0};
    bool released_before_end{false};
    for (size_t pos = 0; pos < count;) {
        const size_t batch = provider.ConsumeIntegralInRange<size_t>(1, std::min<size_t>(16, count - pos));
        const auto result = state.ProcessNextHeaders(std::span{headers}.subspan(pos, batch), true);
        assert(result.success);
        pos += batch;
        for (const auto& header : result.pow_validated_headers) {
            assert(released < count);
            assert(header.GetHash() == headers[released].GetHash());
            ++released;
        }
        if (pos < count && released > 0) released_before_end = true;
        assert(result.request_more == (pos < count));
    }
    assert(released_before_end && released == count);
    assert(state.GetState() == HeadersSyncState::State::FINAL);

    HeadersSyncState invalid{0, consensus, params, anchor, work};
    if (provider.ConsumeBool()) {
        const auto result = invalid.ProcessNextHeaders(headers, true);
        assert(result.success && invalid.GetState() == HeadersSyncState::State::REDOWNLOAD);
    }
    // A late failure must not release the valid prefix of this same batch.
    headers.back().nBits ^= 1;
    const auto rejected = invalid.ProcessNextHeaders(headers, true);
    assert(!rejected.success && !rejected.request_more && rejected.pow_validated_headers.empty());
    assert(invalid.GetState() == HeadersSyncState::State::FINAL);
}
