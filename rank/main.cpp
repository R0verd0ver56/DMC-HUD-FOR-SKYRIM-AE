// Bono DMC Rank - SE/AE port (CommonLibSSE-NG, Address Library).
//
// Re-implementation of BonoDMCRank.dll v1.0 (built for SE 1.5.97 with hard-coded offsets).
// Behaviour is taken from BonoDMCRank.ini + the way the original drives BonoDMCRank.swf.
// This plugin only DISPLAYS a rank; it never changes game values.
//
// Hooks (same three places the original patched, plus the player anim-graph sink):
//   PlayerCharacter::Update            -> per-frame tick, hostile scan, decay, Flash updates
//   Character::HandleHealthDamage      -> player hits an enemy      -> points
//   PlayerCharacter::HandleHealthDamage-> enemy hits the player     -> rank drops
//   BSAnimationGraphEvent sink         -> tells attacks apart (PIE / SoundPlay / Collision_Add / swings)

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <array>
#include <cmath>
#include <deque>
#include <unordered_map>

#include "Support.h"

using namespace std::literals;

namespace
{
	constexpr const char* kIniPath = "Data\\SKSE\\Plugins\\BonoDMCRank.ini";
	constexpr const char* kLogPath = "Data\\SKSE\\Plugins\\BonoDMCRank.log";
	constexpr const char* kEspName = "Bono DMC Rank.esp";
	constexpr int         kRanks = 7;  // D C B A S SS SSS
	constexpr const char* kRankName[kRanks] = { "D", "C", "B", "A", "S", "SS", "SSS" };
	constexpr double      kDepth = 5231.0;  // movie-clip depth in the HUD (BonoDMCHUD uses 5232 / 5233)

	// ------------------------------------------------------------------ config
	struct Config
	{
		float pointsLight{ 10 }, pointsHeavy{ 18 }, pointsSkill{ 12 };
		float heavyRatio{ 1.5f };
		std::vector<float> repeatMult{ 1.0f, 0.7f, 0.4f, 0.15f };
		float repeatForget{ 8 };
		float identityWindow{ 1.2f };
		float airMult{ 1.5f };
		float killBonus{ 25 };
		float extraTargetMult{ 0.5f };
		float sameHitGap{ 0.15f };

		std::vector<float> rankPoints{ 100, 100, 100, 100, 150, 150, 200 };
		std::vector<float> decayPerSec{ 6, 8, 10, 13, 17, 22, 28 };
		float decayDelay{ 2 };
		float hitDropThrottle{ 0.4f };

		float hostileRange{ 4000 };
		float fadeOutTime{ 1.2f };
		float rankX{ 1173 }, rankY{ 240 }, rankScale{ 66.7f };
		bool  sound{ true };

		bool logAnimEvents{ false };
		bool logHits{ true };

		void Load()
		{
			bono::Ini ini;
			bool      ok = ini.Load(kIniPath);
			bono::Log::Get().Init(kLogPath, ok && ini.GetBool("LogEnabled", false), ini.GetInt("MaxLogLines", 4000));
			if (!ok) {
				bono::Logf("INI not found ({}), using defaults", kIniPath);
				return;
			}
			pointsLight = ini.GetFloat("PointsLight", pointsLight);
			pointsHeavy = ini.GetFloat("PointsHeavy", pointsHeavy);
			pointsSkill = ini.GetFloat("PointsSkill", pointsSkill);
			heavyRatio = ini.GetFloat("HeavyDamageRatio", heavyRatio);
			repeatMult = ini.GetFloatList("RepeatMult", repeatMult);
			repeatForget = ini.GetFloat("RepeatForget", repeatForget);
			identityWindow = ini.GetFloat("IdentityWindow", identityWindow);
			airMult = ini.GetFloat("AirMult", airMult);
			killBonus = ini.GetFloat("KillBonus", killBonus);
			extraTargetMult = ini.GetFloat("ExtraTargetMult", extraTargetMult);
			sameHitGap = ini.GetFloat("SameHitGap", sameHitGap);
			rankPoints = ini.GetFloatList("RankPoints", rankPoints);
			decayPerSec = ini.GetFloatList("DecayPerSec", decayPerSec);
			decayDelay = ini.GetFloat("DecayDelay", decayDelay);
			hitDropThrottle = ini.GetFloat("HitDropThrottle", hitDropThrottle);
			hostileRange = ini.GetFloat("HostileRange", hostileRange);
			fadeOutTime = ini.GetFloat("FadeOutTime", fadeOutTime);
			rankX = ini.GetFloat("RankX", rankX);
			rankY = ini.GetFloat("RankY", rankY);
			rankScale = ini.GetFloat("RankScale", rankScale);
			sound = ini.GetBool("Sound", sound);
			logAnimEvents = ini.GetBool("LogAnimEvents", logAnimEvents);
			logHits = ini.GetBool("LogHits", logHits);

			rankPoints.resize(kRanks, 100.0f);
			decayPerSec.resize(kRanks, 10.0f);
			if (repeatMult.empty()) repeatMult.push_back(1.0f);
			for (auto& p : rankPoints) p = std::max(p, 1.0f);
			bono::Logf("config loaded: light {} heavy {} skill {} | rankScale {} at ({}, {})", pointsLight, pointsHeavy, pointsSkill, rankScale, rankX, rankY);
		}
	};

	Config g_cfg;

	// ------------------------------------------------------------------ state
	enum class EvKind { kPIE, kAnno, kSwing };

	struct AnimEv
	{
		float       t;
		EvKind      kind;
		std::string name;
	};

	struct UseInfo
	{
		std::vector<float> times;     // when this attack was used (one entry per use)
		std::uint32_t      lastUse{ 0 };
	};

	struct State
	{
		std::recursive_mutex m;
		float                time{ 0 };

		// rank
		bool  active{ false };
		int   rank{ 0 };
		float pts{ 0 };
		float lastScore{ -999 };
		float lastHitDrop{ -999 };
		float noHostile{ 0 };
		int   hostiles{ 0 };
		float scanTimer{ 0 };

		// attack bookkeeping
		std::deque<AnimEv>                       events;
		std::uint32_t                            useId{ 0 };        // bumped by swing / PIE events
		std::uint32_t                            lastHitUse{ 0 };
		float                                    lastHitTime{ -999 };
		RE::FormID                               firstVictim{ 0 };  // first target hit by the current use
		std::unordered_map<std::string, UseInfo> uses;
		std::unordered_map<std::string, float>   lastSameHit;       // key|victim -> time
		float                                    avgDamage{ 0 };
		int                                      avgCount{ 0 };

		// visuals (applied from Tick on the main thread)
		bool pendShowRank{ false };
		bool pendUp{ false };
		bool pendShake{ false };
		bool pendHide{ false };
		int  shownRank{ -1 };
		int  shownAlpha{ -1 };
		bool shownVisible{ false };

		// flash loading
		RE::GFxMovieView* view{ nullptr };
		int               loadState{ 0 };  // 0 = not requested, 1 = waiting, 2 = ready, 3 = gave up
		int               pathTry{ 0 };
		float             waitT{ 0 };

		// sink
		bool        needSink{ true };
		RE::NiAVObject* lastRoot{ nullptr };
		bool        sinkLogged{ false };

		// sounds
		RE::BGSSoundDescriptorForm* snd[kRanks]{};
		bool                        soundsLooked{ false };
	};

	State g_s;

	constexpr std::array<const char*, 2> kSwfPaths{ "../BonoDMCRank.swf", "BonoDMCRank.swf" };
	constexpr const char*                kProbe = "_root.bdr_rank.pop.shk.beat.r0.fm";

	// ------------------------------------------------------------------ flash helpers
	void SetNum(RE::GFxMovieView* v, const std::string& path, double val) { v->SetVariableDouble(path.c_str(), val); }
	void SetBool(RE::GFxMovieView* v, const std::string& path, bool val) { v->SetVariable(path.c_str(), RE::GFxValue(val)); }
	void GotoPlay(RE::GFxMovieView* v, const char* clip, double frame)
	{
		RE::GFxValue arg(frame);
		v->Invoke((std::string("_root.bdr_rank.") + clip + ".gotoAndPlay").c_str(), nullptr, &arg, 1);
	}

	void ApplyLayout(RE::GFxMovieView* v)
	{
		SetNum(v, "_root.bdr_rank._x", g_cfg.rankX);
		SetNum(v, "_root.bdr_rank._y", g_cfg.rankY);
		SetNum(v, "_root.bdr_rank._xscale", g_cfg.rankScale);
		SetNum(v, "_root.bdr_rank._yscale", g_cfg.rankScale);
		SetBool(v, "_root.bdr_rank._visible", false);
		g_s.shownVisible = false;
		g_s.shownAlpha = -1;
		g_s.shownRank = -1;
	}

	void RequestLoad(RE::GFxMovieView* v)
	{
		const char*  path = kSwfPaths[static_cast<std::size_t>(g_s.pathTry) % kSwfPaths.size()];
		RE::GFxValue res;
		RE::GFxValue cargs[2]{ RE::GFxValue("bdr_rank"), RE::GFxValue(kDepth) };
		v->Invoke("_root.createEmptyMovieClip", &res, cargs, 2);
		RE::GFxValue larg(path);
		v->Invoke("_root.bdr_rank.loadMovie", nullptr, &larg, 1);
		g_s.loadState = 1;
		g_s.waitT = 0;
		bono::Logf("HUD: createEmptyMovieClip(bdr_rank) + loadMovie({})", path);
	}

	// Returns the HUD movie only when it exists and the SWF is loaded and ready to be driven.
	RE::GFxMovieView* GetReadyMovie(float a_dt)
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) return nullptr;
		auto menu = ui->GetMenu(RE::HUDMenu::MENU_NAME);
		if (!menu || !menu->uiMovie) {
			g_s.view = nullptr;
			g_s.loadState = 0;
			return nullptr;
		}
		RE::GFxMovieView* v = menu->uiMovie.get();
		if (v != g_s.view) {  // new HUD movie (game load, menu reload): start over
			g_s.view = v;
			g_s.loadState = 0;
			g_s.pathTry = 0;
		}

		switch (g_s.loadState) {
		case 0:
			RequestLoad(v);
			return nullptr;
		case 1:
			g_s.waitT += a_dt;
			if (v->IsAvailable(kProbe)) {
				g_s.loadState = 2;
				ApplyLayout(v);
				bono::Logf("HUD: SWF loaded from {}", kSwfPaths[static_cast<std::size_t>(g_s.pathTry) % kSwfPaths.size()]);
				return v;
			}
			if (g_s.waitT > 3.0f) {
				bono::Logf("HUD: SWF not there after 3 s -> retry with next path");
				++g_s.pathTry;
				if (g_s.pathTry >= static_cast<int>(kSwfPaths.size())) {
					g_s.loadState = 3;
					bono::Logf("HUD: SWF still not there -> no rank display (check Interface\\BonoDMCRank.swf)");
				} else {
					RequestLoad(v);
				}
			}
			return nullptr;
		case 2:
			return v;
		default:
			return nullptr;
		}
	}

	// ------------------------------------------------------------------ sound
	void LookupSounds()
	{
		if (g_s.soundsLooked) return;
		g_s.soundsLooked = true;
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) return;
		for (int i = 0; i < kRanks; ++i) {
			g_s.snd[i] = dh->LookupForm<RE::BGSSoundDescriptorForm>(static_cast<RE::FormID>(0x800 + i), kEspName);
			bono::Logf("forms: rank sound {} (0x{:X}) {}", kRankName[i], 0x800 + i, g_s.snd[i] ? "found" : "MISSING (SNDR)");
		}
	}

	void PlayRankSound(int a_rank)
	{
		if (!g_cfg.sound || a_rank < 0 || a_rank >= kRanks) return;
		LookupSounds();
		auto* form = g_s.snd[a_rank];
		auto* am = RE::BSAudioManager::GetSingleton();
		if (!form || !form->soundDescriptor || !am) return;
		RE::BSSoundHandle h;
		if (!am->BuildSoundDataFromDescriptor(h, form->soundDescriptor)) {
			bono::Logf("sound: build FAILED for rank {}", kRankName[a_rank]);
			return;
		}
		if (auto* p = RE::PlayerCharacter::GetSingleton(); p && p->Get3D()) h.SetObjectToFollow(p->Get3D());
		if (!h.Play()) bono::Logf("sound: Play FAILED for rank {}", kRankName[a_rank]);
	}

	// ------------------------------------------------------------------ rank logic (call with lock held)
	void ShowRankNow(RE::GFxMovieView* v, bool a_up, bool a_shake)
	{
		if (g_s.shownRank != g_s.rank) {
			for (int i = 0; i < kRanks; ++i)
				SetBool(v, "_root.bdr_rank.pop.shk.beat.r" + std::to_string(i) + "._visible", i == g_s.rank);
			g_s.shownRank = g_s.rank;
		}
		if (!g_s.shownVisible) {
			SetBool(v, "_root.bdr_rank._visible", true);
			g_s.shownVisible = true;
		}
		if (a_up) {
			GotoPlay(v, "pop", 2);
			GotoPlay(v, "flash", 2);
			GotoPlay(v, "burst", 2);
		}
		if (a_shake) GotoPlay(v, "pop.shk", 2);
	}

	void Activate()
	{
		g_s.active = true;
		g_s.rank = 0;
		g_s.pts = 0;
		g_s.lastScore = g_s.time;
		g_s.noHostile = 0;
		g_s.pendShowRank = true;
		g_s.pendUp = true;
		bono::Logf("rank: start at D");
	}

	void Deactivate()
	{
		g_s.active = false;
		g_s.rank = 0;
		g_s.pts = 0;
		g_s.pendHide = true;
		g_s.events.clear();
		g_s.uses.clear();
		g_s.lastSameHit.clear();
		g_s.avgDamage = 0;
		g_s.avgCount = 0;
		bono::Logf("rank: no hostiles around -> fade out");
	}

	void AddPoints(float a_pts)
	{
		g_s.pts += a_pts;
		g_s.lastScore = g_s.time;
		bool up = false;
		while (g_s.rank < kRanks - 1 && g_s.pts >= g_cfg.rankPoints[g_s.rank]) {
			g_s.pts -= g_cfg.rankPoints[g_s.rank];
			++g_s.rank;
			up = true;
		}
		if (g_s.rank == kRanks - 1) g_s.pts = std::min(g_s.pts, g_cfg.rankPoints[kRanks - 1]);
		if (up) {
			g_s.pendShowRank = true;
			g_s.pendUp = true;
			bono::Logf("rank up -> {} ({:.0f}/{:.0f})", kRankName[g_s.rank], g_s.pts, g_cfg.rankPoints[g_s.rank]);
		}
	}

	void DropRank(bool a_shake)
	{
		if (g_s.rank > 0) --g_s.rank;
		g_s.pts = 0;
		g_s.pendShowRank = true;
		g_s.pendShake = a_shake;
	}

	// ------------------------------------------------------------------ attack identity
	struct Identity
	{
		std::string key{ "unknown" };
		bool        skill{ false };
		const char* kindName{ "unknown" };
	};

	Identity Identify()
	{
		Identity id;
		const AnimEv *pie = nullptr, *anno = nullptr, *swing = nullptr;
		for (auto it = g_s.events.rbegin(); it != g_s.events.rend(); ++it) {
			if (g_s.time - it->t > g_cfg.identityWindow) break;
			if (!pie && it->kind == EvKind::kPIE) pie = &*it;
			if (!anno && it->kind == EvKind::kAnno) anno = &*it;
			if (!swing && it->kind == EvKind::kSwing) swing = &*it;
		}
		if (pie) {
			id.key = "skill:" + pie->name;
			id.skill = true;
			id.kindName = "skill";
		} else if (anno) {
			id.key = "anno:" + anno->name;
			id.kindName = "anno";
		} else if (swing) {
			id.key = "swing:" + swing->name;
			id.kindName = "swing";
		}
		return id;
	}

	void OnPlayerHitEnemy(RE::Actor* a_victim, float a_damage, bool a_killed)
	{
		std::scoped_lock l(g_s.m);
		const float t = g_s.time;
		auto*       player = RE::PlayerCharacter::GetSingleton();
		if (!player || a_damage <= 0.0f) return;

		auto id = Identify();

		// same attack hitting the same target again too fast -> 0 points
		const std::string hitKey = id.key + "|" + std::to_string(a_victim->GetFormID());
		if (auto it = g_s.lastSameHit.find(hitKey); it != g_s.lastSameHit.end() && t - it->second < g_cfg.sameHitGap) {
			it->second = t;
			if (g_cfg.logHits) bono::Logf("hit {} dmg {:.1f} -> 0 pts (same hit gap)", id.kindName, a_damage);
			return;
		}
		g_s.lastSameHit[hitKey] = t;

		// heavy = damage clearly above the recent average
		bool heavy = g_s.avgCount >= 3 && g_s.avgDamage > 0.0f && a_damage >= g_cfg.heavyRatio * g_s.avgDamage;
		g_s.avgDamage = g_s.avgCount == 0 ? a_damage : g_s.avgDamage * 0.85f + a_damage * 0.15f;
		++g_s.avgCount;

		float base = id.skill ? g_cfg.pointsSkill : (heavy ? g_cfg.pointsHeavy : g_cfg.pointsLight);

		// is this a new use of the attack, or another hit of the same swing?
		const bool newUse = g_s.useId != g_s.lastHitUse || (t - g_s.lastHitTime) > g_cfg.identityWindow;
		auto&      use = g_s.uses[id.key];
		std::erase_if(use.times, [&](float x) { return t - x > g_cfg.repeatForget; });
		if (newUse) {
			g_s.firstVictim = a_victim->GetFormID();
			use.times.push_back(t);
		}
		g_s.lastHitUse = g_s.useId;
		g_s.lastHitTime = t;

		const std::size_t n = use.times.empty() ? 0 : use.times.size() - 1;  // earlier uses within RepeatForget
		float             mult = g_cfg.repeatMult[std::min(n, g_cfg.repeatMult.size() - 1)];
		if (player->IsInMidair()) mult *= g_cfg.airMult;
		if (!newUse && a_victim->GetFormID() != g_s.firstVictim) mult *= g_cfg.extraTargetMult;

		float score = base * mult;
		if (a_killed) score += g_cfg.killBonus;

		if (!g_s.active) Activate();
		AddPoints(score);
		if (g_cfg.logHits)
			bono::Logf("hit {} dmg {:.1f}{} -> +{:.1f} pts{} | {} {:.0f}/{:.0f}", id.kindName, a_damage, heavy ? " (heavy)" : "", score, a_killed ? " KILL" : "", kRankName[g_s.rank], g_s.pts, g_cfg.rankPoints[g_s.rank]);
	}

	void OnEnemyHitPlayer()
	{
		std::scoped_lock l(g_s.m);
		if (!g_s.active) return;
		if (g_s.time - g_s.lastHitDrop < g_cfg.hitDropThrottle) return;
		g_s.lastHitDrop = g_s.time;
		DropRank(true);
		bono::Logf("player hit -> rank down to {}", kRankName[g_s.rank]);
	}

	// ------------------------------------------------------------------ anim graph sink
	class AnimSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
		{
			if (!a_ev) return RE::BSEventNotifyControl::kContinue;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || a_ev->holder != player) return RE::BSEventNotifyControl::kContinue;

			std::string tag = a_ev->tag.c_str() ? a_ev->tag.c_str() : "";
			std::string payload = a_ev->payload.c_str() ? a_ev->payload.c_str() : "";

			std::scoped_lock l(g_s.m);
			if (g_cfg.logAnimEvents) bono::Logf("anim: {} | {}", tag, payload);

			auto push = [&](EvKind k, std::string name, bool newUse) {
				g_s.events.push_back({ g_s.time, k, std::move(name) });
				while (g_s.events.size() > 64) g_s.events.pop_front();
				if (newUse) ++g_s.useId;
			};

			if (tag == "PIE") {
				push(EvKind::kPIE, payload, true);
			} else if (tag.rfind("SoundPlay", 0) == 0 || tag == "Collision_Add") {
				push(EvKind::kAnno, tag + (payload.empty() ? "" : "|" + payload), false);
			} else if (tag == "weaponSwing" || tag == "weaponLeftSwing" || tag.rfind("attackStart", 0) == 0 || tag.rfind("bashStart", 0) == 0) {
				push(EvKind::kSwing, tag, true);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	AnimSink g_sink;

	// ------------------------------------------------------------------ hooks
	using UpdateFn = void (*)(RE::PlayerCharacter*, float);
	using HealthDmgFn = void (*)(RE::Actor*, RE::Actor*, float);

	std::uintptr_t g_origPlayerUpdate{ 0 };
	std::uintptr_t g_origCharHealth{ 0 };
	std::uintptr_t g_origPlayerHealth{ 0 };

	int CountHostiles(RE::PlayerCharacter* a_player)
	{
		auto* pl = RE::ProcessLists::GetSingleton();
		if (!pl) return 0;
		int  n = 0;
		auto ppos = a_player->GetPosition();
		for (auto& h : pl->highActorHandles) {
			auto sp = h.get();
			auto* a = sp.get();
			if (!a || a == a_player) continue;
			if (a->IsDead() || a->IsDisabled()) continue;
			if (!a->IsInCombat()) continue;
			if (!a->IsHostileToActor(a_player)) continue;
			if (a->GetPosition().GetDistance(ppos) > g_cfg.hostileRange) continue;
			++n;
		}
		return n;
	}

	void TrySinkRegister(RE::PlayerCharacter* a_player)
	{
		auto* root = a_player->Get3D();
		if (!root) return;
		if (root != g_s.lastRoot) {  // graph was (re)built: sink is gone with the old graph
			g_s.lastRoot = root;
			g_s.needSink = true;
		}
		if (!g_s.needSink) return;
		if (a_player->AddAnimationGraphEventSink(&g_sink)) {
			g_s.needSink = false;
			if (!g_s.sinkLogged) {
				bono::Logf("hook: player anim-graph event sink installed");
				g_s.sinkLogged = true;
			}
		}
	}

	void Tick(RE::PlayerCharacter* a_player, float a_dt)
	{
		if (a_dt <= 0.0f || a_dt > 1.0f) return;  // paused / loading hitches

		std::scoped_lock l(g_s.m);
		g_s.time += a_dt;

		TrySinkRegister(a_player);

		// hostile scan (cheap, 5x per second)
		g_s.scanTimer += a_dt;
		if (g_s.scanTimer >= 0.2f) {
			g_s.scanTimer = 0;
			int h = CountHostiles(a_player);
			if (h != g_s.hostiles) bono::Logf("hostiles around: {}", h);
			g_s.hostiles = h;
		}

		if (g_s.hostiles > 0) {
			g_s.noHostile = 0;
			if (!g_s.active) Activate();
		} else if (g_s.active) {
			g_s.noHostile += a_dt;
			if (g_s.noHostile >= g_cfg.fadeOutTime) Deactivate();
		}

		// decay
		if (g_s.active && g_s.time - g_s.lastScore >= g_cfg.decayDelay && g_s.hostiles > 0) {
			g_s.pts -= g_cfg.decayPerSec[g_s.rank] * a_dt;
			if (g_s.pts < 0.0f) {
				if (g_s.rank > 0) {
					--g_s.rank;
					g_s.pts = std::max(0.0f, g_cfg.rankPoints[g_s.rank] + g_s.pts);
					g_s.pendShowRank = true;
				} else {
					g_s.pts = 0.0f;
				}
			}
		}

		// ---- flash
		auto* v = GetReadyMovie(a_dt);
		if (!v) return;

		if (g_s.pendHide) {
			SetBool(v, "_root.bdr_rank._visible", false);
			g_s.shownVisible = false;
			g_s.shownRank = -1;
			g_s.shownAlpha = -1;
			g_s.pendHide = false;
			g_s.pendShowRank = g_s.pendUp = g_s.pendShake = false;
			return;
		}

		if (g_s.active) {
			if (g_s.pendShowRank || !g_s.shownVisible) {
				const bool up = g_s.pendUp;
				ShowRankNow(v, up, g_s.pendShake);
				if (up && g_s.pendShowRank) PlayRankSound(g_s.rank);
				g_s.pendShowRank = g_s.pendUp = g_s.pendShake = false;
			}
			// fade while there are no hostiles
			int alpha = 100;
			if (g_s.hostiles <= 0 && g_cfg.fadeOutTime > 0.0f)
				alpha = static_cast<int>(100.0f * std::clamp(1.0f - g_s.noHostile / g_cfg.fadeOutTime, 0.0f, 1.0f));
			if (alpha != g_s.shownAlpha) {
				SetNum(v, "_root.bdr_rank._alpha", alpha);
				g_s.shownAlpha = alpha;
			}
		}
	}

	// ---- PlayerCharacter::Update
	void Hook_PlayerUpdate(RE::PlayerCharacter* a_this, float a_dt)
	{
		reinterpret_cast<UpdateFn>(g_origPlayerUpdate)(a_this, a_dt);
		Tick(a_this, a_dt);
	}

	// ---- Character::HandleHealthDamage   (this = victim, attacker = who hit)
	void Hook_CharHealth(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const bool relevant = player && a_attacker == player && a_this != player && !a_this->IsDead() && a_this->IsHostileToActor(player);
		reinterpret_cast<HealthDmgFn>(g_origCharHealth)(a_this, a_attacker, a_damage);
		if (relevant) OnPlayerHitEnemy(a_this, std::fabs(a_damage), a_this->IsDead());
	}

	// ---- PlayerCharacter::HandleHealthDamage   (this = player)
	void Hook_PlayerHealth(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
	{
		reinterpret_cast<HealthDmgFn>(g_origPlayerHealth)(a_this, a_attacker, a_damage);
		if (a_attacker && a_attacker != a_this && std::fabs(a_damage) > 0.0f) OnEnemyHitPlayer();
	}

	void InstallHooks()
	{
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
			g_origPlayerUpdate = vtbl.write_vfunc(0xAD, Hook_PlayerUpdate);
			g_origPlayerHealth = vtbl.write_vfunc(0x104, Hook_PlayerHealth);
		}
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
			g_origCharHealth = vtbl.write_vfunc(0x104, Hook_CharHealth);
		}
		bono::Logf("hooks installed (PlayerCharacter::Update, PlayerCharacter::HandleHealthDamage, Character::HandleHealthDamage)");
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			LookupSounds();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			{
				std::scoped_lock l(g_s.m);
				g_s.needSink = true;
				g_s.lastRoot = nullptr;
				g_s.active = false;
				g_s.rank = 0;
				g_s.pts = 0;
				g_s.hostiles = 0;
				g_s.pendHide = true;
				g_s.events.clear();
			}
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
	bono::Logf("Bono DMC Rank v1.0 (SE/AE port) | runtime {}", REL::Module::get().version().string());

	SKSE::AllocTrampoline(64);
	InstallHooks();

	if (auto* msg = SKSE::GetMessagingInterface()) msg->RegisterListener(OnSKSEMessage);
	return true;
}
