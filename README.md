# BountyPay for Red Dead Redemption 2

Losing money should pay off what you owe. BountyPay is a small ASI add-on for [Banks of the West](https://www.nexusmods.com/reddeadredemption2/mods/7289) (works without it too) for RDR2 Story Mode.

## Features

- **Death while the law is after you:** exactly the bounty of the state you are in is taken from your cash (all your cash if you have less). What the game itself takes on death counts towards it
- The bounty drops by the amount paid; a fully paid state loses its bounty and the red area on the map, as when the game clears it
- A death at the hands of lawmen or bounty hunters counts too
- **Surrender:** the whole bounty is taken from your cash, counting what the game charges for jail. If your cash was short, the rest is forgiven; a jail break by the gang costs nothing extra
- Your bank balance and the bounties in other states are never touched; story missions, a respawn in another state and a loaded save are skipped
- A notice in the top-left corner says how much was paid, in English or Russian (the game's language); every event goes to `BountyPay.log`
- DryRun mode: only write to the log what would be taken

## Requirements

- RDR2 1.0.1491.50 (Steam), Story Mode
- [ScriptHookRDR2 V2](https://www.nexusmods.com/reddeadredemption2/mods/1472) and an ASI loader

With Banks of the West, set `death_loss_fraction=0`, `bank_death_loss_fraction=0`, `surrender_loss_fraction=0`, `bank_surrender_loss_fraction=0` and `SurrenderClearsAllStates=0` in `BankRobberyLawBridge.ini`, so nothing is charged twice.

## Install

Copy `BountyPay.asi` and `BountyPay.ini` into the game folder. With Vortex, set the mod type that deploys into the game folder.

## Settings (`BountyPay.ini`)

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | 0 = the add-on does nothing |
| `DryRun` | 0 | 1 = change nothing, only write to the log what would be taken |
| `RequireWanted` | 1 | 1 = a death pays only when the law is after you or lawmen killed you; 0 = any death in a state where you have a bounty |
| `Notify` / `NotifyMs` | 1 / 7000 | the top-left notice and how long it stays, ms |
| `Language` | auto | the notice language: `auto` (the game's), `en`, `ru` |
| `SettleMs` | 4000 | wait after respawn or release before settling, ms |
| `DeathWindowMs` / `ArrestWindowMs` | 90000 / 240000 | a longer gap means a save was loaded: nothing is taken |

## Build

`./build.sh` cross-compiles `out/BountyPay.asi` with mingw-w64 in a throwaway Docker container. `TEST=1 ./build.sh` builds `out/BountyPay-test.asi` with test commands read from `BountyPay.test`.

## License

MIT

---

## По-русски

- Смерть в розыске: с наличных уходит ровно награда штата, где вы находитесь (или все наличные, если их меньше); то, что игра забрала сама, засчитывается
- Награда уменьшается на уплаченное; полностью оплаченный штат снимает розыск и красную зону на карте
- Сдача: уходит вся награда с учётом платы за тюрьму; если наличных не хватило, остаток прощается
- Банк и другие штаты не трогаются; сюжетные миссии и загрузка сохранения пропускаются
- Уведомление на языке игры и подробный журнал `BountyPay.log`
- С Banks of the West выставьте все доли потерь в 0 и `SurrenderClearsAllStates=0`
