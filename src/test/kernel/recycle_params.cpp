// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/bitcoinkernel.h>
#include <kernel/chainparams.h>

#include <cassert>

// White-box fixture setup only: the opaque chain-parameters handle wraps a
// CChainParams (see btck_chain_parameters_create). Keep this implementation
// dependency in the tests rather than adding a production C API test option.
// All block processing, disk reads and lifetime checks use the public API.
void EnableRecycleTestParams(btck_ChainParameters* params, int lifetime)
{
    auto& chainparams = *reinterpret_cast<CChainParams*>(params);
    assert(chainparams.GetChainType() == ChainType::REGTEST);
    assert(lifetime > 0);
    auto& consensus = const_cast<Consensus::Params&>(chainparams.GetConsensus());
    consensus.recycle_enabled = true;
    consensus.recycle_expiry_blocks = lifetime;
}
