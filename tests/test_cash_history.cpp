// c++ -std=c++17 -Isrc tests/test_cash_history.cpp -o /tmp/t && /tmp/t
#include <cstdio>

#include "cash_history.hpp"

static int fails = 0, checks = 0;
#define CHECK(c)                                     \
  do {                                               \
    checks++;                                        \
    if (!(c)) {                                      \
      fails++;                                       \
      printf("FAIL line %d: %s\n", __LINE__, #c);    \
    }                                                \
  } while (0)

int main() {
  {  // the case from the field: $4.17 taken before the death flag
    CashHistory h;
    for (int t = 0; t < 5000; t += 16) h.Add(t, 417);
    for (int t = 5000; t < 5100; t += 16) h.Add(t, 0);
    CHECK(h.Highest(0) == 417);
  }
  {  // an old balance is forgotten
    CashHistory h;
    for (int t = 0; t < 2000; t += 16) h.Add(t, 900);
    for (int t = 2000; t < 2000 + CashHistory::kKeepMs + 1000; t += 16) h.Add(t, 100);
    CHECK(h.Highest(100) == 100);
  }
  {  // money picked up on the last frame counts
    CashHistory h;
    for (int t = 0; t < 1000; t += 16) h.Add(t, 100);
    CHECK(h.Highest(250) == 250);
  }
  {  // no history at all
    CashHistory h;
    CHECK(h.Highest(70) == 70);
  }
  {  // the game timer restarted: old samples do not count, the history keeps working
    CashHistory h;
    for (int t = 100000; t < 101000; t += 16) h.Add(t, 800);
    h.Add(10, 50);
    CHECK(h.Highest(50) == 50);
    for (int t = 26; t < 1000; t += 16) h.Add(t, 60);
    CHECK(h.Highest(0) == 60);
  }
  {  // Clear
    CashHistory h;
    h.Add(0, 500);
    h.Clear();
    CHECK(h.Highest(1) == 1);
  }
  {  // stays bounded
    CashHistory h;
    for (int t = 0; t < 3600000; t += 16) h.Add(t, t % 1000);
    CHECK(h.Highest(0) <= 999);
  }
  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
