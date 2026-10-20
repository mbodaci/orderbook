import argparse
import bisect
import csv
import sys
from collections import Counter, deque
from pathlib import Path

FLOW_HEADER = ["type", "id", "side", "price", "quantity", "order_type", "owner"]
TRADES_HEADER = ["message", "aggressor_id", "passive_id", "price", "quantity"]
MAX_REPORTED = 10

# Indices into a resting-order entry in the reference matcher
ID, SIDE, PRICE, REMAINING, OWNER = range(5)


def to_int(field):
    return int(field) if field != "" else None


def read_csv(path, header):
    with open(path, newline="") as f:
        reader = csv.reader(f)
        if next(reader, None) != header:
            sys.exit(f"{path}: expected header {','.join(header)}")
        return list(reader)


def read_flow(path):
    messages = []
    for kind, order_id, side, price, quantity, order_type, owner in read_csv(path, FLOW_HEADER):
        messages.append({"type": kind, "id": int(order_id), "side": side,
                         "price": to_int(price), "quantity": to_int(quantity),
                         "order_type": order_type, "owner": to_int(owner)})
    return messages


def read_trades(path):
    return [tuple(int(field) for field in row) for row in read_csv(path, TRADES_HEADER)]


# Properties every trade must have, checked without re-running the matching logic.
# Returns a list of error messages.
def check_invariants(flow, trades):
    errors = []
    orders = {}        # id -> {"side", "price", "owner", "remaining"} as last added or modified
    cancelled = set()
    by_message = {}
    previous = -1
    for trade in trades:
        if trade[0] < previous:
            errors.append(f"trades out of message order at message {trade[0]}")
        previous = trade[0]
        by_message.setdefault(trade[0], []).append(trade)

    for index, msg in enumerate(flow):
        fills = by_message.get(index, [])
        if msg["type"] == "cancel":
            if fills:
                errors.append(f"message {index}: a cancel produced trades")
            cancelled.add(msg["id"])
            continue

        if msg["type"] == "add":
            side, owner = msg["side"], msg["owner"]
            limit = msg["price"] if msg["order_type"] == "limit" else None
        elif msg["id"] in orders:
            side, owner = orders[msg["id"]]["side"], orders[msg["id"]]["owner"]
            limit = msg["price"]
        else:
            side = owner = limit = None
            if fills:
                errors.append(f"message {index}: modify of unknown order {msg['id']} traded")

        filled = 0
        for _, aggressor, passive, price, quantity in fills:
            where = f"message {index}, passive {passive}"
            if aggressor != msg["id"]:
                errors.append(f"{where}: aggressor {aggressor} is not the order being processed")
            resting = orders.get(passive)
            if resting is None:
                errors.append(f"{where}: passive order was never added")
                continue
            if passive in cancelled:
                errors.append(f"{where}: passive order traded after it was cancelled")
            if resting["side"] == side:
                errors.append(f"{where}: both sides are {side}")
            if price != resting["price"]:
                errors.append(f"{where}: price {price} is not the resting price "
                              f"{resting['price']}")
            if limit is not None and (price > limit if side == "buy" else price < limit):
                errors.append(f"{where}: price {price} is through the aggressor's limit {limit}")
            if owner and owner == resting["owner"]:
                errors.append(f"{where}: self-trade, both owned by {owner}")
            if quantity > resting["remaining"]:
                errors.append(f"{where}: fill of {quantity} exceeds remaining "
                              f"{resting['remaining']}")
            resting["remaining"] -= quantity
            filled += quantity
        if filled > msg["quantity"]:
            errors.append(f"message {index}: filled {filled}, more than the order's "
                          f"{msg['quantity']}")

        if msg["type"] == "add" and msg["order_type"] == "limit":
            orders[msg["id"]] = {"side": side, "price": msg["price"], "owner": owner,
                                 "remaining": msg["quantity"] - filled}
        elif msg["type"] == "modify" and msg["id"] in orders:
            orders[msg["id"]]["price"] = msg["price"]
            orders[msg["id"]]["remaining"] = msg["quantity"] - filled
    return errors


# Independent implementation of the engine's matching rules, kept deliberately plain.
# Cancelled entries are marked dead (remaining = 0) and skipped, instead of being removed
# from the middle of a level's deque.
class ReferenceBook:
    def __init__(self):
        self.levels = {"buy": {}, "sell": {}}   # price -> deque of entries, oldest first
        self.prices = {"buy": [], "sell": []}   # sorted ascending
        self.orders = {}                        # id -> entry, resting orders only
        self.trades = []

    def best_level(self, side):
        prices = self.prices[side]
        while prices:
            price = prices[-1] if side == "buy" else prices[0]
            level = self.levels[side][price]
            while level and level[0][REMAINING] == 0:
                level.popleft()
            if level:
                return price, level
            del self.levels[side][price]
            prices.pop(-1 if side == "buy" else 0)
        return None

    def match(self, index, order_id, side, limit, quantity, owner):
        opposite = "sell" if side == "buy" else "buy"
        while quantity > 0:
            best = self.best_level(opposite)
            if best is None:
                break
            price, level = best
            if limit is not None and (price > limit if side == "buy" else price < limit):
                break
            while quantity > 0 and level:
                resting = level[0]
                if resting[REMAINING] == 0:
                    level.popleft()
                    continue
                # Self-trade prevention: cancel the incoming remainder, keep the resting order
                if owner != 0 and owner == resting[OWNER]:
                    return 0
                fill = min(quantity, resting[REMAINING])
                self.trades.append((index, order_id, resting[ID], price, fill))
                quantity -= fill
                resting[REMAINING] -= fill
                if resting[REMAINING] == 0:
                    level.popleft()
                    del self.orders[resting[ID]]
        return quantity

    def add(self, index, order_id, side, price, quantity, order_type, owner):
        if quantity == 0 or order_id in self.orders:
            return
        limit = price if order_type == "limit" else None
        remaining = self.match(index, order_id, side, limit, quantity, owner)
        if remaining > 0 and order_type == "limit":
            if price not in self.levels[side]:
                self.levels[side][price] = deque()
                bisect.insort(self.prices[side], price)
            entry = [order_id, side, price, remaining, owner]
            self.levels[side][price].append(entry)
            self.orders[order_id] = entry

    def cancel(self, order_id):
        entry = self.orders.pop(order_id, None)
        if entry is not None:
            entry[REMAINING] = 0

    def modify(self, index, order_id, price, quantity):
        entry = self.orders.get(order_id)
        if entry is None or quantity == 0:
            return
        # Same price, smaller or equal quantity: reduce in place and keep time priority
        if price == entry[PRICE] and quantity <= entry[REMAINING]:
            entry[REMAINING] = quantity
            return
        self.cancel(order_id)
        self.add(index, order_id, entry[SIDE], price, quantity, "limit", entry[OWNER])


def reference_trades(flow):
    book = ReferenceBook()
    for index, msg in enumerate(flow):
        if msg["type"] == "add":
            book.add(index, msg["id"], msg["side"], msg["price"], msg["quantity"],
                     msg["order_type"], msg["owner"])
        elif msg["type"] == "cancel":
            book.cancel(msg["id"])
        else:
            book.modify(index, msg["id"], msg["price"], msg["quantity"])
    return book.trades


def compare(engine, reference):
    for position, (got, expected) in enumerate(zip(engine, reference)):
        if got != expected:
            return (f"trade {position} differs: engine {got}, reference {expected} "
                    "(message, aggressor, passive, price, quantity)")
    if len(engine) != len(reference):
        return f"engine produced {len(engine)} trades, reference {len(reference)}"
    return None


def chart(trades, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    volume = Counter()
    for _, _, _, price, quantity in trades:
        volume[price] += quantity
    prices = sorted(volume)

    fig, (top, bottom) = plt.subplots(2, 1, figsize=(10, 8))
    top.scatter([t[0] for t in trades], [t[3] for t in trades], s=1)
    top.set(title="Trade price over the flow", xlabel="message index",
            ylabel="price (ticks)")
    bottom.bar(prices, [volume[p] for p in prices], width=1.0)
    bottom.set(title="Traded volume by price", xlabel="price (ticks)", ylabel="quantity")
    # Show prices as written, not as an offset from 1e5
    top.ticklabel_format(axis="y", useOffset=False, style="plain")
    bottom.ticklabel_format(axis="x", useOffset=False, style="plain")
    fig.tight_layout()
    fig.savefig(path, dpi=120)


def main():
    parser = argparse.ArgumentParser(
        description="Check matching_engine's trades against a flow file, then chart them.")
    parser.add_argument("flow")
    parser.add_argument("trades")
    parser.add_argument("--chart", help="output PNG; default: <trades>.png")
    parser.add_argument("--no-chart", action="store_true")
    args = parser.parse_args()

    flow = read_flow(args.flow)
    trades = read_trades(args.trades)
    volume = sum(t[4] for t in trades)
    print(f"Messages: {len(flow)}  Trades: {len(trades)}  Volume: {volume}")

    errors = check_invariants(flow, trades)
    print(f"Invariant checks: {'passed' if not errors else f'{len(errors)} failures'}")
    for error in errors[:MAX_REPORTED]:
        print(f"  {error}")

    mismatch = compare(trades, reference_trades(flow))
    print(f"Reference matcher: {'identical trades' if mismatch is None else 'MISMATCH'}")
    if mismatch is not None:
        print(f"  {mismatch}")

    if not args.no_chart:
        path = args.chart or str(Path(args.trades).with_suffix(".png"))
        chart(trades, path)
        print(f"Chart written to {path}")

    sys.exit(1 if errors or mismatch is not None else 0)


if __name__ == "__main__":
    main()
