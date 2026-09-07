#include "Orderbook.h"
#include "ResponseTypes.h"
#include "ServerEngineContext.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

int main() {
  ServerEngineContext ctx;
  Orderbook ob{ctx};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> priceDist(10, 1000);
  std::uniform_int_distribution<int> quantityDist(10, 1000);
  std::uniform_int_distribution<int> sideDist(0, 1);

  const size_t NUM_OPS = 1000000;

  std::vector<Price> prices(NUM_OPS);
  std::vector<Quantity> quantities(NUM_OPS);
  std::vector<Side> sides(NUM_OPS);

  Response::NewOrder res{};

  for (size_t i = 0; i < NUM_OPS; i++) {
    prices[i] = priceDist(gen);
    quantities[i] = quantityDist(gen);
    sides[i] = static_cast<Side>(sideDist(gen));
  }

  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < NUM_OPS; i++) {
    ob.addOrder(res, sides[i], prices[i], quantities[i], OrderType::LIMIT,
                TimeInForce::GOOD_TILL_CANCEL);
  }
  auto end = std::chrono::steady_clock::now();

  auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  double nsPerOp = elapsed / static_cast<double>(NUM_OPS);
  double opsPerSec = (static_cast<double>(NUM_OPS) / elapsed) * 1e9;

  std::printf("new_limit_orders_randomized\n");
  std::printf("  ops:        %zu\n", NUM_OPS);
  std::printf("  total time: %.6f s\n", static_cast<double>(elapsed) / 1e9);
  std::printf("  per op:     %.2f ns\n", nsPerOp);
  std::printf("  throughput: %.0f ops/s\n", opsPerSec);

  const size_t numRestingOrders = ob.getActiveOrders().size();
  std::vector<OrderId> restingOrders(numRestingOrders);
  int idx = 0;
  for (auto restingOrder : ob.getActiveOrders()) {
    restingOrders[idx++] = restingOrder.first;
  }

  Response::CancelOrder cancelRes{};

  start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < numRestingOrders; i++) {
    ob.cancelOrder(cancelRes, restingOrders[i]);
  }
  end = std::chrono::steady_clock::now();

  elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  nsPerOp = elapsed / static_cast<double>(numRestingOrders);
  opsPerSec = (static_cast<double>(numRestingOrders) / elapsed) * 1e9;

  std::printf("cancel_orders\n");
  std::printf("  ops:        %zu\n", numRestingOrders);
  std::printf("  total time: %.6f s\n", static_cast<double>(elapsed) / 1e9);
  std::printf("  per op:     %.2f ns\n", nsPerOp);
  std::printf("  throughput: %.0f ops/s\n", opsPerSec);
}
