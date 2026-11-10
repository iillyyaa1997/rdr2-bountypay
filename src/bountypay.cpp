// BountyPay.asi - cash pays the bounty when you die or surrender while wanted.
// Add-on for Banks of the West (Nexus 7289); keep its death/surrender loss
// fractions at 0 and SurrenderClearsAllStates=0 so nothing is charged twice.
//
// Rules (operator decisions 2026-11-07):
//   Death while wanted: from the cash you carried, exactly the bounty of the
//     state you are in is taken (all cash if you had less). Whatever the game
//     itself takes on death counts towards it. The state bounty drops by the
//     amount paid and closes when fully paid.
//   Surrender / arrest: the whole bounty is taken from cash (all cash if less),
//     counting the part the game already charged for jail. The game clears the
//     bounty itself, as usual. If the bounty was not cleared (for example the
//     gang broke you out of jail) nothing extra is taken.
//   Other states and the bank balance are never touched. Story missions skipped.
//
// Natives are called through ScriptHookRDR2.dll exports resolved at runtime.
// Every event is written to BountyPay.log next to the .asi.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace {

constexpr const char* kVersion = "1.0.4";

constexpr const char* kExpInit = "?nativeInit@@YAX_K@Z";
constexpr const char* kExpPush = "?nativePush64@@YAX_K@Z";
constexpr const char* kExpCall = "?nativeCall@@YAPEA_KXZ";
constexpr const char* kExpScriptRegister = "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z";
constexpr const char* kExpScriptWait = "?scriptWait@@YAXK@Z";

using FnInit = void (*)(uint64_t);
using FnPush = void (*)(uint64_t);
using FnCall = uint64_t* (*)();
using FnScriptRegister = void (*)(HMODULE, void (*)());
using FnScriptWait = void (*)(DWORD);

// Native hashes (alloc8or/rdr3-nativedb-data).
constexpr uint64_t PLAYER_ID = 0x217E9DC48139933DULL;
constexpr uint64_t PLAYER_PED_ID = 0x096275889B8E0EE0ULL;
constexpr uint64_t IS_PLAYER_DEAD = 0x2E9C3FCB6798F397ULL;
constexpr uint64_t IS_PLAYER_PLAYING = 0xBFFB35986CAAE58CULL;
constexpr uint64_t IS_PLAYER_BEING_ARRESTED = 0xC8183AE963C58374ULL;
constexpr uint64_t GET_PLAYER_WANTED_LEVEL = 0xABC532F9098BFD9DULL;
constexpr uint64_t GET_BOUNTY = 0x54310AAB97B92816ULL;
constexpr uint64_t CLEAR_BOUNTY = 0xC76F252371150D9AULL;
constexpr uint64_t SET_WANTED_SCORE = 0xA80FF73F772ACF6AULL;
constexpr uint64_t SET_ENTITY_HEALTH = 0xAC2767ED8BDFAB15ULL;
constexpr uint64_t ADD_BOUNTY = 0x0E3BDEED21BEB945ULL;
constexpr uint64_t GET_WANTED_SCORE = 0xDD5FD601481F648BULL;
constexpr uint64_t MONEY_GET_CASH_BALANCE = 0x0C02DABFA3B98176ULL;
constexpr uint64_t MONEY_DECREMENT_CASH_BALANCE = 0x466BC8769CF26A7AULL;
constexpr uint64_t MONEY_INCREMENT_CASH_BALANCE = 0xBC3422DC91667621ULL;
constexpr uint32_t ADD_REASON_DEFAULT = 0x2CD419DCU;  // joaat("ADD_REASON_DEFAULT")
constexpr uint64_t GET_MISSION_FLAG = 0xB15CD1CF58771DE1ULL;
constexpr uint64_t GET_GAME_TIMER = 0x4F67E8ECA7D3F667ULL;
constexpr uint64_t GET_ENTITY_COORDS = 0xA86D5F069399F44DULL;
constexpr uint64_t GET_MAP_ZONE_AT_COORDS = 0x43AD8FC02B429D33ULL;
constexpr uint64_t VAR_STRING = 0xFA925AC00EB830B9ULL;
constexpr uint64_t UI_FEED_POST_HELP_TEXT = 0x049D5C615BD38BADULL;

HMODULE g_self = nullptr;
HMODULE g_sh = nullptr;
FnInit g_init = nullptr;
FnPush g_push = nullptr;
FnCall g_call = nullptr;
FnScriptWait g_wait = nullptr;

// Settings (BountyPay.ini, section [BountyPay]).
bool g_enabled = true;
bool g_dryRun = false;
bool g_requireWanted = true;
bool g_notify = true;
int g_notifyMs = 7000;
int g_settleMs = 4000;
int g_deathWindowMs = 90000;    // a longer gap means a save was loaded, not a respawn
int g_arrestWindowMs = 240000;  // arrest, jail fade and release
constexpr int kRefundDelayMs = 20000;  // let the save finish loading first
constexpr int kBountySettleMs = 3000;  // before the new bounty is read back
constexpr int kBountyWatchMs = 60000;  // how long later changes of the bounty are logged
int g_watchBountyUntil = 0;
int g_watchedBounty = 0;

FILE* g_log = nullptr;
std::wstring g_dir;

void Log(const char* fmt, ...) {
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

// ------------------------------------------------------------- natives

template <typename T>
uint64_t Arg(T v) {
  static_assert(sizeof(T) <= 8, "argument too large");
  uint64_t r = 0;
  std::memcpy(&r, &v, sizeof(T));
  return r;
}

template <typename R = void, typename... A>
R Call(uint64_t hash, A... args) {
  g_init(hash);
  (g_push(Arg(args)), ...);
  uint64_t* res = g_call();
  if constexpr (std::is_void_v<R>) {
    (void)res;
  } else {
    R out{};
    std::memcpy(&out, res, sizeof(R));
    return out;
  }
}

struct ScrVec {  // script vector: three floats, each in an 8-byte slot
  float x;
  uint32_t px;
  float y;
  uint32_t py;
  float z;
  uint32_t pz;
};

int Player() { return Call<int>(PLAYER_ID); }
int Cash() { return Call<int>(MONEY_GET_CASH_BALANCE); }
int Bounty(int p) { return Call<int>(GET_BOUNTY, p); }
int Now() { return Call<int>(GET_GAME_TIMER); }

uint32_t StateHere() {
  int ped = Call<int>(PLAYER_PED_ID);
  g_init(GET_ENTITY_COORDS);
  g_push(Arg(ped));
  g_push(Arg(1));
  g_push(Arg(1));
  ScrVec v;
  std::memcpy(&v, g_call(), sizeof(v));
  return Call<uint32_t>(GET_MAP_ZONE_AT_COORDS, v.x, v.y, v.z, 0);  // 0 = STATE
}

std::string Usd(int cents) {
  char b[32];
  snprintf(b, sizeof b, "$%d.%02d", cents / 100, cents % 100);
  return b;
}

void Notify(const std::string& text) {
  Log("notice: %s", text.c_str());
  if (!g_notify) return;
  const char* s = Call<const char*>(VAR_STRING, 10, "LITERAL_STRING", text.c_str());
  struct {
    int64_t duration;
    int64_t pad[3];
  } a = {g_notifyMs, {0, 0, 0}};
  struct {
    int64_t unk;
    const char* text;
    int64_t pad[2];
  } b = {0, s, {0, 0}};
  Call<int>(UI_FEED_POST_HELP_TEXT, &a, &b, 1);
}

// ------------------------------------------------------------ the logic

struct Snapshot {
  bool valid = false;
  int at = 0;        // game timer
  int cash = 0;      // cents
  int bounty = 0;    // cents, state the player stands in
  int wantedScore = 0;
  int wantedLevel = 0;
  bool mission = false;
  uint32_t state = 0;
};

enum class Phase { Watch, WaitRespawn, WaitRelease };

Snapshot TakeSnapshot(int p) {
  Snapshot s;
  s.valid = true;
  s.at = Now();
  s.cash = Cash();
  s.bounty = Bounty(p);
  s.wantedScore = Call<int>(GET_WANTED_SCORE, p);
  s.wantedLevel = Call<int>(GET_PLAYER_WANTED_LEVEL, p);
  s.mission = Call<int>(GET_MISSION_FLAG) != 0;
  s.state = StateHere();
  return s;
}

void LogSnap(const char* tag, const Snapshot& s) {
  Log("%s: cash=%d bounty=%d wantedScore=%d wantedLevel=%d mission=%d state=0x%08X", tag, s.cash, s.bounty,
      s.wantedScore, s.wantedLevel, s.mission, s.state);
}

bool Wanted(const Snapshot& s) { return s.wantedScore > 0 || s.wantedLevel > 0; }

void SettleDeath(int p, const Snapshot& before) {
  Snapshot after = TakeSnapshot(p);
  LogSnap("death before", before);
  LogSnap("death after ", after);
  if (before.mission) return Log("death: story mission, skipped");
  if (g_requireWanted && !Wanted(before)) return Log("death: not wanted at the moment of death, skipped");
  if (after.state != before.state) {
    return Log("death: respawned in another state (0x%08X -> 0x%08X), skipped", before.state, after.state);
  }
  if (after.cash > before.cash) return Log("death: cash grew across respawn (save loaded?), skipped");
  // The bounty read after respawn is the one SET_BOUNTY changes; it also catches
  // crimes reported while the player was dying.
  const int bounty = after.bounty;
  if (bounty <= 0) return Log("death: no bounty in this state, skipped");
  int target = before.cash < bounty ? before.cash : bounty;
  int gameTook = before.cash - after.cash;
  if (gameTook < 0) gameTook = 0;
  int extra = target - gameTook;
  if (extra < 0) extra = 0;
  if (extra > after.cash) extra = after.cash;
  int paid = gameTook + extra;
  if (paid > bounty) paid = bounty;
  int left = bounty - paid;
  Log("death: target=%d gameTook=%d extra=%d paid=%d bountyLeft=%d dryRun=%d", target, gameTook, extra, paid, left,
      g_dryRun);
  if (paid <= 0) return;
  if (!g_dryRun) {
    // SET_BOUNTY read back fine yet left the bounty the player sees unchanged, so the cash is taken
    // only after the bounty has moved.
    Call(CLEAR_BOUNTY, p);
    if (left > 0) Call(ADD_BOUNTY, p, left);
    g_wait(kBountySettleMs);
    const int reads = Bounty(p);
    if (reads != left) return Log("death: bounty should be %d but reads %d, no cash taken", left, reads);
    if (extra > 0) Call<int>(MONEY_DECREMENT_CASH_BALANCE, extra);
    g_watchBountyUntil = Now() + kBountyWatchMs;
    g_watchedBounty = reads;
    Log("death: applied, cash now %d, bounty now %d", Cash(), reads);
  }
  if (left == 0) {
    Notify("Награда погашена наличными: " + Usd(paid));
  } else {
    Notify("В уплату награды ушло " + Usd(paid) + " наличными. Осталось: " + Usd(left));
  }
}

void SettleArrest(int p, const Snapshot& before) {
  Snapshot after = TakeSnapshot(p);
  LogSnap("arrest before", before);
  LogSnap("arrest after ", after);
  if (before.mission) return Log("arrest: story mission, skipped");
  if (before.bounty <= 0) return Log("arrest: no bounty in this state, skipped");
  if (after.bounty >= before.bounty) {
    return Log("arrest: bounty not cleared (%d -> %d), e.g. jail break, nothing taken", before.bounty, after.bounty);
  }
  if (after.cash > before.cash) return Log("arrest: cash grew across release (save loaded?), skipped");
  int target = before.cash < before.bounty ? before.cash : before.bounty;
  int gameTook = before.cash - after.cash;
  if (gameTook < 0) gameTook = 0;
  int extra = target - gameTook;
  if (extra < 0) extra = 0;
  if (extra > after.cash) extra = after.cash;
  int paid = gameTook + extra;
  Log("arrest: target=%d gameTook=%d extra=%d paid=%d dryRun=%d", target, gameTook, extra, paid, g_dryRun);
  if (!g_dryRun && extra > 0) {
    Call<int>(MONEY_DECREMENT_CASH_BALANCE, extra);
    Log("arrest: applied, cash now %d", Cash());
  }
  if (paid <= 0) return;
  if (before.cash >= before.bounty) {
    Notify("Награда оплачена полностью: " + Usd(paid));
  } else {
    Notify("Наличных не хватило: в уплату награды ушло " + Usd(paid) + " из " + Usd(before.bounty));
  }
}

// One-shot return of money the add-on took by mistake: BountyPay.refund next to the .asi holds the
// amount in cents. The file is claimed first, so a crash can never pay twice.
void ApplyRefund() {
  const std::wstring path = g_dir + L"\\BountyPay.refund";
  const std::wstring claimed = path + L".claimed";
  int cents = 0;
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return;
  const int read = fscanf(f, "%d", &cents);
  fclose(f);
  if (read != 1 || cents <= 0) return Log("refund: bad amount in BountyPay.refund, ignored");
  if (!MoveFileExW(path.c_str(), claimed.c_str(), MOVEFILE_REPLACE_EXISTING)) return Log("refund: cannot claim the file");
  const int before = Cash();
  Call<int>(MONEY_INCREMENT_CASH_BALANCE, cents, ADD_REASON_DEFAULT);
  g_wait(500);
  const int now = Cash();
  Log("refund: %d cents, cash %d -> %d", cents, before, now);
  if (now - before >= cents) {
    DeleteFileW(claimed.c_str());
    Notify("Возвращено " + Usd(cents) + ", списанные по ошибке");
  } else {
    MoveFileExW(claimed.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    Log("refund: the balance did not grow, will try again next launch");
  }
}

// Test commands: BountyPay.test next to the .asi holds "bounty <cents>" or "die" and is consumed at once.
void CheckTestCommand(int p) {
  const std::wstring path = g_dir + L"\\BountyPay.test";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  char word[16] = {};
  int value = 0;
  FILE* f = _wfopen(path.c_str(), L"r");
  if (!f) return;
  const int read = fscanf(f, "%15s %d", word, &value);
  fclose(f);
  DeleteFileW(path.c_str());
  if (read >= 2 && strcmp(word, "bounty") == 0) {
    Call(CLEAR_BOUNTY, p);
    if (value > 0) Call(ADD_BOUNTY, p, value);
    Log("test: bounty set to %d, reads %d", value, Bounty(p));
  } else if (read >= 1 && strcmp(word, "die") == 0) {
    Call(SET_WANTED_SCORE, p, 500);
    g_wait(500);
    Log("test: wanted score %d, cash %d, bounty %d, killing the player", Call<int>(GET_WANTED_SCORE, p), Cash(), Bounty(p));
    Call(SET_ENTITY_HEALTH, Call<int>(PLAYER_PED_ID), 0, 0);
  } else {
    Log("test: unknown command in BountyPay.test");
  }
}

void ScriptMain() {
  Log("script started (Enabled=%d DryRun=%d RequireWanted=%d Notify=%d)", g_enabled, g_dryRun, g_requireWanted,
      g_notify);
  Phase phase = Phase::Watch;
  Snapshot last, pending;
  int settleFrom = 0, waitFrom = 0;
  bool wasDead = false, wasArrested = false, wasPlaying = true;
  const int startedAt = Now();
  bool refundChecked = false;
  int lastTestCheck = 0;
  for (;;) {
    g_wait(0);
    if (!g_enabled) {
      g_wait(1000);
      continue;
    }
    int p = Player();
    bool dead = Call<int>(IS_PLAYER_DEAD, p) != 0;
    bool arrested = Call<int>(IS_PLAYER_BEING_ARRESTED, p, 0) != 0 || Call<int>(IS_PLAYER_BEING_ARRESTED, p, 1) != 0;
    bool playing = Call<int>(IS_PLAYER_PLAYING, p) != 0;
    int now = Now();
    if (phase == Phase::Watch && playing && !dead && !arrested && now - lastTestCheck >= 1000) {
      lastTestCheck = now;
      CheckTestCommand(p);
    }
    if (g_watchBountyUntil && now < g_watchBountyUntil) {
      const int bounty = Bounty(p);
      if (bounty != g_watchedBounty) {
        Log("bounty changed afterwards: %d -> %d", g_watchedBounty, bounty);
        g_watchedBounty = bounty;
      }
    } else {
      g_watchBountyUntil = 0;
    }
    if (!refundChecked && playing && !dead && !arrested && now - startedAt > kRefundDelayMs) {
      refundChecked = true;
      ApplyRefund();
    }
    if (dead != wasDead || arrested != wasArrested || playing != wasPlaying) {
      Log("flags: dead=%d arrested=%d playing=%d cash=%d bounty=%d", dead, arrested, playing, Cash(), Bounty(p));
      wasDead = dead;
      wasArrested = arrested;
      wasPlaying = playing;
    }
    switch (phase) {
      case Phase::Watch:
        if (playing && !dead && !arrested) {
          last = TakeSnapshot(p);
        } else if (last.valid && now - last.at < 15000 && (dead || arrested)) {
          pending = last;
          phase = dead ? Phase::WaitRespawn : Phase::WaitRelease;
          waitFrom = now;
          settleFrom = 0;
          Log("%s detected", dead ? "death" : "arrest");
          LogSnap("snapshot", pending);
        }
        break;
      case Phase::WaitRespawn:
      case Phase::WaitRelease: {
        int limit = phase == Phase::WaitRespawn ? g_deathWindowMs : g_arrestWindowMs;
        if (now - waitFrom > limit || now < waitFrom) {
          Log("waiting too long, event dropped");
          phase = Phase::Watch;
          last.valid = false;
          break;
        }
        if (phase == Phase::WaitRespawn && arrested) {  // death turned into an arrest
          Log("death flow became an arrest");
          phase = Phase::WaitRelease;
        }
        if (!playing || dead || arrested) {
          settleFrom = 0;
          break;
        }
        if (!settleFrom) settleFrom = now;
        if (now - settleFrom < g_settleMs) break;
        if (phase == Phase::WaitRespawn) {
          SettleDeath(p, pending);
        } else {
          SettleArrest(p, pending);
        }
        phase = Phase::Watch;
        last.valid = false;
        break;
      }
    }
  }
}

void ReadIni(const std::wstring& ini) {
  auto get = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"BountyPay", k, d, ini.c_str()); };
  g_enabled = get(L"Enabled", 1) != 0;
  g_dryRun = get(L"DryRun", 0) != 0;
  g_requireWanted = get(L"RequireWanted", 1) != 0;
  g_notify = get(L"Notify", 1) != 0;
  g_notifyMs = get(L"NotifyMs", 7000);
  g_settleMs = get(L"SettleMs", 4000);
  g_deathWindowMs = get(L"DeathWindowMs", 90000);
  g_arrestWindowMs = get(L"ArrestWindowMs", 240000);
}

bool Register() {
  g_init = (FnInit)GetProcAddress(g_sh, kExpInit);
  g_push = (FnPush)GetProcAddress(g_sh, kExpPush);
  g_call = (FnCall)GetProcAddress(g_sh, kExpCall);
  auto reg = (FnScriptRegister)GetProcAddress(g_sh, kExpScriptRegister);
  g_wait = (FnScriptWait)GetProcAddress(g_sh, kExpScriptWait);
  if (!g_init || !g_push || !g_call || !reg || !g_wait) {
    Log("ScriptHookRDR2 exports not found, BountyPay inactive");
    return false;
  }
  reg(g_self, ScriptMain);
  Log("registered with ScriptHookRDR2");
  return true;
}

DWORD WINAPI Boot(LPVOID) {
  for (int i = 0; i < 600 && !(g_sh = GetModuleHandleW(L"ScriptHookRDR2.dll")); ++i) Sleep(100);
  if (!g_sh) {
    Log("ScriptHookRDR2.dll not loaded, BountyPay inactive");
    return 0;
  }
  Register();
  return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  g_self = inst;
  DisableThreadLibraryCalls(inst);
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(inst, path, MAX_PATH);
  std::wstring p = path;
  std::wstring dir = p.substr(0, p.find_last_of(L"\\/"));
  g_dir = dir;
  g_log = _wfopen((dir + L"\\BountyPay.log").c_str(), L"a");
  Log("BountyPay %s loaded", kVersion);
  ReadIni(dir + L"\\BountyPay.ini");
  if ((g_sh = GetModuleHandleW(L"ScriptHookRDR2.dll"))) {
    Register();
  } else {
    HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}
