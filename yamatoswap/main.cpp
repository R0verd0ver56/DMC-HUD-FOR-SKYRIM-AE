// Bono Yamato SDT swap (SE/AE, CommonLibSSE-NG, Address Library).
//
// While the Vergil SDT armour is worn (Sin Devil Trigger active) the Yamato weapon, scabbard and lace use a second
// set of meshes; when SDT ends they go back. Nothing in any ESP changes: the models of the existing forms are
// swapped in memory (so every patch / condition that points at the Yamato forms keeps working), then the weapon and
// scabbard are re-equipped so the new meshes load.
//
// Everything is driven by BonoYamatoSDTSwap.ini (see that file).

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Support.h"

using namespace std::literals;

namespace
{
	constexpr const char* kIniPath = "Data\\SKSE\\Plugins\\BonoYamatoSDTSwap.ini";
	constexpr const char* kLogPath = "Data\\SKSE\\Plugins\\BonoYamatoSDTSwap.log";

	enum class Slot
	{
		kModel,     // weapon / static / any form with a plain model
		kArmaMale,  // armour addon, male third person
		kArmaFemale,
		kArmaMale1st,
		kArmaFemale1st
	};

	struct Entry
	{
		std::string  plugin;
		RE::FormID   local{ 0 };
		Slot         slot{ Slot::kModel };
		std::string  normal;  // model path while NOT in SDT
		std::string  sdt;     // model path while in SDT
		RE::TESForm* form{ nullptr };
	};

	struct Item
	{
		std::string plugin;
		RE::FormID  local{ 0 };
	};

	struct Config
	{
		bool         enabled{ true };
		int          delayMs{ 600 };
		std::string  sdtPlugin{ "OmecaOneVergilSDT.esp" };
		RE::FormID   sdtLocal{ 0xD63 };
		bool         reequip{ true };
		std::vector<Entry> entries;
		std::vector<Item>  items;
	} g_cfg;

	std::atomic<unsigned> g_gen{ 0 };
	int                   g_applied{ -1 };  // -1 unknown, 0 normal models, 1 SDT models (main thread only)
	bool                  g_dataLoaded{ false };

	std::vector<std::string> Split(const std::string& a_s, char a_sep)
	{
		std::vector<std::string> out;
		std::size_t              pos = 0;
		while (pos <= a_s.size()) {
			auto n = a_s.find(a_sep, pos);
			out.push_back(bono::Trim(std::string_view(a_s).substr(pos, n == std::string::npos ? std::string::npos : n - pos)));
			if (n == std::string::npos) break;
			pos = n + 1;
		}
		return out;
	}

	RE::FormID ParseId(const std::string& a_s)
	{
		return static_cast<RE::FormID>(std::strtoul(a_s.c_str(), nullptr, 0));
	}

	bool ParseSlot(const std::string& a_s, Slot& a_out)
	{
		auto s = bono::Lower(a_s);
		if (s == "model") a_out = Slot::kModel;
		else if (s == "arma_m") a_out = Slot::kArmaMale;
		else if (s == "arma_f") a_out = Slot::kArmaFemale;
		else if (s == "arma_m1") a_out = Slot::kArmaMale1st;
		else if (s == "arma_f1") a_out = Slot::kArmaFemale1st;
		else return false;
		return true;
	}

	void LoadConfig()
	{
		bono::Ini ini;
		bool      have = ini.Load(kIniPath);
		g_cfg.enabled = ini.GetBool("Enabled", true);
		g_cfg.delayMs = std::clamp(ini.GetInt("Delay", 600), 0, 5000);
		g_cfg.reequip = ini.GetBool("Reequip", true);
		bono::Log::Get().Init(kLogPath, ini.GetBool("LogEnabled", false), ini.GetInt("MaxLogLines", 2000));

		auto sdt = Split(ini.GetString("SDTArmor", "OmecaOneVergilSDT.esp|0xD63"), '|');
		if (sdt.size() == 2) {
			g_cfg.sdtPlugin = sdt[0];
			g_cfg.sdtLocal  = ParseId(sdt[1]);
		}

		// Swap=plugin|formid|slot|normal model|SDT model
		for (auto& line : ini.GetAll("Swap")) {
			auto p = Split(line, '|');
			Entry e;
			if (p.size() != 5 || !ParseSlot(p[2], e.slot)) {
				bono::Logf("bad Swap line: {}", line);
				continue;
			}
			e.plugin = p[0];
			e.local  = ParseId(p[1]);
			e.normal = p[3];
			e.sdt    = p[4];
			g_cfg.entries.push_back(std::move(e));
		}

		// Reequip=plugin|formid  (weapon / scabbard that must reload its mesh)
		for (auto& line : ini.GetAll("Item")) {
			auto p = Split(line, '|');
			if (p.size() != 2) {
				bono::Logf("bad Item line: {}", line);
				continue;
			}
			g_cfg.items.push_back({ p[0], ParseId(p[1]) });
		}
		bono::Logf("config loaded ({}): {} swaps, {} items, delay {} ms", have ? "ini" : "no ini", g_cfg.entries.size(), g_cfg.items.size(), g_cfg.delayMs);
	}

	// ------------------------------------------------------------------ model swap
	RE::TESModel* GetModelSlot(Entry& a_e)
	{
		if (!a_e.form) return nullptr;
		if (a_e.slot == Slot::kModel) return skyrim_cast<RE::TESModel*>(a_e.form);
		auto* arma = a_e.form->As<RE::TESObjectARMA>();
		if (!arma) return nullptr;
		switch (a_e.slot) {
		case Slot::kArmaMale: return &arma->bipedModels[RE::SEXES::kMale];
		case Slot::kArmaFemale: return &arma->bipedModels[RE::SEXES::kFemale];
		case Slot::kArmaMale1st: return &arma->bipedModel1stPersons[RE::SEXES::kMale];
		case Slot::kArmaFemale1st: return &arma->bipedModel1stPersons[RE::SEXES::kFemale];
		default: return nullptr;
		}
	}

	bool SamePath(const char* a_a, const std::string& a_b)
	{
		return a_a && bono::Lower(a_a) == bono::Lower(a_b);
	}

	// Returns true when at least one model path actually changed.
	bool ApplyModels(bool a_sdt)
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) return false;
		bool changed = false;
		for (auto& e : g_cfg.entries) {
			if (!e.form) {
				e.form = dh->LookupForm(e.local, e.plugin);
				if (!e.form) {
					bono::Logf("form not found: {} {:X}", e.plugin, e.local);
					continue;
				}
			}
			auto* m = GetModelSlot(e);
			if (!m) {
				bono::Logf("no model slot on {} {:X}", e.plugin, e.local);
				continue;
			}
			const auto& want = a_sdt ? e.sdt : e.normal;
			if (SamePath(m->GetModel(), want)) continue;
			bono::Logf("{} {:X}: '{}' -> '{}'", e.plugin, e.local, m->GetModel() ? m->GetModel() : "", want);
			m->SetModel(want.c_str());
			changed = true;
		}
		return changed;
	}

	void ReequipItems(RE::PlayerCharacter* a_pc)
	{
		auto* em = RE::ActorEquipManager::GetSingleton();
		auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!em || !dom || !dh) return;
		auto* rightHand = dom->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kRightHandEquip);

		for (auto& it : g_cfg.items) {
			auto* form = dh->LookupForm(it.local, it.plugin);
			auto* obj  = form ? form->As<RE::TESBoundObject>() : nullptr;
			if (!obj) {
				bono::Logf("item not found: {} {:X}", it.plugin, it.local);
				continue;
			}
			if (obj->Is(RE::FormType::Weapon)) {
				const bool right = a_pc->GetEquippedObject(false) == form;
				const bool left  = a_pc->GetEquippedObject(true) == form;
				if (!right && !left) continue;
				const RE::BGSEquipSlot* slot = right ? rightHand : nullptr;
				bono::Logf("re-equip weapon {:X} ({})", it.local, right ? "right" : "left");
				em->UnequipObject(a_pc, obj, nullptr, 1, slot, false, true, false, true);
				em->EquipObject(a_pc, obj, nullptr, 1, slot, false, true, false, true);
			} else if (obj->Is(RE::FormType::Armor)) {
				if (!a_pc->GetWornArmor(obj->GetFormID())) continue;
				bono::Logf("re-equip armour {:X}", it.local);
				em->UnequipObject(a_pc, obj, nullptr, 1, nullptr, false, true, false, true);
				em->EquipObject(a_pc, obj, nullptr, 1, nullptr, false, true, false, true);
			}
		}
	}

	bool IsSDTWorn(RE::PlayerCharacter* a_pc)
	{
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) return false;
		auto id = dh->LookupFormID(g_cfg.sdtLocal, g_cfg.sdtPlugin);
		return id != 0 && a_pc->GetWornArmor(id) != nullptr;
	}

	// main thread
	void Evaluate()
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc || !pc->Is3DLoaded() || !g_dataLoaded) return;
		const int sdt = IsSDTWorn(pc) ? 1 : 0;
		const bool changed = ApplyModels(sdt != 0);
		bono::Logf("evaluate: SDT={} applied={} changed={}", sdt, g_applied, changed);
		g_applied = sdt;
		if (changed && g_cfg.reequip) ReequipItems(pc);
	}

	void Schedule(int a_delayMs)
	{
		const auto gen = ++g_gen;
		std::thread([gen, a_delayMs] {
			std::this_thread::sleep_for(std::chrono::milliseconds(a_delayMs));
			if (auto* t = SKSE::GetTaskInterface()) {
				t->AddTask([gen] {
					if (gen != g_gen.load()) return;  // a newer equip event will handle it
					Evaluate();
				});
			}
		}).detach();
	}

	// ------------------------------------------------------------------ events
	class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
	{
	public:
		static EquipSink* Get()
		{
			static EquipSink s;
			return &s;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
		{
			if (!g_cfg.enabled || !a_event || !a_event->actor || !a_event->actor->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) return RE::BSEventNotifyControl::kContinue;
			if (a_event->baseObject == dh->LookupFormID(g_cfg.sdtLocal, g_cfg.sdtPlugin)) {
				bono::Logf("SDT armour {} event", a_event->equipped ? "equip" : "unequip");
				Schedule(g_cfg.delayMs);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			g_dataLoaded = true;
			if (g_cfg.enabled) RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(EquipSink::Get());
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// the save may be in SDT or not, whatever the previous save left in memory
			g_applied = -1;
			if (g_cfg.enabled) Schedule(std::max(g_cfg.delayMs, 1500));
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	LoadConfig();
	bono::Logf("runtime {}", REL::Module::get().version().string());
	if (auto* msg = SKSE::GetMessagingInterface()) msg->RegisterListener(OnSKSEMessage);
	return true;
}
