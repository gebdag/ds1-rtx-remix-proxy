// Offline test of src/comp/game/game_build.cpp against DARKSOULS.exe files on disk.
//
//   build.cmd, then: test_game_build.exe <2015-06-15 exe> <2015-01-14 exe>
//
// Each exe is mapped the way the loader maps it (headers + sections at their RVAs), then:
//   current exe:  build table == the current addresses; fallback lookup finds the same ones
//   2015-01-14:   build table passes its code checks and equals the fallback lookup
//   2015-01-14 with a changed timestamp: detected as unknown, resolved by lookup, same result
#include "std_include.hpp"
#include "../../src/comp/game/game_build.hpp"

#include <fstream>

using namespace comp::game::build;

static std::vector<uint8_t> map_image(const char* path)
{
	std::ifstream f(path, std::ios::binary);
	std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
	if (file.size() < 0x400) return {};
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew);
	std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage);
	std::memcpy(image.data(), file.data(), nt->OptionalHeader.SizeOfHeaders);
	const auto* sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		const DWORD n = std::min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize);
		std::memcpy(image.data() + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, n);
	}
	return image;
}

static void quiet(const char*) {}
static void loud(const char* s) { std::printf("    %s\n", s); }

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: test_game_build <2015-06-15 exe> <2015-01-14 exe>\n");
		return 2;
	}
	auto cur = map_image(argv[1]);
	auto jan = map_image(argv[2]);
	if (cur.empty() || jan.empty()) { std::printf("cannot read the exe files\n"); return 2; }

	const int n = table_size();
	std::vector<resolved> a(n), b(n), c(n);
	int failures = 0;
	auto fail = [&](const std::string& s) { ++failures; std::printf("FAIL %s\n", s.c_str()); };

	// 1. current build, table path: exactly the current addresses
	const image_view vc{ cur.data(), 0x400000, static_cast<DWORD>(cur.size()) };
	if (resolve_all(vc, a.data(), false, &quiet) != id::current) fail("current exe not detected as 2015-06-15");
	for (int i = 0; i < n; ++i)
		if (a[i].address != a[i].current_va) fail(std::format("current table {} 0x{:08X} != 0x{:08X}", a[i].name, a[i].address, a[i].current_va));

	// 2. current build, fallback lookup: must find the same addresses
	resolve_all(vc, b.data(), true, &quiet);
	for (int i = 0; i < n; ++i)
		if (b[i].address != b[i].current_va) fail(std::format("current lookup {} 0x{:08X} != 0x{:08X}", b[i].name, b[i].address, b[i].current_va));
	std::printf("2015-06-15: table and lookup checked for %d addresses\n", n);

	// 3. 2015-01-14, table path (with its code checks) and lookup must agree, all found
	const image_view vj{ jan.data(), 0x400000, static_cast<DWORD>(jan.size()) };
	std::printf("2015-01-14 table:\n");
	if (resolve_all(vj, a.data(), false, &loud) != id::jan_2015) fail("old exe not detected as 2015-01-14");
	resolve_all(vj, b.data(), true, &quiet);
	for (int i = 0; i < n; ++i)
	{
		if (!a[i].address) fail(std::format("2015-01-14 table {} missing", a[i].name));
		if (a[i].address != b[i].address) fail(std::format("2015-01-14 {} table 0x{:08X} != lookup 0x{:08X}", a[i].name, a[i].address, b[i].address));
	}

	// 4. an unknown build: the old exe with another timestamp goes through the lookup
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(jan.data());
	reinterpret_cast<IMAGE_NT_HEADERS32*>(jan.data() + dos->e_lfanew)->FileHeader.TimeDateStamp ^= 0x1;
	if (resolve_all(vj, c.data(), false, &quiet) != id::unknown) fail("changed timestamp not detected as unknown");
	for (int i = 0; i < n; ++i)
		if (c[i].address != a[i].address) fail(std::format("unknown-build lookup {} 0x{:08X} != 0x{:08X}", c[i].name, c[i].address, a[i].address));

	std::printf(failures ? "%d FAILURES\n" : "ALL PASS (%d addresses, 4 paths)\n", failures ? failures : n);
	return failures ? 1 : 0;
}
