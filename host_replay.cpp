/* PC harness for replay.h: runs the exact replay loop the MicroBlaze runs.
 *
 *   g++ -O2 -std=c++14 host_replay.cpp -o host_replay
 *   ./host_replay <itch.bin> [out.csv]
 *
 * Writes the best-price changes in the same CSV format as
 * record_golden_book.py, and prints the record count and FNV-1a hash that the
 * board prints over UART, so a board run can be checked without dumping DDR.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "replay.h"

struct csv_sink {
  FILE *fp;
  void on_bbo(bbo_record const &r)
  {
    if (!fp) return;
    uint64_t const ts = (uint64_t(r.ts_hi) << 32) | r.ts_lo;
    fprintf(fp, "%u,%llu,%u,%d,%d\n", r.msg_idx, (unsigned long long)ts,
            r.locate, r.bid, r.ask);
  }
  void on_progress(uint32_t) {}
};

int main(int argc, char **argv)
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s <itch.bin> [out.csv]\n", argv[0]);
    return 2;
  }
  FILE *in = fopen(argv[1], "rb");
  if (!in) {
    perror(argv[1]);
    return 1;
  }
  fseek(in, 0, SEEK_END);
  long const len = ftell(in);
  fseek(in, 0, SEEK_SET);
  std::vector<unsigned char> image{};
  image.resize(size_t(len));
  if (fread(image.data(), 1, image.size(), in) != image.size()) {
    perror("read");
    return 1;
  }
  fclose(in);

  csv_sink sink = {nullptr};
  if (argc > 2) {
    sink.fp = fopen(argv[2], "w");
    if (!sink.fp) {
      perror(argv[2]);
      return 1;
    }
    fprintf(sink.fp,
            "msg_idx,timestamp_ns,stock_locate,best_bid_price,best_ask_price\n");
  }

  auto const t0 = std::chrono::steady_clock::now();
  replay_result const r = replay_itch(image.data(), image.size(), sink);
  auto const t1 = std::chrono::steady_clock::now();
  if (sink.fp) fclose(sink.fp);

  double const ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count();
  printf("image bytes      : %ld\n", len);
  printf("status           : %s", replay_status_str(r.status));
  if (r.status != REPLAY_OK) printf(" at byte offset %u", r.error_offset);
  printf("\nmessages         : %u (book %u)\n", r.total_msgs, r.book_msgs);
  printf("BBO changes      : %u\n", r.bbo_changes);
  printf("BBO hash (FNV64) : %016llx\n", (unsigned long long)r.hash);
  printf("peak live orders : %zu / %zu slots (%.1f%%)\n",
         order_book::oid_map.m_peak, order_book::oid_map.CAPACITY,
         100.0 * order_book::oid_map.m_peak / order_book::oid_map.CAPACITY);
  printf("price levels used: %zu (pool high-water)\n",
         order_book::s_levels.m_allocated.size());
  printf("PC time          : %.1f ns/book msg\n", ns / r.book_msgs);
  return r.status == REPLAY_OK ? 0 : 1;
}
