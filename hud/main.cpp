// Bono DMC HUD - SE/AE port (CommonLibSSE-NG, Address Library).
//
// Re-implementation of BonoDMCHUD.dll v1.1 (built for SE 1.5.97 with hard-coded offsets).
// Behaviour is taken from BonoDMCHUD.ini; the Flash paths are the ones the original uses
// (strings in the original DLL) and were checked against BonoDMCHUD.swf / BonoDMCBoss.swf:
//
//   BonoDMCHUD.swf  -> _root.bdmc_hud
//     row0 / row1         one row of 10 health cells (row0 at y -59, row1 at y -45)
//       c0..c9            cell frames (_visible = cell exists for this max health)
//       cm                mask (cell shapes) over tr + fl
//       tr                white damage trail, 617 px wide        -> _xscale
//       fl                health fill, 617 px wide               -> _xscale
//         low             30-frame red pulse loop                 -> _visible
//         fx              13-frame heal flash (stopped on f1)     -> gotoAndPlay(2)
//     mp.m / sp.m         magicka / stamina mask, 612 px wide     -> _x = (fraction - 1) * 612
//
//   BonoDMCBoss.swf -> _root.bdmc_boss
//     b0 / b1 / b2        boss bars (b0 bottom, b1 at -72, b2 at -144)
//       nm                name text field
//       fm / tm           masks over fill / trail, 933.6 px centred -> _x = (fraction - 1) * 933.6
//       tip               marker at the fill edge                    -> _x = -466.8 + fraction * 933.6
//
// All Flash work happens in the HUD menu's own AdvanceMovie (per rendered frame, also while the game is
// paused) so the HUD hides together with the vanilla HUD when menus open. This plugin only reads game values.

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "Support.h"

using namespace std::literals;

namespace
{
	constexpr const char* kIniPath = "Data\\SKSE\\Plugins\\BonoDMCHUD.ini";
	constexpr const char* kLogPath = "Data\\SKSE\\Plugins\\BonoDMCHUD.log";

	constexpr double kHudDepth = 5232.0;
	constexpr double kBossDepth = 5233.0;

	constexpr float kRowWidth = 617.0f;   // fl / tr width in a health row
	constexpr float kCellStep = 62.0f;    // c0..c9 spacing
	constexpr float kCellWidth = 59.0f;   // one cell
	constexpr float kBarWidth = 612.0f;   // magicka / stamina mask width
	constexpr float kBossWidth = 933.6f;  // boss masks
	constexpr int   kMaxBossSlots = 3;

	constexpr std::array<const char*, 2> kHudPaths{ "../BonoDMCHUD.swf", "BonoDMCHUD.swf" };
	constexpr std::array<const char*, 2> kBossPaths{ "../BonoDMCBoss.swf", "BonoDMCBoss.swf" };
	constexpr std::array<const char*, 2> kGauges{ "_root.bdg_gauge", "_root.bvg_gauge" };  // Dante / Vergil DT gauge (other mod)

	// ------------------------------------------------------------------ config
	struct Config
	{
		float hudX{ 38.67f }, hudY{ 87.33f }, hudScale{ 66.667f };
		float hpPerCell{ 50 };
		int   maxRows{ 2 };
		float trailHold{ 0.6f }, trailCellsPerSec{ 8 };
		float lowPercent{ 25 };
		float healFlashMin{ 1 };
		bool  hideVanillaBars{ true };
		bool  followVanilla{ true };

		bool  autoHide{ true };
		float autoHideDelay{ 3 }, fadeIn{ 0.15f }, fadeOut{ 0.6f };
		float combatRange{ 4000 };
		bool  autoHideGauges{ true };

		bool  bossBars{ true };
		int   maxBossBars{ 3 };
		float bossMinHealth{ 5000 };
		bool  uniqueBoss{ true };
		float uniqueBossHealthMult{ 3 };
		bool  useTrueHUDLists{ true };
		float bossRange{ 4000 };
		float bossDropTime{ 6 };
		float bossRiseRate{ 0.5f };
		float bossTrailHold{ 0.6f };   // new (not in Bono's INI): how long the boss damage trail stays
		float bossTrailRate{ 0.5f };   // new: boss trail drain speed, fraction of the bar per second
		float bossX{ 640 }, bossY{ 673.3f }, bossScale{ 66.667f };

		bono::Ini ini;  // kept: the own NPC= / Race= ... entries are read after data load

		void Load()
		{
			bool ok = ini.Load(kIniPath);
			bono::Log::Get().Init(kLogPath, ok && ini.GetBool("LogEnabled", false), ini.GetInt("MaxLogLines", 3000));
			if (!ok) {
				bono::Logf("INI not found ({}), using defaults", kIniPath);
				return;
			}
			hudX = ini.GetFloat("HudX", hudX);
			hudY = ini.GetFloat("HudY", hudY);
			hudScale = ini.GetFloat("HudScale", hudScale);
			hpPerCell = std::max(1.0f, ini.GetFloat("HPPerCell", hpPerCell));
			maxRows = std::clamp(ini.GetInt("MaxRows", maxRows), 1, 2);  // the SWF has two rows
			trailHold = ini.GetFloat("TrailHold", trailHold);
			trailCellsPerSec = std::max(0.1f, ini.GetFloat("TrailCellsPerSec", trailCellsPerSec));
			lowPercent = ini.GetFloat("LowPercent", lowPercent);
			healFlashMin = ini.GetFloat("HealFlashMin", healFlashMin);
			hideVanillaBars = ini.GetBool("HideVanillaBars", hideVanillaBars);
			followVanilla = ini.GetBool("FollowVanillaVisibility", followVanilla);

			autoHide = ini.GetBool("AutoHide", autoHide);
			autoHideDelay = ini.GetFloat("AutoHideDelay", autoHideDelay);
			fadeIn = ini.GetFloat("AutoHideFadeIn", fadeIn);
			fadeOut = ini.GetFloat("AutoHideFadeOut", fadeOut);
			combatRange = ini.GetFloat("AutoHideCombatRange", combatRange);
			autoHideGauges = ini.GetBool("AutoHideGauges", autoHideGauges);

			bossBars = ini.GetBool("BossBars", bossBars);
			maxBossBars = std::clamp(ini.GetInt("MaxBossBars", maxBossBars), 1, kMaxBossSlots);
			bossMinHealth = ini.GetFloat("BossMinHealth", bossMinHealth);
			uniqueBoss = ini.GetBool("UniqueBoss", uniqueBoss);
			uniqueBossHealthMult = ini.GetFloat("UniqueBossHealthMult", uniqueBossHealthMult);
			useTrueHUDLists = ini.GetBool("UseTrueHUDBossLists", useTrueHUDLists);
			bossRange = ini.GetFloat("BossRange", bossRange);
			bossDropTime = ini.GetFloat("BossDropTime", bossDropTime);
			bossRiseRate = std::max(0.01f, ini.GetFloat("BossRiseRate", bossRiseRate));
			bossTrailHold = ini.GetFloat("BossTrailHold", trailHold);
			bossTrailRate = std::max(0.01f, ini.GetFloat("BossTrailRate", bossTrailRate));
			bossX = ini.GetFloat("BossX", bossX);
			bossY = ini.GetFloat("BossY", bossY);
			bossScale = ini.GetFloat("BossScale", bossScale);

			bono::Logf("Bono DMC HUD v1.1 (SE/AE port)  AutoHide={} HudScale={} HPPerCell={} MaxRows={} TrailHold={} TrailCellsPerSec={} LowPercent={} HideVanillaBars={} FollowVanillaVisibility={}",
				autoHide, hudScale, hpPerCell, maxRows, trailHold, trailCellsPerSec, lowPercent, hideVanillaBars, followVanilla);
			bono::Logf("boss settings: BossBars={} MaxBossBars={} BossMinHealth={} UniqueBoss={} UseTrueHUDBossLists={} BossRange={}",
				bossBars, maxBossBars, bossMinHealth, uniqueBoss, useTrueHUDLists, bossRange);
		}
	};

	Config g_cfg;

	// ------------------------------------------------------------------ boss lists (TrueHUD / TDM format)
	struct BossLists
	{
		std::unordered_set<RE::TESRace*>            races;
		std::unordered_set<RE::TESActorBase*>       npcs;
		std::unordered_set<RE::TESActorBase*>       blacklist;
		std::unordered_set<RE::BGSLocationRefType*> locRefTypes;
		int                                         files{ 0 };
	};

	BossLists g_lists;

	// "Skyrim.esm:0001A684" (anything after a space is ignored, like TrueHUD) -> form
	template <class T>
	T* ParseForm(const std::string& a_entry)
	{
		auto split = a_entry.find(':');
		if (split == std::string::npos) return nullptr;
		auto mod = bono::Trim(a_entry.substr(0, split));
		auto rest = bono::Trim(a_entry.substr(split + 1));
		rest = rest.substr(0, rest.find(' '));
		if (mod.empty() || rest.empty()) return nullptr;
		char*         end = nullptr;
		unsigned long id = std::strtoul(rest.c_str(), &end, 16);
		if (end == rest.c_str()) return nullptr;
		auto* dh = RE::TESDataHandler::GetSingleton();
		return dh ? dh->LookupForm<T>(static_cast<RE::FormID>(id & 0xFFFFFF), mod) : nullptr;
	}

	void ApplyBossEntries(const bono::Ini& a_ini)
	{
		auto add = [&]<class T>(std::unordered_set<T*>& a_set, const char* a_key, const char* a_removeKey) {
			for (auto& e : a_ini.GetAll(a_key))
				if (auto* f = ParseForm<T>(e)) a_set.insert(f);
			for (auto& e : a_ini.GetAll(a_removeKey))
				if (auto* f = ParseForm<T>(e)) a_set.erase(f);
		};
		add(g_lists.races, "Race", "RemoveRace");
		add(g_lists.npcs, "NPC", "RemoveNPC");
		add(g_lists.locRefTypes, "LocRefType", "RemoveLocRefType");
		add(g_lists.blacklist, "NPCBlacklist", "RemoveNPCBlacklist");
	}

	void ReadBossFolder(const std::filesystem::path& a_dir, const std::string& a_baseName)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(a_dir, ec)) return;
		std::vector<std::filesystem::path> files;
		for (auto& de : std::filesystem::directory_iterator(a_dir, ec)) {
			if (!de.is_regular_file(ec)) continue;
			auto ext = bono::Lower(de.path().extension().string());
			if (ext == ".ini" && bono::Lower(de.path().filename().string()) != bono::Lower(a_baseName)) files.push_back(de.path());
		}
		std::sort(files.begin(), files.end());
		files.insert(files.begin(), a_dir / a_baseName);  // base file first, then the patches
		for (auto& p : files) {
			bono::Ini ini;
			if (!ini.Load(p.string())) continue;
			ApplyBossEntries(ini);
			++g_lists.files;
			bono::Logf("boss list file {}", p.string());
		}
	}

	void LoadBossLists()
	{
		g_lists = {};
		if (g_cfg.useTrueHUDLists) {
			ReadBossFolder("Data\\SKSE\\Plugins\\TrueHUD", "TrueHUD_base.ini");
			ReadBossFolder("Data\\SKSE\\Plugins\\TrueDirectionalMovement", "TrueDirectionalMovement_base.ini");
		}
		ApplyBossEntries(g_cfg.ini);  // own entries in BonoDMCHUD.ini last
		bono::Logf("boss lists: {} file(s), races {}, NPC list {}, blacklist {}, location ref types {}",
			g_lists.files, g_lists.races.size(), g_lists.npcs.size(), g_lists.blacklist.size(), g_lists.locRefTypes.size());
	}

	// ------------------------------------------------------------------ helpers
	float MaxAV(RE::Actor* a_actor, RE::ActorValue a_av)
	{
		auto* avo = a_actor->AsActorValueOwner();
		return avo->GetPermanentActorValue(a_av) + a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av);
	}

	float CurAV(RE::Actor* a_actor, RE::ActorValue a_av) { return a_actor->AsActorValueOwner()->GetActorValue(a_av); }

	float Frac(float a_cur, float a_max) { return a_max > 0.0f ? std::clamp(a_cur / a_max, 0.0f, 1.0f) : 0.0f; }

	// Writes a Flash number only when it changed (lots of paths are touched every frame).
	class Flash
	{
	public:
		void Reset() { _cache.clear(); }

		void Num(RE::GFxMovieView* v, const std::string& a_path, double a_val, double a_eps = 0.01)
		{
			auto it = _cache.find(a_path);
			if (it != _cache.end() && std::fabs(it->second - a_val) < a_eps) return;
			v->SetVariableDouble(a_path.c_str(), a_val);
			_cache[a_path] = a_val;
		}

	private:
		std::unordered_map<std::string, double> _cache;
	};

	void SetVisible(RE::GFxMovieView* v, const std::string& a_path, bool a_val)
	{
		v->SetVariable(a_path.c_str(), RE::GFxValue(a_val));
	}

	// ------------------------------------------------------------------ SWF loader (one per clip)
	struct ClipLoader
	{
		const char*                          clip;   // "bdmc_hud"
		const std::array<const char*, 2>*    paths;
		const char*                          probe;  // path that exists once the SWF is in
		double                               depth;
		const char*                          what;   // for the log
		int                                  state{ 0 };  // 0 = not requested, 1 = waiting, 2 = ready, 3 = gave up
		int                                  pathTry{ 0 };
		float                                waitT{ 0 };

		void Reset()
		{
			state = 0;
			pathTry = 0;
			waitT = 0;
		}

		void Request(RE::GFxMovieView* v)
		{
			const char*  path = (*paths)[static_cast<std::size_t>(pathTry) % paths->size()];
			RE::GFxValue cargs[2]{ RE::GFxValue(clip), RE::GFxValue(depth) };
			v->Invoke("_root.createEmptyMovieClip", nullptr, cargs, 2);
			RE::GFxValue larg(path);
			v->Invoke((std::string("_root.") + clip + ".loadMovie").c_str(), nullptr, &larg, 1);
			state = 1;
			waitT = 0;
			bono::Logf("HUD: HUD movie found, createEmptyMovieClip({}) + loadMovie({})", clip, path);
		}

		// true on the frame the SWF becomes ready
		bool Update(RE::GFxMovieView* v, float a_dt)
		{
			if (state == 0) {
				Request(v);
				return false;
			}
			if (state != 1) return false;
			waitT += a_dt;
			if (v->IsAvailable(probe)) {
				state = 2;
				bono::Logf("HUD: {} loaded from {}", what, (*paths)[static_cast<std::size_t>(pathTry) % paths->size()]);
				return true;
			}
			if (waitT > 3.0f) {
				++pathTry;
				if (pathTry >= static_cast<int>(paths->size())) {
					state = 3;
					bono::Logf("HUD: {} still not there -> not shown (check Interface\\{}.swf)", what, clip);
				} else {
					bono::Logf("HUD: {} not there after 3 s -> retry with next path", what);
					Request(v);
				}
			}
			return false;
		}

		bool Ready() const { return state == 2; }
	};

	ClipLoader g_hudClip{ "bdmc_hud", &kHudPaths, "_root.bdmc_hud.row0.fl", kHudDepth, "SWF" };
	ClipLoader g_bossClip{ "bdmc_boss", &kBossPaths, "_root.bdmc_boss.b0.fm", kBossDepth, "boss SWF" };

	// ------------------------------------------------------------------ state
	struct Boss
	{
		RE::ActorHandle handle;
		RE::FormID      id{ 0 };
		std::string     name;
		float           fill{ 1 };
		float           trail{ 1 };
		float           trailHold{ 0 };
		float           outOfCombat{ 0 };
		float           deadT{ -1 };
	};

	struct State
	{
		RE::GFxMovieView* view{ nullptr };
		Flash             flash;
		bool              listsLoaded{ false };

		// player values (last frame)
		bool  havePrev{ false };
		float hp{ 0 }, hpMax{ 0 }, mp{ 0 }, mpMax{ 0 }, sp{ 0 }, spMax{ 0 };
		float trailCells{ 0 }, trailHoldT{ 0 };
		bool  low{ false };
		int   shownCells{ -1 }, shownRows{ -1 };

		// visibility
		bool  vanillaShown{ true };
		bool  vanillaLogged{ false };
		float idleT{ 0 };
		float alpha{ 1 };  // auto-hide alpha 0..1
		bool  inCombat{ false };
		int   lastAlphaPct{ -1 };
		bool  lastHudVisible{ false };
		bool  autoShowLogged{ true };

		// DT gauges
		std::array<bool, 2>  gaugeFound{};
		std::array<double, 2> gaugeX{};
		std::array<int, 2>   gaugeAlphaPct{ -1, -1 };
		float                gaugeProbeT{ 99 };

		// scanning
		float scanT{ 99 };

		// bosses
		std::vector<Boss> bosses;
		bool              bossesDirty{ true };
		bool              lastBossVisible{ false };
	};

	State g_s;

	// ------------------------------------------------------------------ boss rules
	const char* BossReason(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
	{
		auto*               base = a_actor->GetActorBase();
		RE::TESActorBase*   orig = nullptr;
		if (auto* xlc = a_actor->extraList.GetByType<RE::ExtraLeveledCreature>()) orig = xlc->originalBase;

		if ((base && g_lists.blacklist.contains(base)) || (orig && g_lists.blacklist.contains(orig))) return nullptr;

		auto* race = a_actor->GetRace();
		if (race && race->HasKeywordString("ActorTypeDragon")) return "dragon";
		if (race && g_lists.races.contains(race)) return "race list";
		if ((base && g_lists.npcs.contains(base)) || (orig && g_lists.npcs.contains(orig))) return "NPC list";

		if (!g_lists.locRefTypes.empty()) {
			if (auto* loc = a_player->GetCurrentLocation()) {
				for (auto& ref : loc->specialRefs)
					if (ref.type && g_lists.locRefTypes.contains(ref.type) && ref.refData.refID == a_actor->GetFormID()) return "location boss";
			}
		}

		const float maxHp = MaxAV(a_actor, RE::ActorValue::kHealth);
		if (maxHp >= g_cfg.bossMinHealth) return "health";
		if (g_cfg.uniqueBoss && base && base->IsUnique() && maxHp >= MaxAV(a_player, RE::ActorValue::kHealth) * g_cfg.uniqueBossHealthMult) return "unique + health";
		return nullptr;
	}

	bool HasBoss(RE::FormID a_id)
	{
		return std::any_of(g_s.bosses.begin(), g_s.bosses.end(), [&](const Boss& b) { return b.id == a_id; });
	}

	// Runs 4x per second: combat detection for auto-hide + new bosses.
	void Scan(RE::PlayerCharacter* a_player)
	{
		auto* pl = RE::ProcessLists::GetSingleton();
		bool  combat = a_player->IsInCombat();
		if (!pl) {
			g_s.inCombat = combat;
			return;
		}
		const auto ppos = a_player->GetPosition();
		for (auto& h : pl->highActorHandles) {
			auto  sp = h.get();
			auto* a = sp.get();
			if (!a || a == a_player || a->IsDead() || a->IsDisabled()) continue;
			if (!a->IsInCombat() || !a->IsHostileToActor(a_player)) continue;
			const float dist = a->GetPosition().GetDistance(ppos);
			if (dist <= g_cfg.combatRange) combat = true;

			if (!g_cfg.bossBars || !g_bossClip.Ready()) continue;
			if (dist > g_cfg.bossRange || static_cast<int>(g_s.bosses.size()) >= g_cfg.maxBossBars) continue;
			if (HasBoss(a->GetFormID())) continue;
			if (const char* why = BossReason(a, a_player)) {
				Boss b;
				b.handle = a->GetHandle();
				b.id = a->GetFormID();
				const char* nm = a->GetDisplayFullName();
				b.name = nm ? nm : "";
				b.fill = b.trail = Frac(CurAV(a, RE::ActorValue::kHealth), MaxAV(a, RE::ActorValue::kHealth));
				g_s.bosses.push_back(std::move(b));
				g_s.bossesDirty = true;
				bono::Logf("boss found: {} ({:08X}) {} | max health {:.0f}", g_s.bosses.back().name, a->GetFormID(), why, MaxAV(a, RE::ActorValue::kHealth));
			}
		}
		g_s.inCombat = combat;
	}

	// ------------------------------------------------------------------ per frame: player bars
	// x position (in the 617 px row) where a fill of a_cells (0..10) ends
	float CellsToX(float a_cells)
	{
		a_cells = std::clamp(a_cells, 0.0f, 10.0f);
		const float k = std::floor(a_cells);
		return k * kCellStep + (a_cells - k) * kCellWidth;
	}

	void UpdatePlayerBars(RE::GFxMovieView* v, RE::PlayerCharacter* p, float a_dt, bool a_paused)
	{
		const float hp = std::max(0.0f, CurAV(p, RE::ActorValue::kHealth));
		const float hpMax = std::max(1.0f, MaxAV(p, RE::ActorValue::kHealth));
		const float mp = CurAV(p, RE::ActorValue::kMagicka), mpMax = MaxAV(p, RE::ActorValue::kMagicka);
		const float sp = CurAV(p, RE::ActorValue::kStamina), spMax = MaxAV(p, RE::ActorValue::kStamina);

		// cell layout: HPPerCell per cell, 10 per row, at most MaxRows rows; above that each cell holds more
		const int maxCells = 10 * g_cfg.maxRows;
		int       cells = static_cast<int>(std::ceil(hpMax / g_cfg.hpPerCell - 0.001f));
		float     perCell = g_cfg.hpPerCell;
		if (cells > maxCells) {
			cells = maxCells;
			perCell = hpMax / static_cast<float>(maxCells);
		}
		cells = std::max(cells, 1);
		const int rows = cells > 10 ? 2 : 1;

		if (cells != g_s.shownCells) {
			for (int r = 0; r < 2; ++r) {
				const int inRow = std::clamp(cells - 10 * r, 0, 10);
				for (int c = 0; c < 10; ++c)
					SetVisible(v, std::format("_root.bdmc_hud.row{}.c{}._visible", r, c), c < inRow);
			}
			bono::Logf("HUD: health max {:.0f} -> {} cell(s), {:.1f} HP per cell, {} row(s)", hpMax, cells, perCell, rows);
			g_s.shownCells = cells;
		}
		if (rows != g_s.shownRows) {
			SetVisible(v, "_root.bdmc_hud.row1._visible", rows == 2);
			g_s.flash.Num(v, "_root.bdmc_hud.row0._y", rows == 2 ? -59.0 : -45.0);  // one row sits right above magicka
			g_s.shownRows = rows;
		}

		const float hpCells = hp / perCell;

		if (!a_paused) {
			// damage trail: holds, then drains
			if (!g_s.havePrev || hpCells >= g_s.trailCells) {
				g_s.trailCells = hpCells;
				g_s.trailHoldT = 0;
			} else if (g_s.havePrev && hp < g_s.hp - 0.001f) {
				g_s.trailHoldT = g_cfg.trailHold;  // fresh hit: restart the hold
			} else if (g_s.trailHoldT > 0.0f) {
				g_s.trailHoldT -= a_dt;
			} else {
				g_s.trailCells = std::max(hpCells, g_s.trailCells - g_cfg.trailCellsPerSec * a_dt);
			}

			// heal flash (big jumps only, slow regen does not flash)
			if (g_s.havePrev && std::fabs(hpMax - g_s.hpMax) < 0.5f && hp - g_s.hp >= g_cfg.healFlashMin) {
				RE::GFxValue frame(2.0);
				for (int r = 0; r < rows; ++r)
					v->Invoke(std::format("_root.bdmc_hud.row{}.fl.fx.gotoAndPlay", r).c_str(), nullptr, &frame, 1);
				bono::Logf("HUD: heal flash +{:.1f}", hp - g_s.hp);
			}
		}

		for (int r = 0; r < 2; ++r) {
			const float base = 10.0f * static_cast<float>(r);
			g_s.flash.Num(v, std::format("_root.bdmc_hud.row{}.fl._xscale", r), 100.0 * CellsToX(hpCells - base) / kRowWidth);
			g_s.flash.Num(v, std::format("_root.bdmc_hud.row{}.tr._xscale", r), 100.0 * CellsToX(g_s.trailCells - base) / kRowWidth);
		}

		const bool low = hp > 0.0f && 100.0f * hp / hpMax <= g_cfg.lowPercent;
		if (low != g_s.low || !g_s.havePrev) {
			for (int r = 0; r < 2; ++r) SetVisible(v, std::format("_root.bdmc_hud.row{}.fl.low._visible", r), low);
			g_s.low = low;
		}

		g_s.flash.Num(v, "_root.bdmc_hud.mp.m._x", (Frac(mp, mpMax) - 1.0f) * kBarWidth, 0.05);
		g_s.flash.Num(v, "_root.bdmc_hud.sp.m._x", (Frac(sp, spMax) - 1.0f) * kBarWidth, 0.05);

		// anything moving keeps the HUD up (auto-hide)
		bool changed = !g_s.havePrev ||
		               std::fabs(hp - g_s.hp) > 0.001f || std::fabs(mp - g_s.mp) > 0.001f || std::fabs(sp - g_s.sp) > 0.001f ||
		               std::fabs(hpMax - g_s.hpMax) > 0.001f || std::fabs(mpMax - g_s.mpMax) > 0.001f || std::fabs(spMax - g_s.spMax) > 0.001f;

		// DT gauges from the Dante / Vergil gauge mod: they count as activity and fade with the HUD
		g_s.gaugeProbeT += a_dt;
		const bool probe = g_s.gaugeProbeT >= 1.0f;
		if (probe) g_s.gaugeProbeT = 0;
		for (std::size_t i = 0; i < kGauges.size(); ++i) {
			if (probe && !g_s.gaugeFound[i] && v->IsAvailable(kGauges[i])) {
				g_s.gaugeFound[i] = true;
				g_s.gaugeAlphaPct[i] = -1;
				bono::Logf("HUD: DT gauge clip {} found -> it fades with the HUD", kGauges[i]);
			}
			if (!g_s.gaugeFound[i]) continue;
			const std::string mask = std::string(kGauges[i]) + ".bmask._x";
			if (!v->IsAvailable(mask.c_str())) {
				g_s.gaugeFound[i] = false;
				continue;
			}
			const double x = v->GetVariableDouble(mask.c_str());
			if (std::fabs(x - g_s.gaugeX[i]) > 0.01) changed = true;
			g_s.gaugeX[i] = x;
		}

		if (!a_paused) {
			if (changed) g_s.idleT = 0;
			else g_s.idleT += a_dt;
		}

		g_s.hp = hp;
		g_s.hpMax = hpMax;
		g_s.mp = mp;
		g_s.mpMax = mpMax;
		g_s.sp = sp;
		g_s.spMax = spMax;
		g_s.havePrev = true;
	}

	// ------------------------------------------------------------------ per frame: boss bars
	void UpdateBosses(RE::GFxMovieView* v, RE::PlayerCharacter* p, float a_dt, bool a_paused)
	{
		const auto ppos = p->GetPosition();
		for (auto it = g_s.bosses.begin(); it != g_s.bosses.end();) {
			auto  sp = it->handle.get();
			auto* a = sp.get();
			bool  remove = false;
			if (!a || !a->Is3DLoaded()) {
				remove = true;
				bono::Logf("boss bars: {} unloaded -> bar removed", it->name);
			} else if (!a_paused) {
				const float f = Frac(CurAV(a, RE::ActorValue::kHealth), MaxAV(a, RE::ActorValue::kHealth));
				if (a->IsDead()) {
					it->fill = 0;
					if (it->deadT < 0.0f) it->deadT = 0;
					it->deadT += a_dt;
					if (it->deadT >= 1.5f) {
						remove = true;
						bono::Logf("boss bars: {} dead -> bar removed", it->name);
					}
				} else if (a->GetPosition().GetDistance(ppos) > 2.0f * g_cfg.bossRange) {
					remove = true;
					bono::Logf("boss bars: {} too far -> bar removed", it->name);
				} else {
					if (a->IsInCombat()) it->outOfCombat = 0;
					else it->outOfCombat += a_dt;
					if (it->outOfCombat >= g_cfg.bossDropTime) {
						remove = true;
						bono::Logf("boss bars: {} out of the fight -> bar removed", it->name);
					}
					if (f < it->fill) {  // damage: drop at once, trail holds
						it->trail = std::max(it->trail, it->fill);
						it->fill = f;
						it->trailHold = g_cfg.bossTrailHold;
					} else if (f > it->fill) {  // heal: rise slowly
						it->fill = std::min(f, it->fill + g_cfg.bossRiseRate * a_dt);
					}
				}
				if (it->trail <= it->fill) {
					it->trail = it->fill;
				} else if (it->trailHold > 0.0f) {
					it->trailHold -= a_dt;
				} else {
					it->trail = std::max(it->fill, it->trail - g_cfg.bossTrailRate * a_dt);
				}
			}
			if (remove) {
				it = g_s.bosses.erase(it);
				g_s.bossesDirty = true;
			} else {
				++it;
			}
		}

		// slots: first boss at the bottom (b0)
		for (int i = 0; i < kMaxBossSlots; ++i) {
			const std::string b = std::format("_root.bdmc_boss.b{}", i);
			const bool        used = i < static_cast<int>(g_s.bosses.size());
			if (g_s.bossesDirty) {
				SetVisible(v, b + "._visible", used);
				if (used) v->SetVariable((b + ".nm.text").c_str(), g_s.bosses[i].name.c_str());
			}
			if (!used) continue;
			const auto& boss = g_s.bosses[i];
			g_s.flash.Num(v, b + ".fm._x", (boss.fill - 1.0f) * kBossWidth, 0.05);
			g_s.flash.Num(v, b + ".tm._x", (boss.trail - 1.0f) * kBossWidth, 0.05);
			g_s.flash.Num(v, b + ".tip._x", -0.5f * kBossWidth + boss.fill * kBossWidth, 0.05);
		}
		g_s.bossesDirty = false;
	}

	// ------------------------------------------------------------------ per frame: main
	void ResetForNewMovie(RE::GFxMovieView* v)
	{
		g_s.view = v;
		g_s.flash.Reset();
		g_hudClip.Reset();
		g_bossClip.Reset();
		g_s.havePrev = false;
		g_s.shownCells = g_s.shownRows = -1;
		g_s.lastAlphaPct = -1;
		g_s.lastHudVisible = false;
		g_s.lastBossVisible = false;
		g_s.gaugeFound = {};
		g_s.gaugeAlphaPct = { -1, -1 };
		g_s.bossesDirty = true;
	}

	void OnHudAdvance(RE::IMenu* a_menu, float a_dt)
	{
		if (!a_menu || !a_menu->uiMovie) return;
		RE::GFxMovieView* v = a_menu->uiMovie.get();
		if (v != g_s.view) ResetForNewMovie(v);

		auto* p = RE::PlayerCharacter::GetSingleton();
		if (!p || !p->Is3DLoaded()) return;
		a_dt = std::clamp(a_dt, 0.0f, 0.25f);

		auto*      ui = RE::UI::GetSingleton();
		const bool paused = ui && ui->GameIsPaused();

		// load the SWFs into the HUD movie
		if (g_hudClip.Update(v, a_dt)) {
			g_s.flash.Num(v, "_root.bdmc_hud._x", g_cfg.hudX);
			g_s.flash.Num(v, "_root.bdmc_hud._y", g_cfg.hudY);
			g_s.flash.Num(v, "_root.bdmc_hud._xscale", g_cfg.hudScale);
			g_s.flash.Num(v, "_root.bdmc_hud._yscale", g_cfg.hudScale);
		}
		if (g_cfg.bossBars && g_bossClip.Update(v, a_dt)) {
			g_s.flash.Num(v, "_root.bdmc_boss._x", g_cfg.bossX);
			g_s.flash.Num(v, "_root.bdmc_boss._y", g_cfg.bossY);
			g_s.flash.Num(v, "_root.bdmc_boss._xscale", g_cfg.bossScale);
			g_s.flash.Num(v, "_root.bdmc_boss._yscale", g_cfg.bossScale);
			for (int i = 0; i < kMaxBossSlots; ++i) SetVisible(v, std::format("_root.bdmc_boss.b{}._visible", i), false);
			g_s.bossesDirty = true;
		}

		// vanilla HUD visibility (menus, dialogue, ... hide the vanilla health meter)
		bool vanillaShown = true;
		if (g_cfg.followVanilla) {
			RE::GFxValue vis;
			if (v->GetVariable(&vis, "_root.HUDMovieBaseInstance.Health._visible") && vis.IsBool()) vanillaShown = vis.GetBool();
		}
		if (vanillaShown != g_s.vanillaShown || !g_s.vanillaLogged) {
			bono::Logf("HUD: vanilla health meter _visible = {}", vanillaShown);
			g_s.vanillaShown = vanillaShown;
			g_s.vanillaLogged = true;
		}

		if (!paused) {
			g_s.scanT += a_dt;
			if (g_s.scanT >= 0.25f) {
				g_s.scanT = 0;
				Scan(p);
			}
		}

		// ---- player HUD
		if (g_hudClip.Ready()) {
			if (g_cfg.hideVanillaBars) {
				// the vanilla HUD animates these itself, so force them every frame
				v->SetVariableDouble("_root.HUDMovieBaseInstance.Health._alpha", 0.0);
				v->SetVariableDouble("_root.HUDMovieBaseInstance.Magica._alpha", 0.0);
				v->SetVariableDouble("_root.HUDMovieBaseInstance.Stamina._alpha", 0.0);
			}

			UpdatePlayerBars(v, p, a_dt, paused);

			// auto-hide fade
			if (!paused) {
				const bool  want = !g_cfg.autoHide || g_s.inCombat || g_s.idleT < g_cfg.autoHideDelay;
				const float target = want ? 1.0f : 0.0f;
				const float time = want ? g_cfg.fadeIn : g_cfg.fadeOut;
				if (time <= 0.0f) g_s.alpha = target;
				else if (g_s.alpha < target) g_s.alpha = std::min(target, g_s.alpha + a_dt / time);
				else if (g_s.alpha > target) g_s.alpha = std::max(target, g_s.alpha - a_dt / time);
				if (want != g_s.autoShowLogged) {
					if (want) bono::Logf("auto-hide: show ({})", g_s.inCombat ? "combat" : "values changing");
					else bono::Logf("auto-hide: idle -> fade out ({} s, no combat)", g_cfg.autoHideDelay);
					g_s.autoShowLogged = want;
				}
			}

			const int  alphaPct = static_cast<int>(std::lround(g_s.alpha * 100.0f));
			const bool visible = vanillaShown && alphaPct > 0;
			if (alphaPct != g_s.lastAlphaPct) {
				v->SetVariableDouble("_root.bdmc_hud._alpha", alphaPct);
				g_s.lastAlphaPct = alphaPct;
			}
			if (visible != g_s.lastHudVisible || g_s.shownCells < 0) {
				SetVisible(v, "_root.bdmc_hud._visible", visible);
				g_s.lastHudVisible = visible;
			}

			if (g_cfg.autoHide && g_cfg.autoHideGauges) {
				for (std::size_t i = 0; i < kGauges.size(); ++i) {
					if (!g_s.gaugeFound[i] || alphaPct == g_s.gaugeAlphaPct[i]) continue;
					v->SetVariableDouble((std::string(kGauges[i]) + "._alpha").c_str(), alphaPct);
					g_s.gaugeAlphaPct[i] = alphaPct;
				}
			}
		}

		// ---- boss bars
		if (g_cfg.bossBars && g_bossClip.Ready()) {
			UpdateBosses(v, p, a_dt, paused);
			const bool visible = vanillaShown && !g_s.bosses.empty();
			if (visible != g_s.lastBossVisible) {
				SetVisible(v, "_root.bdmc_boss._visible", visible);
				g_s.lastBossVisible = visible;
			}
		}
	}

	// ------------------------------------------------------------------ hook: HUDMenu::AdvanceMovie
	using AdvanceFn = void (*)(RE::IMenu*, float, std::uint32_t);
	std::uintptr_t g_origAdvance{ 0 };

	void Hook_HudAdvance(RE::IMenu* a_this, float a_interval, std::uint32_t a_currentTime)
	{
		if (g_s.listsLoaded) OnHudAdvance(a_this, a_interval);
		reinterpret_cast<AdvanceFn>(g_origAdvance)(a_this, a_interval, a_currentTime);
	}

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
		g_origAdvance = vtbl.write_vfunc(0x5, Hook_HudAdvance);
		bono::Logf("hook installed (HUDMenu::AdvanceMovie)");
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			LoadBossLists();
			g_s.listsLoaded = true;
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			g_s.bosses.clear();
			g_s.bossesDirty = true;
			g_s.havePrev = false;
			g_s.idleT = 0;
			g_s.alpha = 1;
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	g_cfg.Load();
	bono::Logf("runtime {}", REL::Module::get().version().string());

	InstallHooks();

	if (auto* msg = SKSE::GetMessagingInterface()) msg->RegisterListener(OnSKSEMessage);
	return true;
}
