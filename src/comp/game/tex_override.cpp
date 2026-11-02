// DSfix-style texture packs. The file names are the SuperFastHash (Paul Hsieh, BSD licence) of the texture's source data.
#include "std_include.hpp"
#include "tex_override.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace comp::game::tex_override
{
	namespace
	{
		std::unordered_map<uint32_t, std::filesystem::path> g_files;

		uint32_t super_fast_hash(const char* data, int len)
		{
			if (len <= 0 || !data)
				return 0;
			auto read16 = [](const char* d) { return static_cast<uint32_t>(*reinterpret_cast<const uint16_t*>(d)); };
			uint32_t hash = static_cast<uint32_t>(len);
			const int rem = len & 3;
			for (len >>= 2; len > 0; len--)
			{
				hash += read16(data);
				const uint32_t tmp = (read16(data + 2) << 11) ^ hash;
				hash = (hash << 16) ^ tmp;
				data += 4;
				hash += hash >> 11;
			}
			switch (rem)
			{
			case 3:
				hash += read16(data);
				hash ^= hash << 16;
				hash ^= static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(data[2]))) << 18;
				hash += hash >> 11;
				break;
			case 2:
				hash += read16(data);
				hash ^= hash << 11;
				hash += hash >> 17;
				break;
			case 1:
				hash += static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(*data)));
				hash ^= hash << 10;
				hash += hash >> 1;
				break;
			default:
				break;
			}
			hash ^= hash << 3;
			hash += hash >> 5;
			hash ^= hash << 4;
			hash += hash >> 17;
			hash ^= hash << 25;
			hash += hash >> 6;
			return hash;
		}

		// One texture may come in several formats: .png wins, then .dds, then the rest of what D3DX reads. Lower wins.
		int extension_rank(std::string extension)
		{
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (extension == ".png")
				return 0;
			if (extension == ".dds")
				return 1;
			constexpr const char* kOthers[] = { ".bmp", ".dib", ".hdr", ".jpg", ".pfm", ".ppm", ".tga" };
			return std::any_of(std::begin(kOthers), std::end(kOthers), [&](const char* e) { return extension == e; }) ? 2 : -1;
		}

		// Each file's first use, and each failure, once.
		void note_once(uint32_t hash, const std::string& text)
		{
			static std::mutex mutex;
			static std::set<std::string> seen;
			const std::string key = std::format("{:08x} {}", hash, text);
			std::lock_guard lock(mutex);
			if (seen.insert(key).second)
				shared::common::log("TexOverride", key);
		}
	}

	void init()
	{
		if (shared::globals::lookdev_host)
			return;
		std::error_code error;
		const std::filesystem::path folder = std::filesystem::path(shared::globals::root_path) / "tex_override";
		std::unordered_map<uint32_t, int> ranks;
		for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
		{
			if (!it->is_regular_file(error))
				continue;
			const std::string stem = it->path().stem().string();
			uint32_t hash = 0;
			const auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), hash, 16);
			const int rank = extension_rank(it->path().extension().string());
			if (stem.size() != 8 || parsed.ec != std::errc() || parsed.ptr != stem.data() + stem.size() || rank < 0)
				continue;
			const auto known = ranks.find(hash);
			if (known == ranks.end() || rank < known->second)
			{
				ranks[hash] = rank;
				g_files[hash] = it->path();
			}
		}
		if (!g_files.empty())
			shared::common::log("TexOverride", std::format("{} replacement texture(s) in tex_override", g_files.size()));
	}

	bool read_replacement(const void* src, UINT src_size, std::vector<char>& file)
	{
		if (g_files.empty() || !src)
			return false;
		const uint32_t hash = super_fast_hash(static_cast<const char*>(src), static_cast<int>(src_size));
		const auto it = g_files.find(hash);
		if (it == g_files.end())
			return false;
		std::ifstream in(it->second, std::ios::binary | std::ios::ate);
		const auto size = in ? static_cast<std::streamoff>(in.tellg()) : std::streamoff(0);
		if (size <= 0)
		{
			note_once(hash, "could not be read: the game keeps its own");
			return false;
		}
		file.resize(static_cast<size_t>(size));
		in.seekg(0);
		if (!in.read(file.data(), size))
		{
			note_once(hash, "could not be read: the game keeps its own");
			return false;
		}
		note_once(hash, std::format("replaced by {}", it->second.filename().string()));
		return true;
	}

	void note_failed(const void* src, UINT src_size, HRESULT hr)
	{
		note_once(super_fast_hash(static_cast<const char*>(src), static_cast<int>(src_size)),
			std::format("D3DX could not use the file ({:#x}): the game keeps its own", static_cast<uint32_t>(hr)));
	}
}
