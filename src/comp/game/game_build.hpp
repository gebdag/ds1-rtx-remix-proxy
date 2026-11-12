#pragma once

// Which DARKSOULS.exe is running, and where the fixed game addresses the proxy uses live in it.
//
// The proxy is written against the 2015-06-15 Steam build, and its call sites keep that build's
// addresses as constants. Every one of them is read through build::at():
//
//   1. 2015-06-15 (current): at(va) is EXE_BASE + (va - 0x400000), the arithmetic the call sites
//      always did. Nothing is scanned, looked up or checked.
//   2. 2015-01-14: a second table, derived offline and cross-checked
//      (scripts/gen_build_signatures.py, analysis/exe_build_2015-01-14_compare.html).
//      Code addresses are checked against their signature before use.
//   3. Any other build: each address is looked up once at startup, by a byte signature that
//      matches exactly once in both known builds, by RTTI for vtables, or through another entry.
//
// On builds 2 and 3, an address that is not in the table or cannot be found reads as 0, and the
// feature that needs it stays off. The build is picked from the PE header (link timestamp +
// image size).

namespace comp::game::build
{
	enum class id : uint8_t
	{
		current,   // 2015-06-15
		jan_2015,  // 2015-01-14
		unknown,
	};

	// This process's address for `current_va`, an address of the 2015-06-15 build (a VA at image
	// base 0x400000). 0 when it is not available on the running build.
	DWORD at(DWORD current_va);

	// The reverse, for values recorded into captures (the lookdev host reads them as 2015-06-15
	// addresses): the 2015-06-15 address of a table address on the running build, 0 if it is none.
	// On 2015-06-15 the value itself.
	DWORD to_current(DWORD address);

	id which();
	const char* name();
	bool is_current();

	// ---- for scripts/test_game_build.cpp (checks against exe files on disk) ----

	struct image_view
	{
		const uint8_t* base;   // where the image is mapped in this process
		DWORD va_base;         // the address its own pointers assume (its load address)
		DWORD size;            // SizeOfImage
	};

	struct resolved
	{
		DWORD current_va;
		DWORD address;         // in va_base space, 0 = not available
		const char* name;
	};

	int table_size();

	// Resolve every table entry for `image` the way the module does at startup. force_scan
	// ignores the build tables and uses the fallback lookups. Returns the build it chose.
	id resolve_all(const image_view& image, resolved* out, bool force_scan, void (*log_line)(const char* text) = nullptr);
}
