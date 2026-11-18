// Shared helpers for the Bono DMC AE port: tiny INI reader + tiny file logger.
// Deliberately dependency-free (std only) so it behaves the same on SE / AE.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bono
{
	inline std::string Trim(std::string_view a_in)
	{
		std::size_t b = 0, e = a_in.size();
		while (b < e && std::isspace(static_cast<unsigned char>(a_in[b]))) ++b;
		while (e > b && std::isspace(static_cast<unsigned char>(a_in[e - 1]))) --e;
		return std::string(a_in.substr(b, e - b));
	}

	inline std::string Lower(std::string a_in)
	{
		std::transform(a_in.begin(), a_in.end(), a_in.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_in;
	}

	// ---------------------------------------------------------------- INI
	// Format used by the original mod:  Key=Value   ; comment
	// Lines starting with ';' are comments. Keys are case-insensitive.
	// A key may appear several times (HUD boss lists use NPC= / Race= repeatedly).
	class Ini
	{
	public:
		bool Load(const std::string& a_path)
		{
			std::ifstream f(a_path, std::ios::binary);
			if (!f) return false;
			std::string line;
			bool first = true;
			while (std::getline(f, line)) {
				if (first) {  // strip UTF-8 BOM
					first = false;
					if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
						line.erase(0, 3);
				}
				auto sc = line.find(';');
				if (sc != std::string::npos) line.erase(sc);
				auto eq = line.find('=');
				if (eq == std::string::npos) continue;
				auto key = Lower(Trim(std::string_view(line).substr(0, eq)));
				auto val = Trim(std::string_view(line).substr(eq + 1));
				if (!key.empty()) _entries.emplace_back(std::move(key), std::move(val));
			}
			return true;
		}

		bool Has(std::string_view a_key) const { return Find(a_key) != nullptr; }

		std::string GetString(std::string_view a_key, std::string a_def = {}) const
		{
			auto* v = Find(a_key);
			return v ? *v : a_def;
		}

		float GetFloat(std::string_view a_key, float a_def) const
		{
			auto* v = Find(a_key);
			if (!v || v->empty()) return a_def;
			char* end = nullptr;
			float r = std::strtof(v->c_str(), &end);
			return end == v->c_str() ? a_def : r;
		}

		int GetInt(std::string_view a_key, int a_def) const { return static_cast<int>(GetFloat(a_key, static_cast<float>(a_def))); }

		bool GetBool(std::string_view a_key, bool a_def) const { return GetInt(a_key, a_def ? 1 : 0) != 0; }

		// "1.0,0.7,0.4"  ->  {1.0, 0.7, 0.4}. Missing/short lists are padded from a_def.
		std::vector<float> GetFloatList(std::string_view a_key, std::vector<float> a_def) const
		{
			auto* v = Find(a_key);
			if (!v || v->empty()) return a_def;
			std::vector<float> out;
			std::size_t pos = 0;
			while (pos <= v->size()) {
				auto comma = v->find(',', pos);
				auto tok = Trim(std::string_view(*v).substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
				if (!tok.empty()) out.push_back(std::strtof(tok.c_str(), nullptr));
				if (comma == std::string::npos) break;
				pos = comma + 1;
			}
			for (std::size_t i = out.size(); i < a_def.size(); ++i) out.push_back(a_def[i]);
			return out;
		}

		// All values for a repeated key, in file order.
		std::vector<std::string> GetAll(std::string_view a_key) const
		{
			std::vector<std::string> out;
			auto k = Lower(std::string(a_key));
			for (auto& [ek, ev] : _entries)
				if (ek == k) out.push_back(ev);
			return out;
		}

	private:
		const std::string* Find(std::string_view a_key) const
		{
			auto k = Lower(std::string(a_key));
			const std::string* found = nullptr;
			for (auto& [ek, ev] : _entries)
				if (ek == k) found = &ev;  // last one wins
			return found;
		}

		std::vector<std::pair<std::string, std::string>> _entries;
	};

	// ---------------------------------------------------------------- Log
	// Honors LogEnabled / MaxLogLines from the INI, like the original DLLs.
	class Log
	{
	public:
		static Log& Get()
		{
			static Log inst;
			return inst;
		}

		void Init(const std::string& a_path, bool a_enabled, int a_maxLines)
		{
			std::scoped_lock l(_m);
			_enabled = a_enabled;
			_max = a_maxLines;
			_lines = 0;
			if (_enabled) _f.open(a_path, std::ios::trunc);
		}

		void Write(std::string_view a_msg)
		{
			std::scoped_lock l(_m);
			if (!_enabled || !_f.is_open()) return;
			if (_lines >= _max) {
				if (_lines == _max) {
					_f << "(log line limit reached)\n";
					_f.flush();
					++_lines;
				}
				return;
			}
			_f << a_msg << '\n';
			_f.flush();
			++_lines;
		}

		bool Enabled() const { return _enabled; }

	private:
		std::mutex    _m;
		std::ofstream _f;
		bool          _enabled{ false };
		int           _max{ 3000 };
		int           _lines{ 0 };
	};

	template <class... Args>
	inline void Logf(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		if (!Log::Get().Enabled()) return;
		Log::Get().Write(std::format(a_fmt, std::forward<Args>(a_args)...));
	}
}
