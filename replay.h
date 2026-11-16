#pragma once
/* Platform-independent ITCH replay over an in-memory image.
 *
 * The same code runs on the PC (host_replay.cpp) and on the MicroBlaze
 * (../mb_main.cpp), so the board executes exactly the loop verified on the PC.
 *
 * Input: a sequence of [2-byte big-endian length][ITCH 5.0 message], as
 * written by extract_itch.py. Only the order-book messages (A F E C X D U)
 * update the book; everything else is skipped by its length prefix.
 *
 * Output: for every book message that changes a stock's best bid or ask, a
 * bbo_record is passed to the sink. msg_idx counts book messages only (1-based),
 * matching msg_idx in record_golden_book.py, so the records reproduce
 * reference_best_prices.csv exactly.
 */
#include <stddef.h>
#include <stdint.h>
#include "itch.h"
#include "order_book_mb.h"

/* One best-price change. All fields are 32-bit words so the binary log the
 * board writes to DDR3 can be read on the PC without packing/endian issues
 * (MicroBlaze/AXI and x86 are both little-endian). 24 bytes. */
struct bbo_record {
  uint32_t msg_idx;
  uint32_t locate;
  uint32_t ts_lo;  // timestamp, ns since midnight, low 32 bits
  uint32_t ts_hi;  //                                high 32 bits
  int32_t bid;     // best bid price (ITCH 1/10000 $), 0 if none
  int32_t ask;     // best ask price, 0 if none
};

enum replay_status : uint32_t {
  REPLAY_OK = 0,
  REPLAY_TRUNCATED,    // image ends in the middle of a message
  REPLAY_BAD_LENGTH,   // length prefix does not match the message type
  REPLAY_BAD_LOCATE,   // stock_locate >= order_book::MAX_BOOKS
  REPLAY_OID_TOO_BIG,  // order id does not fit the 31-bit order_id_t
};

struct replay_result {
  replay_status status;
  uint32_t error_offset;  // byte offset of the offending message (if !OK)
  uint32_t total_msgs;    // all messages, including non-book ones
  uint32_t book_msgs;     // A F E C X D U
  uint32_t bbo_changes;
  uint64_t hash;          // FNV-1a 64 over all bbo_records, in order
};

static inline uint64_t fnv1a64(uint64_t h, void const *data, size_t n)
{
  unsigned char const *p = (unsigned char const *)data;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}
static constexpr uint64_t FNV1A64_INIT = 0xcbf29ce484222325ULL;

/* Progress callback interval (book messages). Lets the board accumulate a
 * 32-bit hardware timer without wrapping and print a heartbeat. */
static constexpr uint32_t REPLAY_PROGRESS_INTERVAL = 1u << 16;

static inline sprice_t replay_mksigned(price_t price, BUY_SELL side)
{
  int32_t p = int32_t(MKPRIMITIVE(price));
  return sprice_t(side == BUY_SELL::SELL ? -p : p);
}

/* Sink must provide:
 *   void on_bbo(bbo_record const &r);
 *   void on_progress(uint32_t book_msgs);   // every REPLAY_PROGRESS_INTERVAL
 */
template <class Sink>
replay_result replay_itch(unsigned char const *data, size_t len, Sink &sink)
{
  replay_result res = {REPLAY_OK, 0, 0, 0, 0, FNV1A64_INIT};
  size_t off = 0;

#define REPLAY_FAIL(code)          \
  do {                             \
    res.status = (code);           \
    res.error_offset = uint32_t(off); \
    return res;                    \
  } while (0)

  while (off + 2 <= len) {
    size_t const mlen = (size_t(data[off]) << 8) | data[off + 1];
    if (mlen == 0 || off + 2 + mlen > len) REPLAY_FAIL(REPLAY_TRUNCATED);
    char const *p = (char const *)data + off + 2;
    itch_t const type = itch_t(p[0]);
    ++res.total_msgs;

    uint16_t loc = 0;
    uint64_t ts = 0;
    bool book_msg = true;

    switch (type) {
      case itch_t::ADD_ORDER:
      case itch_t::ADD_ORDER_MPID: {
        if (mlen != (type == itch_t::ADD_ORDER ? netlen<itch_t::ADD_ORDER>
                                               : netlen<itch_t::ADD_ORDER_MPID>))
          REPLAY_FAIL(REPLAY_BAD_LENGTH);
        add_order_t const m = add_order_t::parse(p);  // F is A + MPID suffix
        if (uint64_t(m.oid) >= 0x80000000ULL) REPLAY_FAIL(REPLAY_OID_TOO_BIG);
        loc = m.stock_locate;
        if (loc >= order_book::MAX_BOOKS) REPLAY_FAIL(REPLAY_BAD_LOCATE);
        ts = uint64_t(m.timestamp);
        break;
      }
      case itch_t::EXECUTE_ORDER:
        if (mlen != netlen<itch_t::EXECUTE_ORDER>) REPLAY_FAIL(REPLAY_BAD_LENGTH);
        break;
      case itch_t::EXECUTE_ORDER_WITH_PRICE:
        if (mlen != netlen<itch_t::EXECUTE_ORDER_WITH_PRICE>)
          REPLAY_FAIL(REPLAY_BAD_LENGTH);
        break;
      case itch_t::REDUCE_ORDER:
        if (mlen != netlen<itch_t::REDUCE_ORDER>) REPLAY_FAIL(REPLAY_BAD_LENGTH);
        break;
      case itch_t::DELETE_ORDER:
        if (mlen != netlen<itch_t::DELETE_ORDER>) REPLAY_FAIL(REPLAY_BAD_LENGTH);
        break;
      case itch_t::REPLACE_ORDER:
        if (mlen != netlen<itch_t::REPLACE_ORDER>) REPLAY_FAIL(REPLAY_BAD_LENGTH);
        break;
      default:
        book_msg = false;  // S, R, H, ... : not part of the book
        break;
    }

    if (book_msg) {
      ++res.book_msgs;
      // For E/C/X/D/U the stock comes from the order table, not the message.
      if (type != itch_t::ADD_ORDER && type != itch_t::ADD_ORDER_MPID) {
        oid_t const oid = oid_t(read_eight(p + 11));
        loc = order_book::get_book_idx(order_id_t(uint64_t(oid)));
        ts = read_six(p + 5);
      }
      order_book &book = order_book::s_books[loc];
      int32_t const old_bid = book.best_bid();
      int32_t const old_ask = book.best_ask();

      switch (type) {
        case itch_t::ADD_ORDER:
        case itch_t::ADD_ORDER_MPID: {
          add_order_t const m = add_order_t::parse(p);
          order_book::add_order(order_id_t(uint64_t(m.oid)), book_id_t(loc),
                                replay_mksigned(m.price, m.buy), m.qty);
          break;
        }
        case itch_t::EXECUTE_ORDER:
        case itch_t::EXECUTE_ORDER_WITH_PRICE: {  // C = E + price suffix
          execute_order_t const m = execute_order_t::parse(p);
          order_book::execute_order(order_id_t(uint64_t(m.oid)), m.qty);
          break;
        }
        case itch_t::REDUCE_ORDER: {
          order_reduce_t const m = order_reduce_t::parse(p);
          order_book::cancel_order(order_id_t(uint64_t(m.oid)), m.qty);
          break;
        }
        case itch_t::DELETE_ORDER: {
          order_delete_t const m = order_delete_t::parse(p);
          order_book::delete_order(order_id_t(uint64_t(m.oid)));
          break;
        }
        case itch_t::REPLACE_ORDER: {
          order_replace_t const m = order_replace_t::parse(p);
          if (uint64_t(m.new_order_id) >= 0x80000000ULL)
            REPLAY_FAIL(REPLAY_OID_TOO_BIG);
          // Side is inherited from the old order inside replace_order().
          order_book::replace_order(order_id_t(uint64_t(m.oid)),
                                    order_id_t(uint64_t(m.new_order_id)),
                                    m.new_qty,
                                    replay_mksigned(m.new_price, BUY_SELL::BUY));
          break;
        }
        default:
          break;
      }

      int32_t const new_bid = book.best_bid();
      int32_t const new_ask = book.best_ask();
      if (new_bid != old_bid || new_ask != old_ask) {
        bbo_record const r = {res.book_msgs, loc, uint32_t(ts),
                              uint32_t(ts >> 32), new_bid, new_ask};
        ++res.bbo_changes;
        res.hash = fnv1a64(res.hash, &r, sizeof r);
        sink.on_bbo(r);
      }
      if ((res.book_msgs & (REPLAY_PROGRESS_INTERVAL - 1)) == 0)
        sink.on_progress(res.book_msgs);
    }
    off += 2 + mlen;
  }
  if (off != len) REPLAY_FAIL(REPLAY_TRUNCATED);
#undef REPLAY_FAIL
  return res;
}

static inline char const *replay_status_str(replay_status s)
{
  switch (s) {
    case REPLAY_OK: return "OK";
    case REPLAY_TRUNCATED: return "truncated image";
    case REPLAY_BAD_LENGTH: return "length prefix does not match message type";
    case REPLAY_BAD_LOCATE: return "stock_locate out of range";
    case REPLAY_OID_TOO_BIG: return "order id >= 2^31";
  }
  return "?";
}
