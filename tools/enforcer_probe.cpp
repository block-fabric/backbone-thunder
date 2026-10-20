// bt-enforcer-probe: exercise a cusf.mainchain.v1 server (real enforcer or backboned) with
// the calls a Thunder node makes, and print what it answers.
//   bt-enforcer-probe https://enforcer.beta.ecash.ninja [slot]
#include <bt/enforcer.h>
#include <bt/parse.h>

#include <cstdio>

using namespace bt;

int main(int argc, char** argv)
{
    std::string url = argc > 1 ? argv[1] : "http://localhost:50051";
    uint8_t slot = argc > 2 ? uint8_t(std::min<uint64_t>(bt::parse_u64(argv[2]).value_or(9), 255)) : 9;
    enforcer::Client c(url, slot);
    auto h = c.health("cusf.mainchain.v1.ValidatorService");
    std::printf("health: %s\n", h ? (*h ? "SERVING" : "NOT_SERVING") : "unavailable");
    auto ci = c.get_chain_info();
    if (enforcer::ok(ci)) std::printf("network %d activation %u\n", enforcer::val(ci).network, enforcer::val(ci).activation_height.value_or(0));
    else std::printf("GetChainInfo: %s\n", enforcer::err(ci).what().c_str());
    auto tip = c.get_chain_tip();
    if (!enforcer::ok(tip)) {
        std::printf("GetChainTip: %s\n", enforcer::err(tip).what().c_str());
        return 1;
    }
    auto& t = enforcer::val(tip);
    std::printf("tip %u %s\n", t.height, hex_encode_rev(t.block_hash.span()).c_str());
    auto infos = c.get_block_infos(t.block_hash, 999);
    if (!enforcer::ok(infos)) {
        std::printf("GetBlockInfo: %s\n", enforcer::err(infos).what().c_str());
        return 1;
    }
    size_t bmm = 0, ev = 0;
    for (auto& [hi, bi] : enforcer::val(infos)) {
        if (bi.bmm_commitment) bmm++;
        ev += bi.events.size();
    }
    std::printf("GetBlockInfo(tip, 999): %zu infos (contiguous), %zu BMM commitments, %zu events, oldest %u\n", enforcer::val(infos).size(), bmm, ev,
                enforcer::val(infos).back().first.height);
    auto cm = c.get_bmm_hstar_commitments(t.block_hash, 10);
    if (enforcer::ok(cm)) std::printf("GetBmmHStarCommitment(tip, 10): %zu entries, newest %s\n", enforcer::val(cm).size(),
                                      enforcer::val(cm)[0] ? hex_encode(*enforcer::val(cm)[0]).c_str() : "none");
    return 0;
}
