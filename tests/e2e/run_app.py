#!/usr/bin/env python3
# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license.
"""End-to-end run of thunder-app on a local eCash regtest (doc/app.md "Tests"): slot 9, app.js in its page; the runner
is core/tests/e2e/dropin_app.py.

    run_app.py --bin build-app/chains/thunder --backboned <backboned> --tmp <dir> --shots <dir> [--base-port 49800]
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "core" / "tests" / "e2e"))
from dropin_app import Chain, main  # noqa: E402

READS = [("getblockcount", []), ("get_best_sidechain_block_hash", []), ("get_best_mainchain_block_hash", []), ("list_utxos", []),
         ("pending_withdrawal_bundle", []), ("latest_failed_withdrawal_bundle_height", []), ("sidechain_wealth", []), ("balance", []),
         ("get_wallet_addresses", []), ("get_wallet_utxos", []), ("openapi_schema", []), ("list_peers", [])]

if __name__ == "__main__":
    sys.exit(main(Chain(stem="thunder", title="Thunder", slot=9, script=HERE / "app.js", reads=READS)))
