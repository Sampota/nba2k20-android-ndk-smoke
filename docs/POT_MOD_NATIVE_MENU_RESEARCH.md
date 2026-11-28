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
- Entry count: descriptor offset `+0xB0` -> 8.
- Entry stride: `0xEA8` bytes, not `0x38`.
- This stride is verified in the game's own `NavigationMenu_SetupItems` (VA `0xF40748`) and `NavigationMenu_CreateMenuItems` (VA `0xF40978`). Both use the descriptor count at `+0xB0`, base pointer at `+0x58`, and iterate by `0xEA8`.
- The previous 0x38 scan of the outer OPTIONS table was incorrect. The 0x38 stride appears in nested/runtime sub-item structures.

## Actual POT MOD insertion point under 2K NAV
The user's requested route is `2K NAV -> OPTIONS -> POT MOD`. The relevant menu is the nested subitem list attached to the OPTIONS row in `SlideOnNav_MainMenu`, not a ninth row in the separate `SlideOnNav_OptionsMenu` destination table.

- `SlideOnNav_MainMenu` descriptor at `0x2AA6EC0`: base `+0x58 = 0x2AA2578`, outer count `+0xB0 = 5`.
- Outer row index 1 is at `0x2AA3420` (type 5). Its nested subitem descriptors start at `0x2AA3468` = parent record `+0x48`.
- Nested subitem descriptor stride is `0x38`. The first five child slots (indices 0–4) have data; child slot 5 at `0x2AA3580` is currently all zero. Slots 6 and 7 are also zero.
- The engine's `NavigationMenu_SetupItems` iterates up to 64 nested subitem slots at offsets `+0x48 + n*0x38`, invoking any non-null setup callback at child offset `+0x00` with its runtime child instance.
- The user-selection path `NavigationMenu_SlideOn_Select` converts the raw cursor row to a nested subitem index and accepts indices 0..7 before obtaining the runtime child via `NavigationMenu_GetSubItemByIndex`.
- Thus slot 5 is inside the supported first eight nested subitem indices and is a better POT MOD insertion candidate than modifying the separate outer `SlideOnNav_OptionsMenu` array. The immediate next task is to establish the runtime child record fields populated from this static descriptor and confirm the submenu activation route before writing the CRC/callback fields.
- Do not overwrite `SlideOnNav_OptionsMenu` or the adjacent Playoffs table.

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
- `NavigationMenu_GetSubItemByIndex` at VA `0xF43F98` rejects an index above 7 at `0xF43FA0` (`cmp w2, #7`). Its implementation scans 0x38-byte child-subitem records inside a runtime item; this is a nested-subitem lookup, not the outer OPTIONS row-array lookup. Do not treat this check as an eight-row limit for the outer menu.
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

## Native one-row toggle template
The clean binary's GAMEPLAY settings page provides the requested control class:

- `OptionsMenu_GamePlay` descriptor VA: `0x273A008`.
- Difficulty row 0 VA: `0x273A9E0`; native interface object at `0x2E673E0`.
- Interface class: `MENU_OPTION_ITEM_TOGGLE_INTERFACE`.
- Constructor: VA `0xF3F3AC`; signature `(_bool (*)(), _bool (*)(), void (*)(GAMETEXT*), string_crc)`.
- Constructor stores the localized string CRC at interface `+0x18`, draw callback at `+0x20`, increment callback at `+0x28`, decrement callback at `+0x30`.
- `IncrementValue` at `0xF3F454` dispatches the callback at `+0x28`; `DecrementValue` at `0xF3F468` dispatches `+0x30`.
- Difficulty uses handlers `GlobalData_IncDifficulty` (`0xDF08D8`), `GlobalData_DecDifficulty` (`0xDF0994`), and `GlobalData_DrawDifficultyText` (`0xDF0A90`). Do not reuse these handlers because they modify actual game difficulty.
- `GlobalData_GetPlaylistPanelEnabledValueStringCrc(int)` at `0xDFCEA0` returns localized On/Off string CRC from a two-entry table. A POT callback can pass the plugin enabled state (0/1) and use the returned CRC with `VCTEXT::Draw(GAMETEXT*, string_crc, __vcc_va_list_t*)` to draw localized OFF/ON text.

Proposed POT MOD toggle callbacks (still to implement in the main-menu integration):
- Increment: enable the shotmeter plugin; when the old value was OFF, open the separate customization screen.
- Decrement: disable the shotmeter plugin and close customization if it is open.
- Draw: get plugin enabled state, resolve localized On/Off CRC through the native helper, and call the game's text draw function.
- GetBoolValue: return the plugin's enabled state using the stock callback contract expected by the menu row.

## What this confirms / what it does not
Confirmed:
- The clean ELF contains the `SlideOnNav_OptionsMenu` descriptor and all eight current outer records in file-backed `.data`; the correct outer stride is 0xEA8 and the outer row accessors use the descriptor count dynamically.
- The `+7` bounds check belongs to nested 0x38-byte sub-item lookup, not the outer row array.
- The Difficulty control uses the stock bool-radio interface that produces the native arrow/value presentation.

Not yet confirmed:
- The exact visible text corresponding to every OPTIONS name CRC.
- Which field/dispatcher performs the actual submenu activation for each item.
- The exact runtime creation and lifetime requirements for a cloned one-row POT MOD page.
- Whether an appended ninth row can be introduced safely without interfering with other menu descriptor tables or fixed-size assumptions.

## Implementation constraint
Keep the requested user-facing path exactly `2K NAV -> OPTIONS -> POT MOD`. POT MOD itself must contain only `SHOTMETER < OFF >` / `SHOTMETER < ON >`. The separate shotmeter customization UI remains owned by the shotmeter plugin. Do not add design/audio/green-animation rows to POT MOD.
