#!/usr/bin/env python3
"""Side-by-side regtest through a mainchain reorg: THEIR thunder-rust v0.18.2 (244e20f,
thunder_app) and OUR thunder (backbone-thunder) on one eCash regtest chain (ecashd + electrs +
bip300301_enforcer), peered only with each other over QUIC. Blocks are mined alternately by
each node (BMM through the enforcer); after every block both nodes' JSON-RPC replies are
compared byte for byte. Then the mainchain block(s) holding the BMM commitments of the last two
sidechain blocks are reorged away: both nodes must keep those sidechain blocks (their side-tips
rule), and mining must go on identically on both.

    thunder_reorg_regtest.py full   # up, blocks, reorg, more blocks, whole history, down
    thunder_reorg_regtest.py down   # stop exactly the recorded PIDs

Local only. Binaries: $SKY/ext/... (see BIN). Ports TH_BASE.. TH_BASE+19 (default 49560).
Datadirs under TH_DIR (default: <tmp>/backbone-thunder-regtest).
"""
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

SKY = Path(os.environ.get("SKY", Path.home() / "skydoge"))
sys.path.insert(0, str(SKY / "tools/linked-regtest"))
import connect  # noqa: E402  (grpc-web JSON helper)

DIR = Path(os.environ.get("TH_DIR", Path(tempfile.gettempdir()) / "backbone-thunder-regtest"))
BASE = int(os.environ.get("TH_BASE", "49560"))
P = {"ecash_rpc": BASE, "ecash_p2p": BASE + 1, "ecash_zmq": BASE + 3, "electrum": BASE + 5, "esplora": BASE + 6,
     "electrs_monitor": BASE + 7, "enf_grpc": BASE + 10, "enf_rpc": BASE + 11,
     "their_p2p": BASE + 13, "their_rpc": BASE + 14, "our_p2p": BASE + 16, "our_rpc": BASE + 17}
REPO = Path(__file__).resolve().parents[2]
BIN = {
    "ecashd": SKY / "ext/ecash-bitcoin/build/bin/bitcoind",
    "electrs": SKY / "ext/electrs/target/release/electrs",
    "enforcer": SKY / "ext/enforcer/target/release/bip300301_enforcer",
    "theirs": SKY / "ext/thunder-rust/target/release/thunder_app",
    "ours": Path(os.environ.get("TH_OURS", REPO / "build/chains/thunder/thunder")),
}
SLOT = 9
USER = PASS = "th"
MAGIC = "eca5b134"  # eCash regtest
# BIP-39 test vectors (regtest only)
MNEMONIC = {"theirs": "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
            "ours": "legal winner thank year wave sausage worth useful legal winner thank yellow"}
LOG = []
STATE = {}
CHECKS = {"n": 0, "diff": 0}


def log(*a):
    s = " ".join(str(x) for x in a)
    print(s, flush=True)
    LOG.append(s)


def spawn(name, argv):
    (DIR / "logs").mkdir(parents=True, exist_ok=True)
    out = open(DIR / "logs" / f"{name}.out", "ab")
    p = subprocess.Popen([str(a) for a in argv], stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                         start_new_session=True)
    with open(DIR / "pids", "a") as f:
        f.write(f"{name} {p.pid}\n")
    return p.pid


def pids():
    f = DIR / "pids"
    if not f.exists():
        return {}
    r = {}
    for line in f.read_text().splitlines():
        if line.strip():
            n, p = line.split()
            r[n] = int(p)
    return r


def alive(pid):
    try:
        os.kill(pid, 0)
        return open(f"/proc/{pid}/stat").read().split(") ")[-1].split()[0] != "Z"
    except OSError:
        return False


def wait_for(f, what, timeout=120, step=0.5):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            r = f()
            if r:
                return r
        except Exception as e:  # noqa: BLE001
            last = e
        time.sleep(step)
    raise RuntimeError(f"timeout waiting for {what} ({last})")


def ecash(method, *params):
    body = json.dumps({"jsonrpc": "1.0", "id": 1, "method": method, "params": list(params)}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{P['ecash_rpc']}", body,
                                 {"Authorization": "Basic " + base64.b64encode(f"{USER}:{PASS}".encode()).decode()})
    try:
        r = json.loads(urllib.request.urlopen(req, timeout=120).read())
    except urllib.error.HTTPError as e:
        r = json.loads(e.read())
    if r.get("error"):
        raise RuntimeError(f"{method}: {r['error']}")
    return r["result"]


def grpc(method, req=None, timeout=120):
    return connect.call(f"127.0.0.1:{P['enf_grpc']}", method, req or {}, timeout=timeout)


class Side:
    """jsonrpsee JSON-RPC of one sidechain node; raw() keeps the reply bytes."""

    def __init__(self, name, port):
        self.name, self.port = name, port

    def raw(self, method, params=None, timeout=600):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params if params is not None else []}).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}", body, {"Content-Type": "application/json"})
        try:
            return urllib.request.urlopen(req, timeout=timeout).read()
        except urllib.error.HTTPError as e:
            return e.read()

    def call(self, method, *params, timeout=600):
        r = json.loads(self.raw(method, list(params), timeout))
        if "error" in r:
            raise RuntimeError(f"{self.name} {method}: {r['error']}")
        return r["result"]


THEIRS = Side("theirs", P["their_rpc"])
OURS = Side("ours", P["our_rpc"])
NODES = [THEIRS, OURS]


def start_node(which):
    d = DIR / which
    seedf = DIR / f"{which}.mnemonic"
    seedf.write_text(MNEMONIC[which])
    p2p, rpc, peer = (P["their_p2p"], P["their_rpc"], P["our_p2p"]) if which == "theirs" else (P["our_p2p"], P["our_rpc"], P["their_p2p"])
    spawn(which, [BIN[which], "--headless", "--datadir", d, "--network", "regtest",
                  "--mainchain-grpc-url", f"http://127.0.0.1:{P['enf_grpc']}", "--net-addr", f"127.0.0.1:{p2p}",
                  "--rpc-addr", f"127.0.0.1:{rpc}", "--private-rpc-addr", f"127.0.0.1:{rpc}", "--add-peer", f"127.0.0.1:{peer}",
                  "--mnemonic-seed-phrase-path", seedf, "--log-level", "info", "--log-level-file", "debug"])
    s = THEIRS if which == "theirs" else OURS
    wait_for(lambda: s.call("getblockcount") is not None, f"{which} RPC", 180, 1)


def stop_pid(name, pid, grace=30):
    end = time.time() + grace
    while alive(pid) and time.time() < end:
        time.sleep(0.5)
    if alive(pid):
        os.kill(pid, 15)
        end = time.time() + 30
        while alive(pid) and time.time() < end:
            time.sleep(0.5)
    if alive(pid):
        os.kill(pid, 9)


def stop_node(which):
    s = THEIRS if which == "theirs" else OURS
    pid = pids().get(which)
    try:
        s.call("stop", timeout=10)
    except Exception:  # noqa: BLE001
        pass
    if pid:
        stop_pid(which, pid)
    lines = [ln for ln in (DIR / "pids").read_text().splitlines() if not ln.startswith(which + " ")]
    (DIR / "pids").write_text("\n".join(lines) + "\n")


def start_electrs():
    ed, xd = DIR / "ecash", DIR / "electrs"
    shutil.rmtree(xd, ignore_errors=True)
    xd.mkdir()
    spawn("electrs", [BIN["electrs"], "-v", f"--db-dir={xd}", f"--daemon-dir={ed}", f"--daemon-rpc-addr=127.0.0.1:{P['ecash_rpc']}",
                      f"--electrum-rpc-addr=127.0.0.1:{P['electrum']}", f"--http-addr=127.0.0.1:{P['esplora']}",
                      f"--monitoring-addr=127.0.0.1:{P['electrs_monitor']}", "--network=regtest", f"--cookie={USER}:{PASS}",
                      "--jsonrpc-import"])
    wait_for(lambda: urllib.request.urlopen(f"http://127.0.0.1:{P['esplora']}/blocks/tip/height", timeout=5).read(), "electrs", 120)


def start_enforcer():
    nd = DIR / "enforcer"
    nd.mkdir(exist_ok=True)
    spawn("enforcer", [BIN["enforcer"], f"--data-dir={nd}", f"--node-rpc-addr=127.0.0.1:{P['ecash_rpc']}",
                       f"--node-rpc-user={USER}", f"--node-rpc-pass={PASS}", f"--node-zmq-addr-sequence=tcp://127.0.0.1:{P['ecash_zmq']}",
                       f"--network-magic={MAGIC}", "--op-drivechain=nop8", "--enable-wallet", "--wallet-auto-create",
                       "--enable-mempool", "--enable-block-template-server", f"--wallet-esplora-url=http://127.0.0.1:{P['esplora']}",
                       f"--serve-grpc-addr=127.0.0.1:{P['enf_grpc']}", f"--serve-rpc-addr=127.0.0.1:{P['enf_rpc']}",
                       f"--log-directory={nd / 'logs'}", "--log-level=info"])
    wait_for(lambda: grpc("ValidatorService/GetChainTip"), "enforcer gRPC", 180)


def restart_main():
    """electrs (fresh index) and the enforcer again (both are restarted after the reorg)."""
    ps = pids()
    for n in ("enforcer", "electrs"):
        if n in ps and alive(ps[n]):
            os.kill(ps[n], 15)
            stop_pid(n, ps[n], 0)
    start_electrs()
    start_enforcer()
    wait_for(lambda: grpc("ValidatorService/GetChainTip")["blockHeaderInfo"]["height"] == ecash("getblockcount"), "enforcer sync", 300)


def enforcer_mine(n=1):
    for _ in range(n):
        grpc("MiningService/GenerateToAddress", {"address": STATE["enf_addr"], "blocks": 1}, timeout=300)


def up():
    if any(alive(p) for p in pids().values()):
        sys.exit("already running")
    shutil.rmtree(DIR, ignore_errors=True)
    DIR.mkdir(parents=True)
    busy = []
    for k, v in P.items():
        with socket.socket() as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(("127.0.0.1", v))
            except OSError:
                busy.append((k, v))
    if busy:
        sys.exit(f"ports busy: {busy}")
    ed = DIR / "ecash"
    ed.mkdir()
    spawn("ecashd", [BIN["ecashd"], "-regtest", f"-datadir={ed}", f"-rpcport={P['ecash_rpc']}", f"-port={P['ecash_p2p']}",
                     f"-bind=127.0.0.1:{P['ecash_p2p']}", "-rest", f"-zmqpubsequence=tcp://127.0.0.1:{P['ecash_zmq']}",
                     "-disablewallet", f"-rpcuser={USER}", f"-rpcpassword={PASS}", "-server", "-dnsseed=0",
                     "-fixedseeds=0", "-discover=0", "-printtoconsole=0", "-txindex"])
    wait_for(lambda: ecash("getblockcount") >= 0, "ecashd", 120)
    ecash("generatetoaddress", 1, "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080")
    start_electrs()
    start_enforcer()
    wait_for(lambda: grpc("ValidatorService/GetChainTip")["blockHeaderInfo"]["height"] == 1, "enforcer sync", 180)
    STATE["enf_addr"] = wait_for(lambda: grpc("WalletService/CreateNewAddress")["address"], "enforcer wallet", 120)
    enforcer_mine(101)
    grpc("BlockProducerService/SetAckAllProposals", {"policy": "ACK_ALL_PROPOSALS_POLICY_ALL"})
    grpc("BlockProducerService/SubmitSidechainProposal", {
        "sidechainId": SLOT, "declaration": {"v0": {"title": "Thunder", "description": "Thunder drop-in regtest",
                                                    "hashId1": {"hex": "11" * 32}, "hashId2": {"hex": "22" * 20}}}})

    def active():
        enforcer_mine(1)
        return any(s.get("sidechainNumber") == SLOT for s in grpc("ValidatorService/GetSidechains").get("sidechains", []))
    wait_for(active, f"slot {SLOT} active", 600, 0.1)
    log(f"slot {SLOT} active at mainchain height", ecash("getblockcount"))
    start_node("theirs")
    start_node("ours")
    for s in NODES:
        STATE[f"{s.name}_addr"] = s.call("get_new_address")
    log("nodes up:", STATE)


def down():
    order = ["theirs", "ours", "enforcer", "electrs", "ecashd"]
    ps = pids()
    for n in order + [k for k in ps if k not in order]:
        if n in ps and alive(ps[n]):
            if n in ("theirs", "ours"):
                stop_node(n)
                continue
            if n == "ecashd":
                try:
                    ecash("stop")
                except Exception:  # noqa: BLE001
                    os.kill(ps[n], 15)
            else:
                os.kill(ps[n], 15)
            stop_pid(n, ps[n], 60)
    print("stopped; alive:", [n for n, p in pids().items() if alive(p)])


def mine_with(s):
    """`mine` sends the BMM request, then waits for the next mainchain block."""
    res = {}

    def run():
        try:
            res["r"] = s.call("mine", None, timeout=600)
        except Exception as e:  # noqa: BLE001
            res["e"] = e
    t = threading.Thread(target=run)
    t.start()
    time.sleep(3)
    try:
        enforcer_mine(1)
    except Exception as e:  # noqa: BLE001
        log(f"  enforcer GenerateToAddress failed ({str(e)[:80]}); mining with ecashd")
        ecash("generatetoaddress", 1, STATE["enf_addr"])
    t.join(300)
    if "e" in res:
        raise res["e"]


READS = ["getblockcount", "get_best_sidechain_block_hash", "get_best_mainchain_block_hash", "list_utxos",
         "pending_withdrawal_bundle", "latest_failed_withdrawal_bundle_height", "sidechain_wealth"]


def canon(method, raw):
    j = json.loads(raw)
    if method in ("list_utxos", "get_wallet_utxos"):
        r = j.get("result")
        if isinstance(r, list):
            j["result"] = sorted(r, key=lambda x: json.dumps(x, sort_keys=True))
            return json.dumps(j, sort_keys=True).encode()
    return raw


def same(method, params=None, sets=False):
    a, b = THEIRS.raw(method, params), OURS.raw(method, params)
    if sets:
        a, b = canon(method, a), canon(method, b)
    CHECKS["n"] += 1
    if a != b:
        CHECKS["diff"] += 1
        log(f"DIFF {method} {params}:\n  theirs {a[:600]}\n  ours   {b[:600]}")
        return False
    return True


def sync_tips(timeout=180):
    return wait_for(lambda: THEIRS.call("get_best_sidechain_block_hash") == OURS.call("get_best_sidechain_block_hash")
                    and THEIRS.call("get_best_sidechain_block_hash"), "same sidechain tip", timeout)


def compare_all():
    tip = sync_tips()
    for m in READS:
        same(m, sets=m == "list_utxos")
    h = THEIRS.call("getblockcount")
    for height in range(max(0, h - 3), h):
        bh = THEIRS.call("get_block_hash", height)
        same("get_block_hash", [height])
        same("get_block", [bh])
        same("get_bmm_inclusions", [bh])
    return tip


def compare_history():
    sync_tips()
    h = THEIRS.call("getblockcount")
    for height in range(h):
        bh = THEIRS.call("get_block_hash", height)
        same("get_block_hash", [height])
        same("get_block", [bh])
        same("get_bmm_inclusions", [bh])
    log(f"history compared: {h} blocks, checks {CHECKS['n']}, diffs {CHECKS['diff']}")


def state_line():
    r = []
    for s in NODES:
        try:
            r.append((s.name, s.call("getblockcount"), s.call("get_best_sidechain_block_hash")[:16]))
        except Exception as e:  # noqa: BLE001
            r.append((s.name, f"error: {str(e)[:60]}"))
    return r


def mine_and_compare(s):
    mine_with(s)
    tip = compare_all()
    log(f"  block by {s.name}: height {THEIRS.call('getblockcount')}, tip {tip[:16]}, checks {CHECKS['n']}, diffs {CHECKS['diff']}")


def full():
    up()
    log("== blocks, alternately mined")
    for i in range(6):
        mine_and_compare(NODES[i % 2])
    log("== mainchain reorg over the last two side blocks' BMM blocks")
    before = state_line()
    h_side = THEIRS.call("getblockcount")
    last_two = [THEIRS.call("get_block_hash", h_side - 2), THEIRS.call("get_block_hash", h_side - 1)]
    log(f"  before: {before}")
    h = ecash("getblockcount")
    ecash("invalidateblock", ecash("getblockhash", h - 1))
    ecash("generatetoaddress", 3, STATE["enf_addr"])
    time.sleep(20)
    after = state_line()
    log(f"  after the reorg (mainchain {ecash('getblockcount')}): {after}")
    for n, pid in pids().items():
        if n in ("theirs", "ours", "enforcer", "electrs"):
            log(f"    {n} alive: {alive(pid)}")
    STATE["kept"] = {s.name: s.call("getblockcount") == h_side and s.call("get_best_sidechain_block_hash") == last_two[1]
                     for s in NODES if alive(pids()[s.name])}
    log(f"  side blocks kept after the reorg (height {h_side}, tip {last_two[1][:16]}): {STATE['kept']}")
    restart_main()
    for which in ("theirs", "ours"):
        if not alive(pids()[which]):
            log(f"  {which} exited during the reorg; restarting it")
            stop_node(which)
            start_node(which)
    time.sleep(10)
    log(f"  after restarting electrs and the enforcer: {state_line()}")
    log("== blocks after the reorg, alternately mined")
    for i in range(4):
        mine_and_compare(NODES[i % 2])
    compare_history()
    log(f"checks {CHECKS['n']}, diffs {CHECKS['diff']}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "help"
    if cmd == "full":
        try:
            full()
        finally:
            (DIR / "full.log").write_text("\n".join(LOG))
            down()
    elif cmd == "down":
        down()
    else:
        print(__doc__)
