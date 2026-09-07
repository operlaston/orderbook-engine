// Whole-application benchmark: drives the real server over TCP (loopback) and
// measures end-to-end round-trip latency + throughput through the full path
// (socket -> epoll parse -> SPSC queue -> matching engine -> eventfd -> send).
//
// By default this spawns build/<cfg>/orderbook itself and kills it on exit.
// Use --external to point at a server you started yourself.
//
//   bench_app [--conns N] [--ops N] [--warmup N]
//             [--workload new|cancel|mix] [--port P] [--nagle] [--external]

#include "GlobalConsts.h"
#include "MessageType.h"
#include "OrderType.h"
#include "Side.h"
#include "TimeInForce.h"
#include "Utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <barrier>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef SERVER_BIN
#define SERVER_BIN "build/release/orderbook"
#endif

namespace {

struct Config {
  int conns = 8;
  size_t ops = 200000; // total across all connections
  size_t warmup = 2000; // per connection
  std::string workload = "new";
  uint16_t port = 8080;
  bool nodelay = true;
  bool external = false;
};

[[noreturn]] void die(const char *msg) {
  std::perror(msg);
  std::exit(1);
}

Config parseArgs(int argc, char **argv) {
  Config c;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "--conns")
      c.conns = std::stoi(next());
    else if (a == "--ops")
      c.ops = std::stoull(next());
    else if (a == "--warmup")
      c.warmup = std::stoull(next());
    else if (a == "--workload")
      c.workload = next();
    else if (a == "--port")
      c.port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--nagle")
      c.nodelay = false;
    else if (a == "--external")
      c.external = true;
    else {
      std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(1);
    }
  }
  if (c.conns < 1)
    c.conns = 1;
  if (c.workload != "new" && c.workload != "cancel" && c.workload != "mix") {
    std::fprintf(stderr, "workload must be new|cancel|mix\n");
    std::exit(1);
  }
  return c;
}

// ---- socket helpers -------------------------------------------------------

int connectOnce(uint16_t port, bool nodelay) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    die("socket");

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  if (nodelay) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }
  return fd;
}

void writeAll(int fd, const void *buf, size_t n) {
  const char *p = static_cast<const char *>(buf);
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      die("write");
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
}

void readAll(int fd, void *buf, size_t n) {
  char *p = static_cast<char *>(buf);
  while (n > 0) {
    ssize_t r = ::read(fd, p, n);
    if (r == 0) {
      std::fprintf(stderr, "server closed connection mid-response\n");
      std::exit(1);
    }
    if (r < 0) {
      if (errno == EINTR)
        continue;
      die("read");
    }
    p += r;
    n -= static_cast<size_t>(r);
  }
}

// ---- wire encoding ------------------------------------------------------

constexpr size_t NEW_ORDER_WIRE = 1 + GlobalLengths::NEW_ORDER_MESSAGE;    // 20
constexpr size_t CANCEL_WIRE = 1 + GlobalLengths::CANCEL_ORDER_MESSAGE;    // 9
constexpr size_t NEW_ORDER_RESP = 9;
constexpr size_t CANCEL_RESP = 1;

void encodeNewOrder(uint8_t *buf, Side side, Price price, Quantity qty) {
  buf[0] = static_cast<uint8_t>(MessageType::NEW_ORDER);
  buf[1] = static_cast<uint8_t>(side);
  buf[2] = static_cast<uint8_t>(OrderType::LIMIT);
  buf[3] = static_cast<uint8_t>(TimeInForce::GOOD_TILL_CANCEL);
  uint64_t priceRaw = htobe64(std::bit_cast<uint64_t>(price));
  uint64_t qtyRaw = htobe64(qty);
  std::memcpy(buf + 4, &priceRaw, 8);
  std::memcpy(buf + 12, &qtyRaw, 8);
}

void encodeCancel(uint8_t *buf, OrderId id) {
  buf[0] = static_cast<uint8_t>(MessageType::CANCEL_ORDER);
  uint64_t idRaw = htobe64(id);
  std::memcpy(buf + 1, &idRaw, 8);
}

OrderId sendNewOrder(int fd, Side side, Price price, Quantity qty) {
  uint8_t req[NEW_ORDER_WIRE];
  encodeNewOrder(req, side, price, qty);
  writeAll(fd, req, sizeof(req));

  uint8_t resp[NEW_ORDER_RESP];
  readAll(fd, resp, sizeof(resp));
  uint64_t idRaw;
  std::memcpy(&idRaw, resp + 1, 8);
  return be64toh(idRaw);
}

void sendCancel(int fd, OrderId id) {
  uint8_t req[CANCEL_WIRE];
  encodeCancel(req, id);
  writeAll(fd, req, sizeof(req));
  uint8_t resp[CANCEL_RESP];
  readAll(fd, resp, sizeof(resp));
}

// ---- server lifecycle -------------------------------------------------

pid_t spawnServer(uint16_t port) {
  pid_t pid = ::fork();
  if (pid < 0)
    die("fork");
  if (pid == 0) {
    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::execl(SERVER_BIN, SERVER_BIN, static_cast<char *>(nullptr));
    ::perror("execl " SERVER_BIN);
    ::_exit(127);
  }

  // wait until the server is accepting connections
  for (int attempt = 0; attempt < 500; attempt++) {
    int fd = connectOnce(port, false);
    if (fd >= 0) {
      ::close(fd);
      return pid;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::fprintf(stderr, "server did not come up on port %u\n", port);
  ::kill(pid, SIGKILL);
  std::exit(1);
}

void killServer(pid_t pid) {
  ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

// ---- stats ----------------------------------------------------------

double pct(std::vector<uint64_t> &sorted, double p) {
  if (sorted.empty())
    return 0.0;
  double idx = p / 100.0 * static_cast<double>(sorted.size() - 1);
  size_t lo = static_cast<size_t>(std::floor(idx));
  size_t hi = static_cast<size_t>(std::ceil(idx));
  double frac = idx - static_cast<double>(lo);
  return static_cast<double>(sorted[lo]) * (1.0 - frac) +
         static_cast<double>(sorted[hi]) * frac;
}

void report(const char *name, std::vector<uint64_t> latenciesNs,
            double wallSeconds) {
  std::sort(latenciesNs.begin(), latenciesNs.end());
  size_t n = latenciesNs.size();
  double mean = 0.0;
  for (uint64_t v : latenciesNs)
    mean += static_cast<double>(v);
  mean /= static_cast<double>(n);

  std::printf("\n%s\n", name);
  std::printf("  ops:        %zu\n", n);
  std::printf("  wall:       %.3f s\n", wallSeconds);
  std::printf("  throughput: %.0f ops/s\n",
              static_cast<double>(n) / wallSeconds);
  std::printf("  latency (round-trip, us):\n");
  std::printf("    mean %.2f  min %.2f  p50 %.2f  p90 %.2f  p99 %.2f  "
              "p99.9 %.2f  max %.2f\n",
              mean / 1e3, static_cast<double>(latenciesNs.front()) / 1e3,
              pct(latenciesNs, 50) / 1e3, pct(latenciesNs, 90) / 1e3,
              pct(latenciesNs, 99) / 1e3, pct(latenciesNs, 99.9) / 1e3,
              static_cast<double>(latenciesNs.back()) / 1e3);
}

// ---- workers -------------------------------------------------------

using Clock = std::chrono::steady_clock;

// one worker owns one connection; results land in `out`
void runWorker(const Config &cfg, int workerIdx, size_t opsForWorker,
               std::barrier<> &startGate, std::vector<uint64_t> &out) {
  int fd = connectOnce(cfg.port, cfg.nodelay);
  if (fd < 0)
    die("connect");

  std::mt19937_64 rng(0xC0FFEE ^ static_cast<uint64_t>(workerIdx));
  std::uniform_int_distribution<int> priceDist(1, 1000);
  std::uniform_int_distribution<uint64_t> qtyDist(1, 1000);
  std::uniform_int_distribution<int> sideDist(0, 1);

  // warmup (also seeds a resting book for cancel/mix)
  std::vector<OrderId> liveIds;
  for (size_t i = 0; i < cfg.warmup; i++) {
    // rest away from the cross so warmup orders don't all fill
    Side side = (i & 1) ? Side::BUY : Side::SELL;
    Price price = (side == Side::BUY) ? 1.0 : 100000.0;
    OrderId id = sendNewOrder(fd, side, price, qtyDist(rng));
    if (id)
      liveIds.push_back(id);
  }

  bool cancelWork = cfg.workload == "cancel";
  bool mixWork = cfg.workload == "mix";

  // for a pure cancel run we need enough resting orders to cancel
  if (cancelWork) {
    while (liveIds.size() < opsForWorker) {
      Side side = (liveIds.size() & 1) ? Side::BUY : Side::SELL;
      Price price = (side == Side::BUY) ? 1.0 : 100000.0;
      OrderId id = sendNewOrder(fd, side, price, qtyDist(rng));
      if (id)
        liveIds.push_back(id);
    }
  }

  out.reserve(opsForWorker);
  size_t idCursor = 0;

  startGate.arrive_and_wait();

  for (size_t i = 0; i < opsForWorker; i++) {
    auto t0 = Clock::now();
    if (cancelWork) {
      sendCancel(fd, liveIds[idCursor++]);
    } else if (mixWork && (i & 1) && idCursor < liveIds.size()) {
      sendCancel(fd, liveIds[idCursor++]);
    } else {
      Side side = static_cast<Side>(sideDist(rng));
      OrderId id = sendNewOrder(fd, side, static_cast<Price>(priceDist(rng)),
                                qtyDist(rng));
      if (mixWork && id)
        liveIds.push_back(id);
    }
    auto t1 = Clock::now();
    out.push_back(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  }

  ::close(fd);
}

} // namespace

int main(int argc, char **argv) {
  Config cfg = parseArgs(argc, argv);

  std::printf("whole-app benchmark\n");
  std::printf("  server:   %s\n",
              cfg.external ? "external (already running)" : SERVER_BIN);
  std::printf("  conns:    %d\n", cfg.conns);
  std::printf("  workload: %s\n", cfg.workload.c_str());
  std::printf("  ops:      %zu total, warmup %zu/conn\n", cfg.ops, cfg.warmup);
  std::printf("  TCP_NODELAY: %s\n", cfg.nodelay ? "on" : "off (Nagle)");

  pid_t serverPid = -1;
  if (!cfg.external) {
    if (int probe = connectOnce(cfg.port, false); probe >= 0) {
      ::close(probe);
      std::fprintf(stderr,
                   "port %u already in use; refusing to spawn a server. "
                   "Use --external to target it.\n",
                   cfg.port);
      return 1;
    }
    serverPid = spawnServer(cfg.port);
  }

  size_t base = cfg.ops / static_cast<size_t>(cfg.conns);
  size_t remainder = cfg.ops % static_cast<size_t>(cfg.conns);

  std::vector<std::vector<uint64_t>> perWorker(cfg.conns);
  std::barrier startGate(cfg.conns + 1);

  std::vector<std::jthread> workers;
  workers.reserve(cfg.conns);
  for (int w = 0; w < cfg.conns; w++) {
    size_t opsForWorker = base + (static_cast<size_t>(w) < remainder ? 1 : 0);
    workers.emplace_back(runWorker, std::cref(cfg), w, opsForWorker,
                         std::ref(startGate), std::ref(perWorker[w]));
  }

  // release all workers together, then time the measured phase
  startGate.arrive_and_wait();
  auto start = Clock::now();
  for (auto &t : workers)
    t.join();
  auto end = Clock::now();

  double wall = std::chrono::duration<double>(end - start).count();

  std::vector<uint64_t> all;
  for (auto &v : perWorker)
    all.insert(all.end(), v.begin(), v.end());

  std::string label = cfg.workload + " (" + std::to_string(cfg.conns) + " conns)";
  report(label.c_str(), std::move(all), wall);

  if (serverPid > 0)
    killServer(serverPid);
  return 0;
}
