# POT MOD native menu integration research (clean main ELF)

## Clean binary baseline
- File: `libnba2k20_clean_opt(1).so`
- SHA-256: `f0b31d2050700e3d49ba53b0c6a1ce4896137e07e77b7aeed2066ebbf1de4d09`
- ELF: 64-bit AArch64
- Build ID: `e8317c4f978861414000a5851641ad5543f7fc6b`
- SONAME: `libnba_clean_opt.so`

## OPTIONS slide-on menu
- Descriptor symbol: `SlideOnNav_OptionsMenu` at ELF VA `0x2ab3288`, size 224.
- Child-entry base pointer: descriptor offset `+0x58` -> `0x2ab3528`.
- Entry count: descriptor offset `+0xB4` -> 8.
- Entry stride: `0xEA8` bytes, not `0x38`.
- This stride is verified in the game's own `NavigationMenu_SetupItems` (VA `0xF40748`) and `NavigationMenu_CreateMenuItems` (VA `0xF40978`). Both use the descriptor count at `+0xB0`, base pointer at `+0x58`, and iterate by `0xEA8`.
- The previous 0x38 scan of the outer OPTIONS table was incorrect. The 0x38 stride appears in nested/runtime sub-item structures.

## Eight existing OPTIONS records
All addresses below are ELF virtual addresses. Callback fields refer to function pointers stored in each 0xEA8 record. The CRC values are recorded as opaque identifiers; labels are not asserted from CRC guesses.

| Index | Record VA | Type | Name CRC | Description CRC | Non-zero callbacks |
|---:|---:|---:|---:|---:|---|
| 0 | `0x2AB3528` | 4 | `0xC52801CE` | `0xEAA81A4C` | `+0xE38 -> 0x19A0DE4` |
| 1 | `0x2AB43D0` | 4 | `0x290692EA` | `0x4D339EAC` | `+0xE38 -> 0x19A0E1C` |
| 2 | `0x2AB5278` | 4 | `0xB1336C80` | `0xD035FA87` | `+0xE38 -> 0x19A0F1C` |
| 3 | `0x2AB6120` | 4 | `0x3276AFAA` | `0x55663ADE` | none |
| 4 | `0x2AB6FC8` | 3 | `0x8E0F42B3` | `0xD937F4F2` | `+0xE30 -> 0x19A0F6C`, `+0xE38 -> 0x19A0FCC`, `+0xE58 -> 0x19A108C` |
| 5 | `0x2AB7E70` | 5 | `0x458BFF78` | `0x42407F69` | none |
| 6 | `0x2AB8D18` | 5 | `0xCAA1EB57` | `0x7FEC344E` | `+0xE38 -> 0x19A1100` |
| 7 | `0x2AB9BC0` | 5 | `0xEB96F03F` | `0xD035FA87` | `+0xE38 -> 0x19A1310` |

The existing records also have `+0xE88 = 1` and `+0xE98 = 0xFFFFFFFF`. These fields are preserved; their semantics have not yet been established.

## Ninth OPTIONS row: allocation and index-limit findings
- The 8 existing OPTIONS records occupy exactly `8 * 0xEA8 = 0x7540` bytes, from `0x2AB3528` through the exclusive end address `0x2ABAA68`.
- `SlideOnNav_PlayoffsOptionsMenu` begins its own two-record table at exactly `0x2ABAA68`. Its descriptor is at `0x2AB3368`, base pointer at `+0x58 = 0x2ABAA68`, count at `+0xB0 = 2`. Therefore the apparent ninth slot is occupied by the Playoffs Options table and must not be overwritten.
- `NavigationMenu_GetSubItemByIndex` at VA `0xF43F98` rejects an index above 7 at `0xF43FA0` (`cmp w2, #7`). Its implementation scans 0x38-byte child-subitem records inside a runtime item; this is a nested-subitem lookup, not the outer OPTIONS row-array lookup. Do not assume this check is the outer menu's nine-row limit.
- The outer row accessors `NavigationMenu_GetItemInstanceByRawRow`, `NavigationMenu_GetItemInstanceByDisplayRow`, and `NavigationMenu_GetNumberOfEnabledItems` use the descriptor's `+0xB0` count and `+0x58` base pointer dynamically. The outer display-row scan advances by `0xEA8`.
- A ninth outer row therefore needs a separately allocated/copied 9-record `0xEA8` table (or an equally safe reserved region), with the same existing OPTIONS descriptor redirected to it and count updated. The Playoffs table must remain untouched. Before implementation, the activation path and the new row's `MENU_NAVIGATION_INSTANCE_ITEM` initialization/callback fields still need to be fully defined.

## Native GAMEPLAY row template
- Menu page symbol: `OptionsMenu_GamePlay` at VA `0x273A008`, size 224.
- Row pointer at page offset `+0x60` -> `0x273A9E0`.
- Row count at page offset `+0xB4` -> 17.
- Difficulty is row 0 at `0x273A9E0`; the native option-record stride is `0x38`.
- Difficulty row fields: type `1`, name CRC `0x36288E4D`, description CRC `0x602E2CBC`, interface object pointer `0x2E673E0`.
- The interface type is `MENU_OPTION_ITEM_BOOLRADIOBUTTON_INTERFACE`:
  - vtable object: `0x2520138`
  - constructor: `0xF3F904`
  - Increment callback dispatch: `0xF3F97C`
  - Decrement callback dispatch: `0xF3F990`
  - GetBoolValue callback dispatch: `0xF3F9A4`
  - DrawRadioButton callback dispatch: `0xF3F964`
- Constructor signature from the exported symbol: `(_bool (*)(), _bool (*)(), void (*)(GAMETEXT*, int), _bool (*)())`.
- Base option name and description rendering use the row's name/description string CRCs through `Localize_DrawText`. The native toggle interface uses its four stored callbacks for left/right, value rendering, and boolean readback.

## What this confirms / what it does not
Confirmed:
- The OPTIONS menu has eight populated native records, the correct outer stride is 0xEA8, and the stock index accessor has an explicit maximum index of 7.
- The Difficulty control uses the stock bool-radio interface that produces the native arrow/value presentation.

Not yet confirmed:
- The exact visible text corresponding to every OPTIONS name CRC.
- Which field/dispatcher performs the actual submenu activation for each item.
- The exact runtime creation and lifetime requirements for a cloned one-row POT MOD page.
- Whether an appended ninth row can be introduced safely without interfering with other menu descriptor tables or fixed-size assumptions.

## Implementation constraint
Keep the requested user-facing path exactly `2K NAV -> OPTIONS -> POT MOD`. POT MOD itself must contain only `SHOTMETER < OFF >` / `SHOTMETER < ON >`. The separate shotmeter customization UI remains owned by the shotmeter plugin. Do not add design/audio/green-animation rows to POT MOD.
