import argparse
import csv
import random

MID = 100_000
HEADER = ["type", "id", "side", "price", "quantity", "order_type", "owner"]


def limit_price(rng, side):
    # Offsets of -2..0 reach the other side of mid, so some limit orders trade on arrival
    offset = rng.randint(-2, 20)
    return MID - offset if side == "buy" else MID + offset


def main():
    parser = argparse.ArgumentParser(description="Write synthetic order flow as CSV.")
    parser.add_argument("output")
    parser.add_argument("--messages", type=int, default=100_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--owners", type=int, default=10,
                        help="owners are drawn from 0..N; 0 means no owner (exempt from STP)")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    # Limit orders added and not yet cancelled. The script cannot see fills, so some
    # cancels and modifies target orders that have already filled; the engine rejects them.
    live = []
    next_id = 1

    # Mix: 50% limit add, 25% cancel, 15% modify, 10% market order
    with open(args.output, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(HEADER)
        for _ in range(args.messages):
            r = rng.randrange(100)
            side = rng.choice(("buy", "sell"))
            owner = rng.randint(0, args.owners)
            quantity = rng.randint(1, 100)
            if r < 50 or not live:
                price = limit_price(rng, side)
                writer.writerow(["add", next_id, side, price, quantity, "limit", owner])
                live.append((next_id, side))
                next_id += 1
            elif r < 75:
                i = rng.randrange(len(live))
                order_id, _ = live[i]
                live[i] = live[-1]
                live.pop()
                writer.writerow(["cancel", order_id, "", "", "", "", ""])
            elif r < 90:
                order_id, order_side = live[rng.randrange(len(live))]
                price = limit_price(rng, order_side)
                writer.writerow(["modify", order_id, "", price, quantity, "", ""])
            else:
                writer.writerow(["add", next_id, side, 0, quantity, "market", owner])
                next_id += 1


if __name__ == "__main__":
    main()
