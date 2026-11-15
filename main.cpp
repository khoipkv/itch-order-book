#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include "bufferedreader.h"
#include "itch.h"
#include "order_book.h"

template <itch_t __code>
class PROCESS
{
 public:
  static itch_message<__code> read_from(buf_t *__buf)
  {
    uint16_t const msglen = be16toh(*(uint16_t *)__buf->get(0));
    __buf->advance(2);
    assert(msglen == netlen<__code>);

    __buf->ensure(netlen<__code>);
    itch_message<__code> ret = itch_message<__code>::parse(__buf->get(0));
    __buf->advance(netlen<__code>);
    return ret;
  }
};

static sprice_t mksigned(price_t price, BUY_SELL buy)
{
  assert(MKPRIMITIVE(price) < std::numeric_limits<int32_t>::max());
  auto ret = MKPRIMITIVE(price);
  if (BUY_SELL::SELL == buy) ret = -ret;
  return sprice_t(ret);
}
#define DO_CASE(__itch_t)               \
  case (__itch_t): {                    \
    PROCESS<__itch_t>::read_from(&buf); \
    break;                              \
  }

static inline void log_if_bbo_changed(FILE *fp, size_t npkts, uint64_t ts_ns,
                                      uint16_t locate, int32_t old_bid,
                                      int32_t old_ask, size_t &bbo_changes)
{
  int32_t new_bid = order_book::s_books[locate].best_bid();
  int32_t new_ask = order_book::s_books[locate].best_ask();
  if (new_bid != old_bid || new_ask != old_ask) {
    ++bbo_changes;
    fprintf(fp, "%lu,%lu,%u,%d,%d\n", npkts, ts_ns, locate, new_bid, new_ask);
  }
}

int main(int argc, char **argv)
{
  const char *csv_path = (argc > 1) ? argv[1] : "reference_best_prices.csv";
  FILE *ref_fp = fopen(csv_path, "w");
  if (!ref_fp) {
    perror("Failed to open output CSV");
    return 1;
  }
  fprintf(ref_fp, "pkt_num,timestamp_ns,stock_locate,best_bid_price,best_ask_price\n");

  buf_t buf(1024);
  buf.fd = STDIN_FILENO;
  std::chrono::steady_clock::time_point start;
  size_t npkts = 0;
  size_t bbo_changes = 0;

  order_book::oid_map.reserve(order_id_t(184118975 * 2));  // the first number
                                                           // is the empirically
                                                           // largest oid seen.
                                                           // multiply by 2 for
                                                           // good measure
  printf("Allocated books bytes: %lu\n", sizeof(order_book) * order_book::MAX_BOOKS);

  while (is_ok(buf.ensure(3))) {
    if (npkts) ++npkts;
    itch_t const msgtype = itch_t(*buf.get(2));
    switch (msgtype) {
      DO_CASE(itch_t::SYSEVENT);
      DO_CASE(itch_t::STOCK_DIRECTORY);
      DO_CASE(itch_t::TRADING_ACTION);
      DO_CASE(itch_t::REG_SHO_RESTRICT);
      DO_CASE(itch_t::MPID_POSITION);
      DO_CASE(itch_t::MWCB_DECLINE);
      DO_CASE(itch_t::MWCB_STATUS);
      DO_CASE(itch_t::IPO_QUOTE_UPDATE);
      DO_CASE(itch_t::TRADE);
      DO_CASE(itch_t::CROSS_TRADE);
      DO_CASE(itch_t::BROKEN_TRADE);
      DO_CASE(itch_t::NET_ORDER_IMBALANCE);
      DO_CASE(itch_t::RETAIL_PRICE_IMPROVEMENT);
      DO_CASE(itch_t::PROCESS_LULD_AUCTION_COLLAR_MESSAGE);

      case (itch_t::ADD_ORDER): {
        if (!npkts) {
          start = std::chrono::steady_clock::now();
          ++npkts;
        }
        auto const pkt = PROCESS<itch_t::ADD_ORDER>::read_from(&buf);
        assert(uint64_t(pkt.oid) <
               uint64_t(std::numeric_limits<int32_t>::max()));
        uint16_t loc = pkt.stock_locate;
        int32_t old_bid = order_book::s_books[loc].best_bid();
        int32_t old_ask = order_book::s_books[loc].best_ask();
        order_book::add_order(order_id_t(pkt.oid), book_id_t(loc),
                              mksigned(pkt.price, pkt.buy), pkt.qty);
        log_if_bbo_changed(ref_fp, npkts, uint64_t(pkt.timestamp), loc,
                           old_bid, old_ask, bbo_changes);
        break;
      }
      case (itch_t::ADD_ORDER_MPID): {
        auto const pkt = PROCESS<itch_t::ADD_ORDER_MPID>::read_from(&buf);
        uint16_t loc = pkt.add_msg.stock_locate;
        int32_t old_bid = order_book::s_books[loc].best_bid();
        int32_t old_ask = order_book::s_books[loc].best_ask();
        order_book::add_order(
            order_id_t(pkt.add_msg.oid), book_id_t(loc),
            mksigned(pkt.add_msg.price, pkt.add_msg.buy), pkt.add_msg.qty);
        log_if_bbo_changed(ref_fp, npkts, uint64_t(pkt.add_msg.timestamp), loc,
                           old_bid, old_ask, bbo_changes);
        break;
      }
      case (itch_t::EXECUTE_ORDER): {
        auto const pkt = PROCESS<itch_t::EXECUTE_ORDER>::read_from(&buf);
        uint16_t loc = order_book::get_book_idx(order_id_t(pkt.oid));
        int32_t old_bid = order_book::s_books[loc].best_bid();
        int32_t old_ask = order_book::s_books[loc].best_ask();
        order_book::execute_order(order_id_t(pkt.oid), pkt.qty);
        log_if_bbo_changed(ref_fp, npkts, uint64_t(pkt.timestamp), loc,
                           old_bid, old_ask, bbo_changes);
        break;
      }
      case (itch_t::EXECUTE_ORDER_WITH_PRICE): {
        auto const pkt =
            PROCESS<itch_t::EXECUTE_ORDER_WITH_PRICE>::read_from(&buf);
        uint16_t loc = order_book::get_book_idx(order_id_t(pkt.exec.oid));
        int32_t old_bid = order_book::s_books[loc].best_bid();
        int32_t old_ask = order_book::s_books[loc].best_ask();
        order_book::execute_order(order_id_t(pkt.exec.oid), pkt.exec.qty);
        log_if_bbo_changed(ref_fp, npkts, uint64_t(pkt.exec.timestamp), loc,
                           old_bid, old_ask, bbo_changes);
        break;
      }
      case (itch_t::REDUCE_ORDER): {
        auto const pkt = PROCESS<itch_t::REDUCE_ORDER>::read_from(&buf);
        uint16_t loc = order_book::get_book_idx(order_id_t(pkt.oid));
        int32_t old_bid = order_book::s_books[loc].best_bid();
        int32_t old_ask = order_book::s_books[loc].best_ask();
        order_book::cancel_order(order_id_t(pkt.oid), pkt.qty);
        log_if_bbo_changed(ref_fp, npkts, uint64_t(pkt.timestamp), loc,
                           old_bid, old_ask, bbo_changes);
        break;
      }
      case (itch_t::DELETE_ORDER): {
        auto const pkt = PROCESS<itch_t::DELETE_ORDER>::read_from(&buf);
        uint16_t loc = order_book::get_book_idx(order_id_t(pkt.oid));
        int32_t old_bid = order_book::s_books[loc].best_bid();
        int32_t old_ask = order_book::s_books[loc].best_ask();
        order_book::delete_order(order_id_t(pkt.oid));
        log_if_bbo_changed(ref_fp, npkts, uint64_t(pkt.timestamp), loc,
                           old_bid, old_ask, bbo_changes);
        break;
      }
      case (itch_t::REPLACE_ORDER): {
        auto const pkt = PROCESS<itch_t::REPLACE_ORDER>::read_from(&buf);
        uint16_t loc = order_book::get_book_idx(order_id_t(pkt.oid));
        int32_t old_bid = order_book::s_books[loc].best_bid();
        int32_t old_ask = order_book::s_books[loc].best_ask();
        order_book::replace_order(order_id_t(pkt.oid),
                                  order_id_t(pkt.new_order_id), pkt.new_qty,
                                  mksigned(pkt.new_price, BUY_SELL::BUY));
        log_if_bbo_changed(ref_fp, npkts, uint64_t(pkt.timestamp), loc,
                           old_bid, old_ask, bbo_changes);
        break;
      }
      default: {
        printf("Uh oh bad code %d\n", char(msgtype));
        assert(false);
        break;
      }
    }
  }

  fclose(ref_fp);
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  size_t nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  printf("%lu packets in %lu nanos , %.2f nanos per packet (BBO changes: %lu)\n",
         npkts, nanos, nanos / (double)npkts, bbo_changes);
}
