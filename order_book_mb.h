#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <vector>

#define MKPRIMITIVE(__x) ((typename std::underlying_type<decltype(__x)>::type)__x)

enum class sprice_t : int32_t {};
bool constexpr is_bid(sprice_t const x) { return int32_t(x) >= 0; }

#define MEMORY_DEFS    \
  using __ptr = ptr_t; \
  using size_t__ = typename std::underlying_type<ptr_t>::type;

template <class T, typename ptr_t, size_t SIZE_HINT>
class pool
{
 public:
  MEMORY_DEFS;
  std::vector<T> m_allocated;
  std::vector<ptr_t> m_free;
  pool() { m_allocated.reserve(SIZE_HINT); }
  pool(size_t reserve_size) { m_allocated.reserve(reserve_size); }
  T *get(ptr_t idx) { return &m_allocated[size_t__(idx)]; }
  T &operator[](ptr_t idx) { return m_allocated[size_t__(idx)]; }
  __ptr alloc(void)
  {
    if (m_free.empty()) {
      auto ret = __ptr(m_allocated.size());
      m_allocated.push_back(T());
      return ret;
    } else {
      auto ret = __ptr(m_free.back());
      m_free.pop_back();
      return ret;
    }
  }
  void free(__ptr idx) { m_free.push_back(idx); }
};

class level
{
 public:
  sprice_t m_price;
  qty_t m_qty;
  level(sprice_t __price, qty_t __qty) : m_price(__price), m_qty(__qty) {}
  level() {}
};

enum class book_id_t : uint16_t {};
enum class level_id_t : uint32_t {};
enum class order_id_t : uint32_t {};

typedef struct order {
  qty_t m_qty;
  level_id_t level_idx;
  book_id_t book_idx;
} order_t;

class price_level
{
 public:
  price_level() {}
  price_level(sprice_t __price, level_id_t __ptr)
      : m_price(__price), m_ptr(__ptr)
  {
  }
  sprice_t m_price;
  level_id_t m_ptr;
};

/*
 * Fixed-Capacity Open-Addressing Hash Table for MicroBlaze DDR3 (8 MB)
 * - Capacity: 2^19 = 524,288 slots (handles >358k concurrent live orders)
 * - Key: order_id_t (0 = EMPTY slot; ITCH order_reference_number is always >= 1)
 * - Deletion: Knuth Algorithm R backward-shift deletion (zero tombstones!)
 */
template <class T, size_t CAPACITY_POW2 = (1 << 19)>
class oid_hashtable
{
 public:
  static constexpr size_t CAPACITY = CAPACITY_POW2;
  static constexpr size_t MASK = CAPACITY - 1;

  struct slot_t {
    uint32_t key;  // 0 means empty
    T val;
  };

  slot_t *m_slots;
  size_t m_count;
  size_t m_peak;  // max simultaneous live orders seen (for sizing)

  oid_hashtable() : m_count(0), m_peak(0)
  {
    m_slots = new slot_t[CAPACITY]();  // Zero-initialized (all keys = 0)
  }

  static inline size_t hash_oid(uint32_t k)
  {
    // MurmurHash3 32-bit finalizer mix (fast on MicroBlaze barrel shifter/multiplier)
    k ^= k >> 16;
    k *= 0x85ebca6bU;
    k ^= k >> 13;
    k *= 0xc2b2ae35U;
    k ^= k >> 16;
    return size_t(k) & MASK;
  }

  void reserve(order_id_t const) { /* Fixed 8 MB table already allocated */ }

  // Insert or lookup an order slot (used by add_order)
  T *insert(order_id_t const oid)
  {
    uint32_t const k = uint32_t(oid);
    size_t idx = hash_oid(k);
    while (m_slots[idx].key != 0 && m_slots[idx].key != k) {
      idx = (idx + 1) & MASK;
    }
    if (m_slots[idx].key == 0) {
      // Keep at least one empty slot, otherwise get()/insert() never terminate
      assert(m_count < CAPACITY - 1);
      m_slots[idx].key = k;
      ++m_count;
      if (m_count > m_peak) m_peak = m_count;
    }
    return &m_slots[idx].val;
  }

  // Lookup an existing order slot
  T *get(order_id_t const oid)
  {
    uint32_t const k = uint32_t(oid);
    size_t idx = hash_oid(k);
    while (m_slots[idx].key != k) {
      assert(m_slots[idx].key != 0);  // Must exist in active orders
      idx = (idx + 1) & MASK;
    }
    return &m_slots[idx].val;
  }

  T &operator[](order_id_t const oid) { return *get(oid); }

  // Backward-shift deletion (eliminates tombstones so probe chains stay short)
  void erase(order_id_t const oid)
  {
    uint32_t const k = uint32_t(oid);
    size_t i = hash_oid(k);
    while (m_slots[i].key != k) {
      assert(m_slots[i].key != 0);
      i = (i + 1) & MASK;
    }

    size_t j = i;
    while (true) {
      j = (j + 1) & MASK;
      if (m_slots[j].key == 0) {
        break;
      }
      size_t r = hash_oid(m_slots[j].key);
      // Check if r is cyclically outside (i, j]
      if ((i <= j) ? (r <= i || r > j) : (r <= i && r > j)) {
        m_slots[i] = m_slots[j];
        i = j;
      }
    }
    m_slots[i].key = 0;
    --m_count;
  }
};

bool operator>(price_level a, price_level b)
{
  return int32_t(a.m_price) > int32_t(b.m_price);
}

qty_t operator+(qty_t const a, qty_t const b)
{
  return qty_t(MKPRIMITIVE(a) + MKPRIMITIVE(b));
}

class order_book
{
 public:
  static constexpr size_t MAX_BOOKS = 1 << 14;
  static constexpr size_t NUM_LEVELS = 1 << 16;  // 65,536 levels (~512 KB)
  static order_book *s_books;
  static oid_hashtable<order_t> oid_map;
  using level_vector = pool<level, level_id_t, NUM_LEVELS>;
  using sorted_levels_t = std::vector<price_level>;
  static level_vector s_levels;
  sorted_levels_t m_bids;
  sorted_levels_t m_offers;

  int32_t best_bid() const
  {
    return m_bids.empty() ? 0 : int32_t(m_bids.back().m_price);
  }
  int32_t best_ask() const
  {
    return m_offers.empty() ? 0 : -int32_t(m_offers.back().m_price);
  }
  static uint16_t get_book_idx(order_id_t const oid)
  {
    return uint16_t(oid_map.get(oid)->book_idx);
  }

  static void add_order(order_id_t const oid, book_id_t const book_idx,
                        sprice_t const price, qty_t const qty)
  {
    order_t *order = oid_map.insert(oid);
    order->m_qty = qty;
    order->book_idx = book_idx;
    s_books[size_t(book_idx)].ADD_ORDER(order, price, qty);
  }

  void ADD_ORDER(order_t *order, sprice_t const price, qty_t const qty)
  {
    sorted_levels_t *sorted_levels = is_bid(price) ? &m_bids : &m_offers;
    auto insertion_point = sorted_levels->end();
    bool found = false;
    while (insertion_point-- != sorted_levels->begin()) {
      price_level &curprice = *insertion_point;
      if (curprice.m_price == price) {
        order->level_idx = curprice.m_ptr;
        found = true;
        break;
      } else if (price > curprice.m_price) {
        break;
      }
    }
    if (!found) {
      order->level_idx = s_levels.alloc();
      s_levels[order->level_idx].m_qty = qty_t(0);
      s_levels[order->level_idx].m_price = price;
      price_level const px(price, order->level_idx);
      ++insertion_point;
      sorted_levels->insert(insertion_point, px);
    }
    s_levels[order->level_idx].m_qty = s_levels[order->level_idx].m_qty + qty;
  }

  static void delete_order(order_id_t const oid)
  {
    order_t *order = oid_map.get(oid);
    s_books[size_t(order->book_idx)].DELETE_ORDER(order);
    oid_map.erase(oid);
  }

  static void cancel_order(order_id_t const oid, qty_t const qty)
  {
    order_t *order = oid_map.get(oid);
    order_book *book = &s_books[MKPRIMITIVE(order->book_idx)];
    if (qty == order->m_qty) {
      book->DELETE_ORDER(order);
      oid_map.erase(oid);
    } else {
      book->REDUCE_ORDER(order, qty);
    }
  }

  void REDUCE_ORDER(order_t *order, qty_t const qty)
  {
    auto tmp = MKPRIMITIVE(s_levels[order->level_idx].m_qty);
    tmp -= MKPRIMITIVE(qty);
    s_levels[order->level_idx].m_qty = qty_t(tmp);

    tmp = MKPRIMITIVE(order->m_qty);
    tmp -= MKPRIMITIVE(qty);
    order->m_qty = qty_t(tmp);
  }

  void DELETE_ORDER(order_t *order)
  {
    assert(MKPRIMITIVE(s_levels[order->level_idx].m_qty) >=
           MKPRIMITIVE(order->m_qty));
    auto tmp = MKPRIMITIVE(s_levels[order->level_idx].m_qty);
    tmp -= MKPRIMITIVE(order->m_qty);
    s_levels[order->level_idx].m_qty = qty_t(tmp);
    if (qty_t(0) == s_levels[order->level_idx].m_qty) {
      sprice_t price = s_levels[order->level_idx].m_price;
      sorted_levels_t *sorted_levels = is_bid(price) ? &m_bids : &m_offers;
      auto it = sorted_levels->end();
      while (it-- != sorted_levels->begin()) {
        if (it->m_price == price) {
          sorted_levels->erase(it);
          break;
        }
      }
      s_levels.free(order->level_idx);
    }
  }

  static void execute_order(order_id_t const oid, qty_t const qty)
  {
    order_t *order = oid_map.get(oid);
    order_book *book = &s_books[MKPRIMITIVE(order->book_idx)];
    if (qty == order->m_qty) {
      book->DELETE_ORDER(order);
      oid_map.erase(oid);
    } else {
      book->REDUCE_ORDER(order, qty);
    }
  }

  static void replace_order(order_id_t const old_oid, order_id_t const new_oid,
                            qty_t const new_qty, sprice_t new_price)
  {
    order_t *order = oid_map.get(old_oid);
    book_id_t const book_idx = order->book_idx;
    order_book *book = &s_books[MKPRIMITIVE(book_idx)];
    bool const bid = is_bid(book->s_levels[order->level_idx].m_price);
    book->DELETE_ORDER(order);
    oid_map.erase(old_oid);
    if (!bid) {
      new_price = sprice_t(-1 * MKPRIMITIVE(new_price));
    }
    book->add_order(new_oid, book_idx, new_price, new_qty);
  }
};

oid_hashtable<order_t> order_book::oid_map = oid_hashtable<order_t>();
order_book *order_book::s_books = new order_book[order_book::MAX_BOOKS];
order_book::level_vector order_book::s_levels = order_book::level_vector();
