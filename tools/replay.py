import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(
        description="Feed a flow file through matching_engine and write its trades.")
    parser.add_argument("flow")
    parser.add_argument("--engine", default="build/matching_engine")
    parser.add_argument("--trades", help="default: <flow>.trades.csv")
    args = parser.parse_args()

    trades = args.trades or str(Path(args.flow).with_suffix(".trades.csv"))
    command = [args.engine, "--input", args.flow, "--trades", trades]
    print("$", " ".join(command), flush=True)
    try:
        result = subprocess.run(command)
    except FileNotFoundError:
        sys.exit(f"engine not found: {args.engine} (build it, or pass --engine)")
    # 1: threaded run differed from the engine's single-threaded replay; 2: bad input
    if result.returncode == 0:
        print(f"Trades written to {trades}")
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
