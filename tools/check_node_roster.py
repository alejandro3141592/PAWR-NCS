#!/usr/bin/env python3
"""Check a node roster CSV for node_id values reused across different rigs.

Each physical peripheral's node_id must be globally unique across every
central/gateway rig, not just unique within its own rig -- the MQTT payload
(common/pawr_protocol.h's sensor_payload) only carries node_id, with no
central_id/rig field, so the GUI and gateway_9151's flash log both key
purely off node_id (see NOTES.md 2026-08-08 "multi-gateway compatibility").
A node_id reused on two different rigs makes the GUI silently interleave
two different people's readings under one node, with no error.

gen_node_slot_table.py already refuses a duplicate (central_id, node_id)
pair (the same node listed twice for the same rig), but does NOT catch the
same node_id appearing under two different central_ids -- that's what this
script checks. Run it against the same input CSV before flashing/deploying
multiple rigs.

Input CSV format: same as gen_node_slot_table.py -- central_id,node_id per
line, no header, '#' comment lines allowed.

Usage:
    python tools/check_node_roster.py tools/node_roster.csv
"""
import argparse
import csv
import sys
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv_path", help="input CSV: central_id,node_id per line, no header")
    args = ap.parse_args()

    rigs_by_node = defaultdict(set)
    with open(args.csv_path, newline="") as f:
        reader = csv.reader(f)
        for lineno, row in enumerate(reader, start=1):
            if not row or row[0].strip().startswith("#"):
                continue
            if len(row) != 2:
                sys.exit(f"{args.csv_path}:{lineno}: expected 'central_id,node_id', got {row!r}")
            try:
                central_id, node_id = int(row[0]), int(row[1])
            except ValueError:
                sys.exit(f"{args.csv_path}:{lineno}: non-integer value in {row!r}")

            rigs_by_node[node_id].add(central_id)

    if not rigs_by_node:
        sys.exit(f"{args.csv_path}: no data rows found")

    collisions = {node_id: rigs for node_id, rigs in rigs_by_node.items() if len(rigs) > 1}

    if collisions:
        print(f"FAIL: {len(collisions)} node_id(s) reused across multiple rigs in {args.csv_path}:",
              file=sys.stderr)
        for node_id in sorted(collisions):
            rigs = sorted(collisions[node_id])
            print(f"  node_id {node_id}: assigned to central_id {rigs} -- pick one, "
                  f"relabel the others", file=sys.stderr)
        sys.exit(1)

    total_nodes = len(rigs_by_node)
    total_rigs = len({c for rigs in rigs_by_node.values() for c in rigs})
    print(f"OK: {total_nodes} node_id(s) across {total_rigs} rig(s), all globally unique")


if __name__ == "__main__":
    main()
