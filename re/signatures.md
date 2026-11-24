# Signatures

Each entry: pattern (`??` = wildcard), exe hash it was made from, purpose, confidence.
Code must require exactly one match; when it fails, use the RVA fallback only if the exe
hash is in `src/mod/known_exe.h`.

## binds_table_getter
- **pattern:** `8B 44 24 08 8B 4C 24 04 8D 04 88 C1 E0 06 05 ?? ?? ?? ?? C3`
- **extract:** imm32 at +15 = live binding table base (`int32 code[set][player][16]`)
- **exe:** `bc49a82c8234fb80ac071ad43698598cfa104c363688ed12d248b89b7573093e` → match at
  `0x4b2802`, imm = `0x69f5f0` (RVA fallback `0x29f5f0`)
- **used by:** `src/mod/p2_binds.cpp` (P2 keyboard codes = base + 0x40)
- **confidence:** confirmed (unique in `.text`; default table at 0x5f4bb0 matches the ReadMe
  binds; resolved at runtime inside the game: `P2 binds: game table at 0x0069F5F0 (signature)`)

## Used by widescreen / char select (all unique in bc49a82c...)
| name | address | pattern | extract |
|---|---|---|---|
| video_mode_filter | `0x4b0376` | `8B 45 D0 3D 80 02 00 00 0F 8C ?? ?? ?? ?? 8B 4D D4 81 F9 E0 01 00 00 0F 8C ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 75 ?? 3D 00 04 00 00 0F 8F ?? ?? ?? ?? 81 F9 00 03 00 00 0F 8F ?? ?? ?? ?? 3D 40 06 00 00 7F ?? 81 F9 B0 04 00 00 7F ?? C1 E1 02 8D 04 40 3B C1 75 ??` | patch +36 `75`->`EB`, +62/+70 imm -> 8192, +84 `75 xx`->`90 90` |
| select_video_mode | `0x4af77c` | `8B 0D ?? ?? ?? ?? 69 C9 A8 00 00 00 03 0D ?? ?? ?? ?? 53 8B 5C 24 10 56 8B 71 54` | +2 &adapter index, +14 &adapters |
| RwCameraSetViewWindow | `0x4bdad0` | `8B 44 24 08 56 8B 74 24 08 8B 08 D9 05 ?? ?? ?? ?? 89 4E 68 8B 50 04 D8 76 68 89 56 6C` | hook |
| RwIm2DRenderPrimitive | `0x4b8130` | `A1 ?? ?? ?? ?? FF 60 30` | +1 &RwEngineInstance |
| char_select_update | `0x495881` | `55 8B EC 51 53 56 8B 75 08 8B 46 04 57 6A 02 5B 33 FF 3B C3 74 ?? 83 F8 03 74 ?? 83 F8 05 74 ?? 83 F8 06` | hook |
| assign_pad_slot | `0x4a971e` | `56 8B 74 24 08 69 F6 AC 00 00 00 81 C6 ?? ?? ?? ?? 83 3E 00 7C 05 E8` | +13 &pad slots; callable |

## Candidates for v2 (not used by code yet)
| purpose | address (bc49a82c…) | first bytes |
|---|---|---|
| input combine → final per-player masks | `0x4b2a2f` | `55 8B EC 83 EC 0C 53 56 57 E8 ?? ?? ?? ?? 6A 06 33 C0 59 BF ?? ?? ?? ?? F3 AB` |
| raw per-set masks from bind pointers | `0x4b2839` | `55 8B EC 83 EC 10 53 56 57 33 DB C7 45 F8 ?? ?? ?? ??` |

## Main menu, window, controls, sound (tmntnet 0.5-0.7; all unique in bc49a82c...)
| name | address | pattern | extract |
|---|---|---|---|
| main_menu_init | `0x48cda6` | `55 8B EC 53 56 57 8B 7D 08 68 B4 00 00 00 33 DB 53 57 E8` | hook |
| main_menu_update | `0x48cee4` | `55 8B EC 51 51 56 57 8D 45 FC 50 8D 45 F8 50 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 8B 7D 08 EB` | hook |
| main_menu_item_count | `0x48cdea` | `83 67 10 00 6A 08 89 07 58 6A 07` | +5 = push 8 |
| main_menu_sprite_table | `0x48ce65` | `FF B7 B0 00 00 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 87 A4 00 00 00` | +7 &layout table |
| (cursor tables) | 5 matches | `FF 74 24 04 E8 ?? ?? ?? ?? C7 04 24 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 59 59 C3` | +12 table; ours starts (-200, 184) |
| play_sound | `0x4aeede` | `55 8B EC 83 EC 0C 57 E8 ?? ?? ?? ?? 33 FF 39 78 08 75 07 33 C0 E9` | callable (id) |
| place_window | `0x4afa93` | `55 8B EC 83 EC 18 56 33 F6 39 35 ?? ?? ?? ?? 57 0F 85 ?? ?? ?? ?? 53 FF 75 0C 8D 45 E8 FF 75 08 56 56 50 FF 15 ?? ?? ?? ?? 68 00 02 00 00 56 68 00 00 CA 00` | hook; +0xC0 push [hwnd] |
| binds_rebuild | `0x4b2945` | `55 8B EC 83 EC 0C 83 65 FC 00 53 56 8B 75 08 C1 E6 08 57 C7 45 F8 ?? ?? ?? ?? 81 C6 ?? ?? ?? ?? FF 75 FC` | callable (set) |
| settings_save | `0x4b3607` | `55 8B EC 81 EC A4 03 00 00 53 56 57 C7 85 5C FC FF FF 02 00 00 00 33 DB` | callable () |
| pad_index_getter | `0x4b27d2` | `8B 44 24 04 8B 04 85 ?? ?? ?? ?? C3 8B 44 24 08 8B 4C 24 04 89 04 8D` | +7 &pad index[2] (`0x69f7f0`) |
| gamepad_poll | `0x4b0b30` | `83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 57 8B 3D ?? ?? ?? ?? 6A 02 58 48 8B 7F 1C 75 FA` | +2 &joystick count, +16 &device list head |
| mwPlyCreateSofdec | `0x53d8b0` | `83 EC 18 53 8B 5C 24 20 55 56 85 DB 57 75 ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 33 C0 5F 5E 5D 5B 83 C4 18 C3 53 E8` | hook |
| sound_object_ptr | `0x4af356` | `A1 ?? ?? ?? ?? D9 E8 D9 58 24 6A 04 59 D9 E8` | +1 &ptr to sound object (`0x5EEEE4`) |
| volume_db_table | `0x4ae2cd` | `8B 44 24 04 85 C0 7D 02 33 C0 83 F8 7F 7E 03 6A 7F 58 8B 04 85 ?? ?? ?? ?? C3` | +21 table int32[128] (`0x5EECE0`) |

More sound patterns (not used by code yet, from the 0.7.0 analysis, all unique): config pointer
`0x60EAE4` via `56 8B 35 ?? ?? ?? ?? 57 8B 7C 24 0C 81 C6 E8 10 00 00 A5 A5 A5 A5` (+3); movie task
pointer `0x837B00` via `A1 ?? ?? ?? ?? 8B 48 28 FF B0 B0 00 00 00` (+1); per-frame movie volume
restore `0x49047E` = `83 3D ?? ?? ?? ?? 00 74 2B A1 ?? ?? ?? ?? 8B 48 28 FF B0 B0 00 00 00 8B 09 FF 70 28 FF 51 2C`;
`ADXT_SetOutVol` `0x530650` = `56 8B 74 24 08 85 F6 75 0F 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 5E C3 8B 4E 04 66 8B 44 24 0C 51 66 89 46 40`.

### Menus, option pages and the key table (tmntnet 0.7.2, verified unique; re/notes.md §15-17)

| name | address | pattern | use |
|---|---|---|---|
| draw_sprite | `0x4abab2` | `55 8D 6C 24 8C 81 EC 88 00 00 00 8B 45 7C 83 78 38 00 0F 84` | hook (quad): picture swap for our rows |
| main_menu_draw | `0x48ccdf` | `56 8B 74 24 08 8D 46 68 50 E8 ?? ?? ?? ?? FF B6 B0 00 00 00 FF B6 A4 00 00 00 E8` | hook: pages over the main menu |
| pad_slots_update | `0x4a984d` | `55 8B EC 83 EC 0C 53 56 57 E8 ?? ?? ?? ?? 83 65 F4 00 BE ?? ?? ?? ?? 8D 7E F4` | hook: buttons for the pages |
| text_in_box | `0x4a72cb` | `55 8B EC A1 ?? ?? ?? ?? 56 FF 75 08 FF 90 20 01 00 00 05 80 00 00 00 50 E8` | hook: 4:3 viewport |
| other_init / update / draw | `0x48e48d` / `0x48e616` / `0x48ea20` | `56 8B 74 24 08 57 68 7C 01 00 00 33 FF` / `83 EC 70 53 55 56 8B B4 24 80 00 00 00` / `56 8B 74 24 08 6A 01 FF B6 78 01 00 00` | hooks (ПРОЧИЕ ПАРАМЕТРЫ) |
| other_menu_count | `0x48e4a5` | `68 07 60 00 00 57 6A 05` | +7 push 5 |
| other_positions | `0x48e32a` | `C7 04 24 10 1B 59 00 56` | +3 &cursor positions (`0x591b10`) |
| other_labels | `0x48e4d5` | `83 C4 20 D9 1C 24 6A 05 68 ?? ?? ?? ??` | +9 &labels table (`0x591a98`) |
| sound_init / update / draw | `0x48fdd4` / `0x48ff23` / `0x49023d` | `56 8B 74 24 08 57 68 E4 00 00 00 33 FF` / `53 8B 5C 24 08 55 56 8D AB BC 00 00 00` / `56 8B 74 24 08 6A 01 FF B6 E0 00 00 00` | hooks (ОПЦИИ ЗВУКА) |
| sound_menu_count | `0x48fe09` | `68 07 60 00 00 57 6A 06` | +7 push 6 |
| sound_positions | `0x48fcbb` | `8B F0 C7 04 24 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 68 ?? ?? ?? ??` | +5 &cursor positions (`0x5928a8`) |
| sound_labels | `0x48fe28` | `83 C4 24 D9 1C 24 6A 06 68 ?? ?? ?? ??` | +9 &labels table (`0x592818`) |
| disp_init / update / draw | `0x48db54` / `0x48dc86` / `0x48df5c` | `56 8B 74 24 08 57 68 70 01 00 00 33 FF` / `53 55 56 8B 74 24 10 8B 86 5C 01 00 00` / `55 8B EC 51 56 8B 75 08 57 8D BE 68 01 00 00` | hooks (ПАРАМЕТРЫ ГРАФИКИ) |
| disp_menu_count | `0x48db92` | `6A 03 6A 03 89 86 58 01 00 00` | +1 push 3 |
| disp_positions | `0x48da7c` | `C7 04 24 9C 18 59 00 56` | +3 &cursor positions (`0x59189c`) |
| disp_labels | `0x48dba9` | `6A 05 68 ?? ?? ?? ?? 89 46 14` | +3 &labels table (`0x5917a0`) |
| disp_shadow_values | `0x48dbc0` | `6A 02 68 ?? ?? ?? ?? 89 86 68 01 00 00` | +3 &ТЕНИ values (`0x591868`) |
| disp_shadow_arrows | `0x48daa3` | `E8 ?? ?? ?? ?? 68 C4 18 59 00` | +6 &ТЕНИ arrows position (`0x5918c4`) |
| disp_name_call | `0x48dffe` | `D9 1C 24 50 E8 ?? ?? ?? ?? 83 C4 10 5F` | +4 call of font_draw (`0x4a7296`), redirected |
| controls_init / update / draw | `0x48e0aa` / `0x48e178` / `0x48e2d8` | `56 8B 74 24 08 6A 68 6A 00 56 E8` / `56 8B 74 24 08 57 33 FF 39 7E 08 0F 84 31 01 00 00 FF 76 54 E8` / `8B 44 24 04 6A 03 FF 70 64 E8 ?? ?? ?? ?? 59 59 C3 55 8B EC 83 EC 10` | hooks (ПАРАМЕТРЫ УПРАВЛЕНИЯ, the small page) |
| controls_menu_count | `0x48e0dd` | `6A 00 F7 D1 23 C8 6A 03 6A 03 89 46 60` | +7 push 3 |
| controls_positions | `0x48e02b` | `C7 04 24 10 1A 59 00 50 E8` | +3 &cursor positions (`0x591a10`) |
| dialog_answer | `0x4127db` | `8B 44 24 04 8B 40 34 8B 40 08 C3 68 55 01 00 00` | callable (dialog task) |
| keyconf_update | `0x4b239c` | `A1 ?? ?? ?? ?? 8B 48 04 83 F9 07 57 8B 3D ?? ?? ?? ?? 0F 87 ?? ?? ?? ?? 56 33 F6 46 FF 24 8D` | hook; +1 &task data (`0x69f584`), +14 &aux (`0x69f580`) |
| keyconf_draw_names | `0x4b1946` | `55 8B EC 83 EC 30 8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? 53 33 DB 39 19 89 45 FC 56 C7 45 D8 00 80 8C 43 C7 45 DC 00 80 9B 43 C7 45 E0 00 00 98 42 C7 45 E4` | hook |
| keyconf_init / term | `0x4b1c90` / `0x4b12a2` | `55 8B EC 83 EC 24 56 57 33 F6 56 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 6A 2C FF 90 34 01 00 00 6A 2C 56 50 A3` / `A1 ?? ?? ?? ?? 83 38 00 75 08 6A 00 E8 ?? ?? ?? ?? 59 FF 35 ?? ?? ?? ?? A1 ?? ?? ?? ?? FF 90 38 01 00 00` | hooks |
| keyconf_rows | `0x4b1e89` | `6A 0C 5E 99 F7 FE 51 FF 34 D5 ?? ?? ?? ?? 53 E8` | +10 &rows {slot, y} (`0x5bdbc8`) |
| keyconf_names | `0x4b19bc` | `FF 34 8D ?? ?? ?? ?? EB 05 68 ?? ?? ?? ?? 50 E8` | +3 &names (`0x5f43c0`), +10 "none" |
| keyconf_allowed | `0x4b1e79` | `38 99 ?? ?? ?? ?? 74 2F A1` | +2 &allowed[256] (`0x5bd918`, .rdata): 0xF0..0xF4 set to 1 (mouse keys) |

### The game on every PC (tmntnet 0.8.0, verified unique; re/notes.md §18)

| Name | Address | Pattern | Use |
|---|---|---|---|
| crt_rand / crt_srand | `0x565dbd` / `0x565db0` | `E8 ?? ?? ?? ?? 8B 48 14 69 C9 FD 43 03 00 81 C1 C3 9E 26 00 89 48 14` / `E8 ?? ?? ?? ?? 8B 4C 24 04 89 48 14 C3` | srand at the common start; call target = _getptd (state +0x14) |
| single_instance | `0x4b0109` | `BE ?? ?? ?? ?? 56 33 FF 57 57 89 85 AC 00 00 00 FF 15` | +1 &"TMNT_WIN32" (mutex and window class), lab only |
| loader_read_stat | `0x4a6d76` | `FF B7 84 2F 00 00 C1 E6 04 81 C6 ?? ?? ?? ?? E8` | +15 call ADXF_GetStat, redirected (reads end in their frame) |
| sound_bank_done / sound_bank_step | `0x4ae594` / `0x4aeaa6` | `33 C0 39 05 ?? ?? ?? ?? 0F 94 C0 C3` / `A1 ?? ?? ?? ?? 48 56 0F 84 ?? ?? ?? ?? 48 0F 84 ?? ?? ?? ?? 48 0F 84` | +4 &state; the step, pumped at frame end |
| cri_tick_run | `0x4af69d` | `E9 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? E9` | CRI tick wait; +20 CRI run |
| sound_update | `0x4af0f0` | `E8 ?? ?? ?? ?? 83 78 08 00 74 0F E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 C0 C3` | call target = sound struct getter (+8 up) |
| frame_spin | `0x4b0068` | `39 7D EC 73 0F E8 ?? ?? ?? ?? 3B D6 7C F7 7F 04 3B C7 72 F1` | +5 the frame wait's counter read ([debug] light) |
| player_masks | `0x4b2a2f` | `55 8B EC 83 EC 0C 53 56 57 E8 ?? ?? ?? ?? 6A 06 33 C0 59 BF ?? ?? ?? ?? F3 AB` | hook; +20 &masks (`0x69f590`), sticks 8 before |
| save_serialize / save_deserialize | `0x4aa585` / `0x4aa5d1` | `56 8B 74 24 08 8D 46 08 57 50 C6 06 03 E8 ?? ?? ?? ?? 8D 86 30 01 00 00` / `56 8B 74 24 08 80 3E 03 74 05 33 C0 40 5E C3 57 8D BE 60 11 00 00` | the host's progress for the guest |
| main_loop_running | `0x4ab20f` | `33 C0 39 05 ?? ?? ?? ?? 0F 95 C0 C3` | hook: its first 0 = the way out (give back before the settings save) |
| text_width | `0x4a7209` | `D9 44 24 08 51 D9 1C 24 FF 74 24 08 FF 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C C3` | callable (text, height) |
| sprite_set_visible | `0x49ddff` | `8A 44 24 08 8B 4C 24 04 88 41 3C C3` | callable (sprite, visible); not used since 0.7.3 |

### Who plays whom (tmntnet 0.9.0, verified unique; re/notes.md §19)

| Name | Address | Pattern | Use |
|---|---|---|---|
| player_character | `0x404740` | `8B 44 24 04 8B 0D ?? ?? ?? ?? 8D 44 80 19 8B 04 C1 C3` | +6 &info (`0x60a438`): character +0xC8+p*0x28, turtle +0x44+p*0x44 |
| map_load | `0x4a05f1` | `6A 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C8 08 50 E8 ?? ?? ?? ?? FF 74 24 0C E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 00 E8` | hook; +0x30 call = load a character (`0x493c92`) |
| map_free | `0x4a062a` | `6A 00 E8 ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? FF 74 24 10 E8` | hook; +0xF call = free a character (`0x493cda`) |
| map_turtle | `0x4a07b0` | `55 8B EC 83 EC 0C 56 D9 EE 8B 75 0C D9 5D F8 8B 06 D9 EE A3` | hook; checked inside: +0x13/+0x39 mov target x/z, +0xBB mov turtle, +0xD4 push brain, +0xDA mov pad, makers' calls at +0x63..+0xB3 |
| world_camera | `0x4a25a5` | `B8 ?? ?? ?? ?? C3 A1 ?? ?? ?? ?? C3` | +1 &holder (`0x61ced8`): the RwCamera the world is drawn with |
| camera_begin_update | (`[camera + 0x18]`) | none: read from the camera at run time | hook: the world camera began a picture |

### Four players, down not out, the story's menu (tmntnet 0.10, verified unique; re/notes.md §20-21)

| Name | Address | Pattern | Use |
|---|---|---|---|
| turtle_lying | `0x419256` | `55 8B EC 56 8B 75 08 56 E8 ?? ?? ?? ?? 83 F8 01 59 75 21 FF 75 0C 56 E8 ?? ?? ?? ?? 85 C0 59 59 6A 00 74 04 6A 1D EB 02 6A 16` | hook: a turtle's lying ends in 0x1D (dead) or 0x16; +0x17 call = is dead (`0x41921d`) |
| turtle_hit_ignored | `0x432e20` | `FF 74 24 04 E8 ?? ?? ?? ?? 85 C0 59 74 04 33 C0 40 C3 FF 74 24 0C FF 74 24 0C E8` | hook: 1 = the hit does not hurt (a stunned turtle) |
| hud_hearts | `0x406d46` | `55 8B EC 51 B8 ?? ?? ?? ?? 53 50 A3 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? C1 E0 04 05 ?? ?? ?? ?? C7 05` | two matches (the enemies' count `0x406e0c`): the hearts' one calls `0x40427a` at +0x45; +5 heart sprite, +0x32 number sprite |
| story_menu_init | `0x48d1ad` | `55 8B EC 53 56 8B 75 08 57 68 88 00 00 00 33 FF 57 56 E8` | hook; calls at +0xDE (menu object), +0xE9 (cursor), +0x124 (sprites); +0x107 the story's table |
| story_menu_term | `0x48d0e8` | `56 8B 74 24 08 FF 76 68 FF 76 7C E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? FF 76 14 E8` | hook; calls at +0xB (free sprites), +0x19 (free the menu object) |
| story_menu_update | `0x48d2e8` | `56 8B 74 24 08 83 7E 6C 00 0F 85 ?? ?? ?? ?? 83 7E 08 00 0F 84 ?? ?? ?? ?? 83 7E 74 00 0F 84` | hook; +0x26 call = parent busy (`0x48e09c`) |
| (cursor table) | `0x48d05c` | main_menu.cpp's `kCursorSig` (5 matches) | the one whose table is (-170, 126, -170, 87, -170, 47, -170, 7): its +12 imm repointed to a 6-row copy |
| p_set_74 | `0x404444` | `8B 44 24 04 8B 4C 24 08 6B C0 44 8B 15 ?? ?? ?? ?? 89 4C 10 74 C3` | hook: the best combo of players 3 and 4 |
| object_remove | `0x4a91a1` | `8B 44 24 04 C7 40 04 02 00 00 00 C3` | lab: debug_kill; hooked only with `[debug] actions=1` (who removes a turtle) |
| p_is_player | `0x4186e7` | `56 33 F6 E8 ?? ?? ?? ?? 85 C0 7E 19 56 E8 ?? ?? ?? ?? 59 8B 4C 24 08 39 01 74 0E 46 E8 ?? ?? ?? ?? 3B F0 7C E7 33 C0 5E` | hook: players 3 and 4's characters are players' too (hits between turtles and enemies) |
| (free_players) | `0x493ec0` | load_players + 0x30 | not hooked (loops into its first bytes): its callers redirected (patch::redirect_calls) |
| slot_rumble | `0x4a9ab6` | `8B 44 24 08 56 8B 74 24 08 69 F6 AC 00 00 00 81 C6 ?? ?? ?? ?? 89 86 A4 00 00 00 50` | hook: no rumble for slots 2 and 3 |
| slot_rumble_for | `0x4a9af0` | `D9 44 24 0C 56 8B 74 24 08 D8 15 ?? ?? ?? ?? 69 F6 AC 00 00 00 DF E0 81 C6` | hook: no rumble for slots 2 and 3 |
| turtle_volumes | `0x418618` | `55 8B EC 83 EC 14 56 8B 75 08 8B 46 60 8B 80 54 01 00 00 83 78 10 1D 89 45 08 74` | hook: a stunned turtle puts no volumes up (enemies leave it) |

### The team's progress (tmntnet 0.16.0, verified unique; re/notes.md §37)

| Name | Address | Pattern | Use |
|---|---|---|---|
| dojo_prize_1 | `0x4109bf` | `55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 83 4D D4 02 8D 45 D0` | hook: the first dojo's prize (record flag 2) for a character |
| dojo_prize_2 | `0x4109e6` | `55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 83 4D D4 04 8D 45 D0` | hook: the second dojo's (flag 4) |
| dojo_prize_3 | `0x410a0d` | `55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 83 4D D4 08 8D 45 D0` | hook: the third dojo's (flag 8) |
| furthest_stage | `0x410bae` | `55 8B EC 83 EC 30 8D 45 D0 50 FF 75 08 E8 ?? ?? ?? ?? 8B 45 0C 39 45 F0` | hook: (character, stage): record +0x20, only up |
| stage_opens | `0x40f939` | `53 56 57 33 F6 33 FF E8 ?? ?? ?? ?? 33 DB 43 83 F8 18` | hook: what the stage just played opens for player 1's character; returns the notices' bits |
| solo_stage | `0x40498a` | `A1 ?? ?? ?? ?? 8B 80 3C 01 00 00 83 F8 06 74 0D 83 F8 13` | called: the stage is 6, 19 or 30 (the dojo) |
| config_ptr | `0x410b6b` | `A1 ?? ?? ?? ?? 8B 80 54 11 00 00 C3` | +1 &holder (`0x60eae4`): the progress object, the turtles' records at its start (for the log) |
| dojo_passed | `0x401198` | `33 C0 40 50 A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 00 FF 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C E8` | lab (`dojo_pass_at`): the dojo's judge's "passed" |

### The ending ended before its own end (tmntnet 0.16.0, verified unique; re/notes.md §38)

| Name | Address | Pattern | Use |
|---|---|---|---|
| ending_destroy | `0x49a88a` | `56 E8 ?? ?? ?? ?? 8B 74 24 08 83 7E 28 00 74 09 E8 ?? ?? ?? ?? 83 66 28 00 5E C3` | hook: the "destroy" of the turtles' ending scene (data: +0 state, +8 view, +0xC / +0x10 / +0x14 cutscenes, +0x18 staff roll, +0x1C stage, +0x20 last picture) |
| ending_movie_destroy | `0x49a8a5` | `E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 59 C3` | hook: the "destroy" of the ending scene with a movie first (+8, +0x18, +0x20 only) |
| staff_roll_stop | `0x49b3b9` | `83 25 ?? ?? ?? ?? 00 C7 05 ?? ?? ?? ?? 00 00 E0 43 E9 ?? ?? ?? ??` | called (task): the roll's lines draw no more (`[0x5e6e80]` = 0) and leave at 448 a frame, the roll's task is ended |
| staff_roll_line_step | `0x49b5e9` | `55 8B EC 83 EC 10 53 8B 5D 08 D9 43 40 D8 1D ?? ?? ?? ?? DF E0 F6 C4 41 75 0C E8 ?? ?? ?? ?? 50 E8` | address only: the tasks with this step are the roll's lines |
| cutscene_actor_step | `0x44f4e3` | `55 8B EC 83 EC 0C 56 E8 ?? ?? ?? ?? 8D 4D FC 51 8D 4D F8 51 50 89 45 F4 E8 ?? ?? ?? ?? 8B 75 08 83 C4 0C 85 C0 74 7C 53` | address only: the tasks with this step are cutscenes' actors (data +0x18 the cutscene's task) |
| task_end | `0x4ac4b5` | `E8 ?? ?? ?? ?? 85 C0 74 11 6A 00 6A 00 FF 74 24 0C E8 ?? ?? ?? ?? 83 C4 0C C3 56 8B 74 24 08 E8 ?? ?? ?? ?? 5E C3` | called (task): ends a task, at once outside the tasks' passes, else by a message at the frame's end |
| view_2d_free | `0x4acff8` | `56 8B 74 24 08 85 F6 74 32 57 6A 00 E8 ?? ?? ?? ?? 6A 00 6A 00 E8 ?? ?? ?? ?? 56 E8` | called (view): lets a 2D view made by `0x4acfc9` go |
| task_list | `0x4aca5f` | `56 8B 35 ?? ?? ?? ?? EB 36 8B 06 83 78 10 03 74 2B` | +3 &head (`0x646d3c`), as lockstep.cpp: a node +0 task, +8 next; a task +0x1C its step |