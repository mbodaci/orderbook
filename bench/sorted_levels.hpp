// Comparison implementation kept for reference, not used in production.
// The sorted-vector price-level container measured against std::map in
// results/benchmarks.md. To rerun the comparison, swap it in for the two maps in
// order_book.hpp: SortedLevels<std::greater<Price>> bids_, SortedLevels<std::less<Price>> asks_.

#pragma once

#include <algorithm>
#include <cassert>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include "orderbook/order.hpp"
#include "orderbook/price_level.hpp"

namespace orderbook {

// Vector moves PriceLevels when it shifts or grows. Handles stay valid only because
// moving a std::list keeps its nodes; a copy would allocate new ones. If the move could
// throw, vector would copy instead, so this must hold.
static_assert(std::is_nothrow_move_constructible_v<PriceLevel>);

// Drop-in for the subset of std::map the book uses. Compare orders best-first, as the
// map's comparator does. Levels are stored worst-to-best so the best level is at the
// back, where inserts and erases shift the fewest elements; iterators are reversed so
// begin() is still the best level.
template <typename Compare>
class SortedLevels {
public:
    using value_type = std::pair<Price, PriceLevel>;
    using iterator = typename std::vector<value_type>::reverse_iterator;
    using const_iterator = typename std::vector<value_type>::const_reverse_iterator;

    iterator begin() { return levels_.rbegin(); }
    iterator end() { return levels_.rend(); }
    const_iterator begin() const { return levels_.rbegin(); }
    const_iterator end() const { return levels_.rend(); }
    bool empty() const { return levels_.empty(); }

    iterator find(Price price) {
        auto it = search(price);
        return it != end() && it->first == price ? it : end();
    }

    PriceLevel& at(Price price) {
        auto it = find(price);
        assert(it != end());
        return it->second;
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(Price price, Args&&... args) {
        auto it = search(price);
        if (it != end() && it->first == price) {
            return {it, false};
        }
        // A reverse iterator's base() is the forward position just after the element it
        // points at, which is where the new, better-priced level belongs
        auto inserted =
            levels_.emplace(it.base(), price, PriceLevel(std::forward<Args>(args)...));
        return {iterator(std::next(inserted)), true};
    }

    void erase(iterator it) { levels_.erase(std::next(it).base()); }

private:
    // First level, best-first, that is not better than price
    iterator search(Price price) {
        return std::lower_bound(begin(), end(), price, [](const value_type& level, Price p) {
            return Compare{}(level.first, p);
        });
    }

    std::vector<value_type> levels_;
};

}  // namespace orderbook
