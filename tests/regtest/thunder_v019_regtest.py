#!/usr/bin/env python3
"""Side-by-side regtest of the v0.19.1 rules (LayerTwo's mainnet rules): THEIR thunder-rust v0.19.1 (b44ac16,
thunder_app) and OUR thunder with --bt-consensus v0.19 on one eCash regtest chain (ecashd + electrs +
bip300301_enforcer), peered only with each other, the harness of thunder_reorg_regtest.py. After every step both
nodes' JSON-RPC replies are compared byte for byte (UTXO lists as sets).

Covered: blocks mined by each node; deposits and their 50-block maturity (a transfer of an immature deposit is
refused by both); transfers (blocks with authorizations, now in the merkle root); a withdrawal (the bundle built
after the block's transactions, pending, then submitted or dropped after 4 blocks with its outputs recreated at
UnsuccessfulWithdrawal outpoints, spent again by a transfer); coinbase maturity (50 sidechain blocks); a mainchain
reorg; the whole history at the end.

    thunder_v019_regtest.py full   # up, scenario, down
    thunder_v019_regtest.py down

Binaries: theirs = $TH_THEIRS (default $SKY/ext/thunder-rust-019/target/release/thunder_app); the rest as
thunder_reorg_regtest.py. Ports TH_BASE.. (default 49660). Datadirs under TH_DIR (default <tmp>/backbone-thunder-v019).
"""
import os
import sys
import tempfile
import time
from pathlib import Path

os.environ.setdefault("TH_BASE", "49660")
os.environ.setdefault("TH_DIR", str(Path(tempfile.gettempdir()) / "backbone-thunder-v019"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import thunder_reorg_regtest as base  # noqa: E402

base.BIN["theirs"] = Path(os.environ.get("TH_THEIRS", base.SKY / "ext/thunder-rust-019/target/release/thunder_app"))
_spawn = base.spawn


def spawn(name, argv):
    if name == "ours":
        argv = list(argv) + ["--bt-consensus", "v0.19", "--bt-wallet-derivation", "v0.19"]
    return _spawn(name, argv)


base.spawn = spawn
log, THEIRS, OURS, NODES, same, compare_all = base.log, base.THEIRS, base.OURS, base.NODES, base.same, base.compare_all
STATE, CHECKS = base.STATE, base.CHECKS

def step(what):
    log(f"== {what}")


def mine(s, n=1):
    for _ in range(n):
        base.mine_and_compare(s)


def wallet_same():
    # the wallets come from different mnemonics: their balances are logged, the UTXO set compared
    same("list_utxos", sets=True)
    for s in NODES:
        log(f"  {s.name} wallet: {len(s.call('get_wallet_addresses'))} addresses, balance {s.call('balance')}")


def deposit(s, to, sats, fee=1000):
    txid = s.call("create_deposit", to, sats, fee)
    log(f"  deposit {sats} sats to {to} by {s.name}: {txid}")
    return txid


def both_refuse(method, *params):
    r = {}
    for s in NODES:
        try:
            s.call(method, *params)
            r[s.name] = "accepted"
        except RuntimeError as e:
            r[s.name] = "refused: " + str(e)[:160]
    log(f"  {method}: {r}")
    return r


def full():
    base.up()
    step("blocks, alternately mined")
    mine(THEIRS)
    mine(OURS)
    step("deposits to both wallets")
    # one deposit per mainchain block: this enforcer builds no template with two deposits to one slot
    deposit(THEIRS, STATE["theirs_addr"], 500_000)
    base.enforcer_mine(1)
    deposit(OURS, STATE["ours_addr"], 400_000)
    base.enforcer_mine(1)
    mine(OURS)  # the deposit blocks' 2WPD comes with the next BMM
    mine(THEIRS)
    wallet_same()
    step("an immature deposit: each wallet refuses to spend it (not enough mature funds)")
    STATE["immature"] = both_refuse("create_transfer", STATE["ours_addr"], 1000, 100)
    step("50 mainchain blocks: the deposits mature")
    base.enforcer_mine(50)
    mine(THEIRS)
    mine(OURS)
    step("transfers each way (authorizations in the merkle root)")
    t1 = THEIRS.call("create_transfer", STATE["ours_addr"], 120_000, 1000)
    t2 = OURS.call("create_transfer", STATE["theirs_addr"], 80_000, 1000)
    log(f"  transfers {t1} {t2}")
    time.sleep(3)
    mine(OURS)
    mine(THEIRS)
    wallet_same()
    step("withdrawals: the bundle built after the block's transactions")
    main_addr = base.grpc("WalletService/CreateNewAddress")["address"]
    w1 = THEIRS.call("create_withdrawal", main_addr, 50_000, 2_000, 1_000)
    w2 = OURS.call("create_withdrawal", main_addr, 30_000, 1_500, 1_000)
    log(f"  withdrawals {w1} {w2} to {main_addr}")
    time.sleep(3)
    mine(THEIRS)
    same("pending_withdrawal_bundle")
    log(f"  pending: {THEIRS.call('pending_withdrawal_bundle')}")
    step("four more blocks: the bundle submitted, or dropped and its outputs recreated")
    for i in range(5):
        mine(NODES[i % 2])
        log(f"  pending {str(THEIRS.call('pending_withdrawal_bundle'))[:80]}, failed at {THEIRS.call('latest_failed_withdrawal_bundle_height')}")
    unsucc = [u for u in THEIRS.call("list_utxos") if "UnsuccessfulWithdrawal" in u.get("outpoint", {})]
    log(f"  UnsuccessfulWithdrawal outputs: {len(unsucc)}")
    step("coinbase maturity: 50 sidechain blocks")
    start = THEIRS.call("getblockcount")
    while THEIRS.call("getblockcount") < start + 52:
        mine(NODES[THEIRS.call("getblockcount") % 2])
    step("transfers spending the miners' coinbase outputs")
    for s, to in ((THEIRS, STATE["ours_addr"]), (OURS, STATE["theirs_addr"])):
        try:
            log(f"  {s.name} transfer: {s.call('create_transfer', to, 5_000, 500)}")
        except RuntimeError as e:
            log(f"  {s.name} transfer refused: {str(e)[:160]}")
    time.sleep(3)
    mine(OURS)
    mine(THEIRS)
    wallet_same()
    step("mainchain reorg over the last side block's BMM block")
    h = base.ecash("getblockcount")
    base.ecash("invalidateblock", base.ecash("getblockhash", h))
    base.ecash("generatetoaddress", 2, STATE["enf_addr"])
    time.sleep(20)
    base.restart_main()
    for which in ("theirs", "ours"):
        if not base.alive(base.pids()[which]):
            log(f"  {which} exited during the reorg; restarting it")
            base.stop_node(which)
            base.start_node(which)
    time.sleep(10)
    mine(THEIRS)
    mine(OURS)
    step("whole history")
    base.compare_history()
    log(f"RESULT: checks {CHECKS['n']}, diffs {CHECKS['diff']}, immature refusals {STATE.get('immature')}")
    base.down()
    (base.DIR / "result.log").write_text("\n".join(base.LOG) + "\n")
    return CHECKS["diff"] == 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "full"
    if cmd == "down":
        base.down()
    elif cmd == "full":
        try:
            ok = full()
        except Exception:
            (base.DIR / "result.log").write_text("\n".join(base.LOG) + "\n")
            raise
        sys.exit(0 if ok else 1)
