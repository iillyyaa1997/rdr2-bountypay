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

constexpr const char* kVersion = "1.1.2";

constexpr const char* kExpInit = "?nativeInit@@YAX_K@Z";
constexpr const char* kExpPush = "?nativePush64@@YAX_K@Z";
constexpr const char* kExpCall = "?nativeCall@@YAPEA_KXZ";
constexpr const char* kExpScriptRegister = "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z";
constexpr const char* kExpScriptWait = "?scriptWait@@YAXK@Z";
constexpr const char* kExpGetGlobalPtr = "?getGlobalPtr@@YAPEA_KH@Z";

using FnInit = void (*)(uint64_t);
using FnPush = void (*)(uint64_t);
using FnCall = uint64_t* (*)();
using FnScriptRegister = void (*)(HMODULE, void (*)());
using FnScriptWait = void (*)(DWORD);
using FnGetGlobalPtr = uint64_t* (*)(int);

// Native hashes (alloc8or/rdr3-nativedb-data).
constexpr uint64_t PLAYER_ID = 0x217E9DC48139933DULL;
constexpr uint64_t PLAYER_PED_ID = 0x096275889B8E0EE0ULL;
constexpr uint64_t IS_PLAYER_DEAD = 0x2E9C3FCB6798F397ULL;
constexpr uint64_t IS_PLAYER_PLAYING = 0xBFFB35986CAAE58CULL;
constexpr uint64_t IS_PLAYER_BEING_ARRESTED = 0xC8183AE963C58374ULL;
constexpr uint64_t GET_PLAYER_WANTED_LEVEL = 0xABC532F9098BFD9DULL;
constexpr uint64_t GET_BOUNTY = 0x54310AAB97B92816ULL;
constexpr uint64_t SET_BOUNTY = 0x093A9D1F72DF0D19ULL;
constexpr uint64_t SET_WANTED_SCORE = 0xA80FF73F772ACF6AULL;
constexpr uint64_t SET_ENTITY_HEALTH = 0xAC2767ED8BDFAB15ULL;
constexpr uint64_t GET_WANTED_SCORE = 0xDD5FD601481F648BULL;
constexpr uint64_t MONEY_GET_CASH_BALANCE = 0x0C02DABFA3B98176ULL;
constexpr uint64_t MONEY_DECREMENT_CASH_BALANCE = 0x466BC8769CF26A7AULL;
constexpr uint64_t MONEY_INCREMENT_CASH_BALANCE = 0xBC3422DC91667621ULL;
constexpr uint32_t ADD_REASON_DEFAULT = 0x2CD419DCU;  // joaat("ADD_REASON_DEFAULT")
constexpr uint64_t GET_MISSION_FLAG = 0xB15CD1CF58771DE1ULL;
constexpr uint64_t GET_GAME_TIMER = 0x4F67E8ECA7D3F667ULL;
constexpr uint64_t GET_ENTITY_COORDS = 0xA86D5F069399F44DULL;
constexpr uint64_t GET_MAP_ZONE_AT_COORDS = 0x43AD8FC02B429D33ULL;
constexpr uint64_t GET_PED_SOURCE_OF_DEATH = 0x93C8B64DEB84728CULL;
constexpr uint64_t IS_ENTITY_A_PED = 0xCF8176912DDA4EA5ULL;
constexpr uint64_t IS_ENTITY_A_VEHICLE = 0xC3D96AF45FCCEC4CULL;
constexpr uint64_t GET_DRIVER_OF_VEHICLE = 0x2963B5C1637E8A27ULL;
constexpr uint64_t GET_ENTITY_MODEL = 0xDA76A9F39210D365ULL;
constexpr uint64_t GET_PED_RELATIONSHIP_GROUP_HASH = 0x7DBDD04862D95F04ULL;
constexpr uint64_t VAR_STRING = 0xFA925AC00EB830B9ULL;
constexpr uint64_t UI_FEED_POST_HELP_TEXT = 0x049D5C615BD38BADULL;

HMODULE g_self = nullptr;
HMODULE g_sh = nullptr;
FnInit g_init = nullptr;
FnPush g_push = nullptr;
FnCall g_call = nullptr;
FnScriptWait g_wait = nullptr;
FnGetGlobalPtr g_globalPtr = nullptr;
bool g_tableUsable = false;  // the table offset is verified for one game version only

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

// The bounty of every state lives in a script global (short_update: func_601, func_790):
// Global_40.f_358[state], element size 12, the array's first slot holds its length. GET_BOUNTY and
// SET_BOUNTY see only the state the player is in, and the map reads the table, so both must change.
constexpr int kBountyTable = 40 + 358 + 1;
constexpr int kBountyStride = 12;
constexpr const char* kStateNames[] = {"Ambarino", "New Hanover", "Lemoyne", "West Elizabeth", "New Austin", "Guarma"};
constexpr int kStateCount = sizeof(kStateNames) / sizeof(kStateNames[0]);
constexpr int kZoneDistrict = 10;

// Jenkins one-at-a-time, the game's name hash (lower case)
constexpr uint32_t Joaat(const char* s) {
  uint32_t h = 0;
  for (; *s; ++s) {
    const char c = (*s >= 'A' && *s <= 'Z') ? static_cast<char>(*s - 'A' + 'a') : *s;
    h += static_cast<uint8_t>(c);
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

struct District {
  uint32_t hash;  // joaat of the district name, written without spaces or underscores
  int state;
};
// Seen in game: heartlands, grizzlieswest, scarlettmeadows, bayounwa, bigvalley, greatplains
constexpr District kDistricts[] = {
    {Joaat("grizzlieseast"), 0},  {Joaat("grizzlieswest"), 0},    {Joaat("heartlands"), 1},
    {Joaat("cumberlandforest"), 1}, {Joaat("roanokeridge"), 1},   {Joaat("scarlettmeadows"), 2},
    {Joaat("bluewatermarsh"), 2}, {Joaat("bayounwa"), 2},         {Joaat("bigvalley"), 3},
    {Joaat("greatplains"), 3},    {Joaat("talltrees"), 3},        {Joaat("chollasprings"), 4},
    {Joaat("riobravo"), 4},       {Joaat("gaptoothridge"), 4},    {Joaat("hennigansstead"), 4},
    {Joaat("guarma"), 5},
};
static_assert(Joaat("heartlands") == 0x07D4FF5F, "joaat");
static_assert(Joaat("scarlettmeadows") == 0xCC7C3314, "joaat");
// Who the law sends after a player with a bounty (dispatch.meta and wilderness/bountyhunters.meta:
// the bounty hunter riders by bounty level, marshals, Pinkertons, their dogs) and the lawmen
// themselves. Bounty hunters come only while the player is not wanted, so a death at their hands
// counts as a law death even when the wanted level reads zero.
constexpr uint32_t kLawModels[] = {
    Joaat("g_m_m_bountyhunters_01"),      Joaat("s_m_m_sdcowpoke_01"),
    Joaat("g_m_m_unilangstonboys_01"),    Joaat("s_m_m_marshallsrural_01"),
    Joaat("s_m_m_pinlaw_01"),             Joaat("a_c_doghound_01"),
    Joaat("s_m_m_dispatchlawrural_01"),   Joaat("s_m_m_dispatchleaderrural_01"),
    Joaat("s_m_m_ambientlawrural_01"),    Joaat("s_m_m_dispatchpolice_01"),
    Joaat("s_m_m_dispatchleaderpolice_01"), Joaat("s_m_m_ambientblwpolice_01"),
    Joaat("s_m_m_ambientsdpolice_01"),    Joaat("s_m_m_valdeputy_01"),
    Joaat("a_m_m_valdeputyresident_01"),  Joaat("a_m_m_rhddeputyresident_01"),
    Joaat("a_m_m_strdeputyresident_01"),  Joaat("a_m_m_asbdeputyresident_01"),
    Joaat("a_m_m_armdeputyresident_01"),
};

// Some spots (rivers, a few gaps) have no district: the last known state counts while the player
// is still within this distance of where it was seen.
constexpr float kLastStateRadius = 400.0f;

// The script globals move between game builds, so the table is used on the verified build only
constexpr DWORD kGameVersionMS = (1u << 16) | 0u;     // 1.0
constexpr DWORD kGameVersionLS = (1491u << 16) | 50u;  // .1491.50

bool GameVersionIsVerified() {
  HMODULE ver = LoadLibraryW(L"version.dll");
  if (!ver) return false;
  using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
  using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
  using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
  auto size = (SizeFn)GetProcAddress(ver, "GetFileVersionInfoSizeW");
  auto info = (InfoFn)GetProcAddress(ver, "GetFileVersionInfoW");
  auto query = (QueryFn)GetProcAddress(ver, "VerQueryValueW");
  bool ok = false;
  wchar_t exe[MAX_PATH];
  if (size && info && query && GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
    DWORD ignored = 0;
    const DWORD n = size(exe, &ignored);
    std::string data(n, '\0');
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    if (n && info(exe, 0, n, &data[0]) && query(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &len) && fixed) {
      Log("game version %u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
          HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
      ok = fixed->dwFileVersionMS == kGameVersionMS && fixed->dwFileVersionLS == kGameVersionLS;
    }
  }
  FreeLibrary(ver);
  return ok;
}

int* TableSlot(int state) { return reinterpret_cast<int*>(g_globalPtr(kBountyTable + kBountyStride * state)); }

ScrVec PlayerCoords() {
  int ped = Call<int>(PLAYER_PED_ID);
  g_init(GET_ENTITY_COORDS);
  g_push(Arg(ped));
  g_push(Arg(1));
  g_push(Arg(1));
  ScrVec v;
  std::memcpy(&v, g_call(), sizeof(v));
  return v;
}

// The state the player stands in, -1 if neither the district nor a nearby last known state tells
int StateHere() {
  static int lastState = -1;
  static ScrVec lastAt{};
  const ScrVec v = PlayerCoords();
  const uint32_t district = Call<uint32_t>(GET_MAP_ZONE_AT_COORDS, v.x, v.y, v.z, kZoneDistrict);
  for (const District& d : kDistricts) {
    if (d.hash == district) {
      lastState = d.state;
      lastAt = v;
      return d.state;
    }
  }
  const float dx = v.x - lastAt.x, dy = v.y - lastAt.y;
  const bool inReach = lastState >= 0 && dx * dx + dy * dy <= kLastStateRadius * kLastStateRadius;
  static uint32_t warned = 1;
  if (district != warned) {
    warned = district;
    Log("unknown district 0x%08X at %.1f %.1f %.1f, %s", district, v.x, v.y, v.z,
        inReach ? kStateNames[lastState] : "no state nearby");
  }
  return inReach ? lastState : -1;
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
  int state = -1;
  uint32_t killerModel = 0;  // set when the death is detected
  bool killedByLaw = false;
};

// The model of whoever killed the player (the driver, if it was a vehicle), 0 if nobody
uint32_t KillerModel() {
  const int ped = Call<int>(PLAYER_PED_ID);
  int killer = Call<int>(GET_PED_SOURCE_OF_DEATH, ped);
  if (killer && Call<int>(IS_ENTITY_A_VEHICLE, killer)) killer = Call<int>(GET_DRIVER_OF_VEHICLE, killer);
  if (!killer || !Call<int>(IS_ENTITY_A_PED, killer)) return 0;
  const uint32_t model = Call<uint32_t>(GET_ENTITY_MODEL, killer);
  Log("killed by ped model 0x%08X, relationship group 0x%08X", model,
      Call<uint32_t>(GET_PED_RELATIONSHIP_GROUP_HASH, killer));
  return model;
}

bool IsLawModel(uint32_t model) {
  for (uint32_t m : kLawModels) {
    if (m == model) return true;
  }
  return false;
}

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
  Log("%s: cash=%d bounty=%d wantedScore=%d wantedLevel=%d mission=%d state=%d", tag, s.cash, s.bounty,
      s.wantedScore, s.wantedLevel, s.mission, s.state);
}

bool Wanted(const Snapshot& s) { return s.wantedScore > 0 || s.wantedLevel > 0; }

void SettleDeath(int p, const Snapshot& before) {
  Snapshot after = TakeSnapshot(p);
  LogSnap("death before", before);
  LogSnap("death after ", after);
  if (before.mission) return Log("death: story mission, skipped");
  if (g_requireWanted && !Wanted(before) && !before.killedByLaw) {
    return Log("death: not wanted and not killed by the law or bounty hunters, skipped");
  }
  if (!Wanted(before) && before.killedByLaw) Log("death: killed by the law or bounty hunters (model 0x%08X)", before.killerModel);
  if (after.state != before.state) {
    return Log("death: respawned in another state (%d -> %d), skipped", before.state, after.state);
  }
  if (after.cash > before.cash) return Log("death: cash grew across respawn (save loaded?), skipped");
  const int state = after.state;
  if (!g_tableUsable) return Log("death: this game version is not verified, nothing taken");
  if (state < 0) return Log("death: state unknown, skipped");
  // Native first: it also catches crimes reported while the player was dying, the table can lag.
  const int bounty = Bounty(p);
  const int table = *TableSlot(state);
  Log("death: %s, native %d, table %d", kStateNames[state], bounty, table);
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
    // The cash is taken only after the bounty has moved
    Call(SET_BOUNTY, p, left);
    *TableSlot(state) = left;
    g_wait(kBountySettleMs);
    const int reads = *TableSlot(state);
    if (reads != left || Bounty(p) != left) {
      return Log("death: bounty should be %d but table reads %d and native %d, no cash taken", left, reads, Bounty(p));
    }
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
  if (read >= 1 && strcmp(word, "states") == 0) {
    char line[200] = "";
    for (int i = 0; i < kStateCount && g_tableUsable; ++i) {
      snprintf(line + strlen(line), sizeof line - strlen(line), " %s=%d", kStateNames[i], *TableSlot(i));
    }
    Log("table:%s | native %d, here %d", line, Bounty(p), StateHere());
  } else if (read >= 1 && strcmp(word, "zones") == 0) {
    const ScrVec v = PlayerCoords();
    for (int type = -1; type <= 12; ++type) {
      Log("zone type %d: 0x%08X", type, Call<uint32_t>(GET_MAP_ZONE_AT_COORDS, v.x, v.y, v.z, type));
    }
  } else if (read >= 2 && strcmp(word, "scan") == 0) {
    // finds where a known amount sits among the first globals
    for (int g = 0; g < 4000 && g_globalPtr; ++g) {
      if (static_cast<int>(*g_globalPtr(g)) == value) Log("global %d = %d", g, value);
    }
  } else if (read >= 2 && strcmp(word, "money") == 0) {
    Call<int>(MONEY_INCREMENT_CASH_BALANCE, value, ADD_REASON_DEFAULT);
    g_wait(300);
    Log("test: cash now %d", Cash());
  } else if (read >= 2 && strcmp(word, "bounty") == 0) {
    const int state = StateHere();
    if (state < 0 || !g_tableUsable) return Log("test: state unknown or game version not verified");
    Call(SET_BOUNTY, p, value);
    *TableSlot(state) = value;
    Log("test: bounty of %s set to %d, table %d, native %d", kStateNames[state], value, *TableSlot(state), Bounty(p));
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
  g_tableUsable = g_globalPtr && GameVersionIsVerified();
  Log("bounty table usable: %d", g_tableUsable);
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
          if (dead) {
            pending.killerModel = KillerModel();
            pending.killedByLaw = pending.killerModel && IsLawModel(pending.killerModel);
          }
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
  g_globalPtr = (FnGetGlobalPtr)GetProcAddress(g_sh, kExpGetGlobalPtr);
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
