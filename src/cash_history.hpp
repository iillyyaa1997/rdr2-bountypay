#ifndef CASH_HISTORY_HPP
#define CASH_HISTORY_HPP

#include <deque>

// The game takes the cash a moment before IS_PLAYER_DEAD turns true, so the last frame alive
// already shows the reduced balance (2026-11-10: $4.17 taken, the last frame alive read $0).
// Recent balances tell what the player really carried.
class CashHistory {
 public:
  static constexpr int kKeepMs = 6000;
  static constexpr int kSampleMs = 250;

  void Add(int now, int cash) {
    if (!samples_.empty() && now < samples_.back().at) samples_.clear();  // the game timer restarted
    if (samples_.empty() || now - samples_.back().at >= kSampleMs) samples_.push_back({now, cash});
    while (now - samples_.front().at > kKeepMs) samples_.pop_front();
  }

  // The most the player carried recently, `current` being the balance of the last frame alive
  int Highest(int current) const {
    int high = current;
    for (const Sample& s : samples_) {
      if (s.cash > high) high = s.cash;
    }
    return high;
  }

  void Clear() { samples_.clear(); }

 private:
  struct Sample {
    int at;
    int cash;
  };
  std::deque<Sample> samples_;
};

#endif
