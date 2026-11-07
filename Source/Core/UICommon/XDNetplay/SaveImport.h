// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include "UICommon/XDNetplay/Gen3Save.h"

// Opt-in import of a user's own Gen 3 save into a GBA socket, replacing the
// bundled team-editor save. Guarantees, in order of importance:
//  - the file the user picked is only ever READ. (That is a statement about
//    the LOCAL file only. What a hosted netplay session shares is governed by
//    DisposableSave: the imported socket save itself never enters the synced
//    save path -- a rebuilt party-and-identity save stands in for it, and the
//    user-facing wording everywhere says so.);
//  - whatever occupied the socket is backed up ONCE to <save>.preimport, a
//    name no cleanup path (TeamInjector's purge included) ever deletes, and a
//    re-import never overwrites that first backup;
//  - nothing lands in the socket unless the image validated in memory first
//    (size, structure, checksums, and game-vs-ROM match).
// Mirrored 1:1 on Android in features/xdnetplay/gen3/SaveImport.kt.
namespace XDNetplay::SaveImport
{
// Backup of whatever occupied a GBA socket before the FIRST user import.
// PROVABLY outside TeamInjector's cleanup set (.hostteam / .guestteam / .bak /
// .tmp / NetPlayTemp*.sav -- TeamInjector.cpp's PurgeNow): no code path in the
// tree deletes a .preimport file, so the pre-import world survives every
// session, purge, and re-import.
constexpr const char* PREIMPORT_SUFFIX = ".preimport";

// What ValidateGen3SaveImage learned about an accepted image.
struct SaveValidation
{
  Gen3Game game = Gen3Game::Emerald;
  // Byte count of the file as picked, before any padding.
  size_t original_size = 0;
  // A 65536-byte single-bank file was 0xFF-padded (flash erase state) up to
  // the full 131072; block 0 carries the whole save in that case.
  bool padded_from_64k = false;
  // Always 0 on an accepted image -- any mismatch refuses it. Kept so the
  // struct mirrors the Kotlin port field-for-field.
  int bad_checksums = 0;
};

// Pure, no I/O. Size gate (exactly 65536, or >= 131072 with any trailing
// emulator footer preserved) -> optional 0xFF pad to 131072 -> structure parse
// -> game detection -> game-aware checksum verification. On success *bytes
// holds exactly the image an import may write; on failure *bytes is
// unspecified and *error (if given) holds a user-displayable reason.
std::optional<SaveValidation> ValidateGen3SaveImage(std::vector<u8>* bytes,
                                                    std::string* error = nullptr);

// The Gen 3 game a GBA ROM is, from the 4-character game code at header
// 0xAC.. 0xAF ("BPE*" Emerald, "AXV*"/"AXP*" Ruby/Sapphire, "BPR*"/"BPG*"
// FireRed/LeafGreen; the fourth letter is only the language). std::nullopt
// (with *error) for unreadable files and non-Pokemon ROMs -- the launcher's
// manual picker accepts any GBA ROM, so this cannot assume Emerald.
std::optional<Gen3Game> DetectRomGame(const std::string& rom_path, std::string* error = nullptr);

// The whole no-destruction import flow for GBA socket `device` (1 = port 2,
// the user's side; 2 = port 3, the guest slot): resolve the socket's save
// path, refuse while a room or emulation could clobber the result, validate
// the source in memory, back up the current save once, then tmp-write /
// read back / rename the validated image into place and record the source
// filename in the per-port config key. The source file is never written.
// *status gets a user-displayable success summary.
bool ImportUserSave(const std::string& source_path, int device, std::string* status,
                    std::string* error);

// Team-only import for a partial cartridge read (GB-Link): |partial_path| holds a Gen 3 image
// in which only the footers and logical sections 0 and 1 are real. The trainer identity and
// party are extracted, written into the bundled template for socket |device| (the same
// construction a netplay host uses), saved to |built_path|, and imported with ImportUserSave.
// Team sharing. TeamTextFromSave reads socket |device|'s current save and returns its team as the
// base64 text of a PartyBundle (what the OrreLink Hub stores under a team code). ImportTeamText is
// the reverse: validate a team text (plain or "SaveBundle:"-prefixed), build the socket save from
// the bundled template, write it to |built_path| and import it like any other save.
bool TeamTextFromSave(int device, std::string* team_text, std::string* summary, std::string* error);
bool ImportTeamText(const std::string& team_text, const std::string& built_path, int device,
                    std::string* status, std::string* error);

bool ImportTeamFromPartialSave(const std::string& partial_path, const std::string& built_path,
                               int device, std::string* status, std::string* error);

// Put the bundled team-editor template back into socket `device`'s save and
// clear the per-port config key. The replaced file is kept as <save>.bak
// (one-level undo, same as every verified write); <save>.preimport is left
// untouched forever.
bool RestoreDefaultSave(int device, std::string* error);

// The GBA player model (BattleCustomizer's ids 0x06 Leaf, 0x07 Red, 0x08 May R/S, 0x09 Brendan R/S)
// that matches the cartridge whose team was read into socket `device`, or 0 when that team did not
// come from a FireRed/LeafGreen or Ruby/Sapphire cartridge (an Emerald trainer is already the
// game default). The netplay save is always an Emerald rebuild, so this is what lets a LeafGreen
// player appear as Leaf instead of May.
int CartridgeModelForDevice(int device);

// True when <save>.preimport exists for the socket. This -- not the config
// key, which is display-only -- is the authoritative "an import happened"
// signal, so a hand-deleted file cannot desync the UI.
bool HasImportBackup(int device);
}  // namespace XDNetplay::SaveImport
