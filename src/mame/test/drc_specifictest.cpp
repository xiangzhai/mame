// license:BSD-3-Clause
// copyright-holders:MAMEdev Team

/***************************************************************************

    drc_specifictest.cpp

    Standalone test for the DRC specific-access fast path.

    A 32-bit address space has native_bytes == 4, so UML dword reads and
    writes take the specific_access_info path in the native backend.  That
    path has to synthesise a full 32-bit mem_mask; getting it wrong (for
    example by shifting a 32-bit constant by 32) produces a zero mask,
    which does not crash but makes reads return 0 and writes disappear.

    This test deliberately does not use a guest CPU.  It builds a small
    32-bit space backed by RAM, performs dword reads and writes through a
    generated UML block, and verifies the data both inside the block and
    in the backing memory afterwards.

***************************************************************************/

#include "emu.h"
#include "emuopts.h"

#include "cpu/drcuml.h"
#include "cpu/drcumlsh.h"
#include "cpu/drccache.h"

#include <array>
#include <memory>


namespace {

// The address map is built again when the machine is validated, so the
// handlers must not rely on state from a particular driver instance.
u32 g_register_read_mask = 0;
u32 g_register_write_mask = 0;
u32 g_narrow_read_mask = 0;
u32 g_narrow_write_mask = 0;
u32 g_narrow_write_data = 0;

} // anonymous namespace


DECLARE_DEVICE_TYPE(DRC_SPECIFICTEST_MEM, drc_specifictest_mem_device)

class drc_specifictest_mem_device : public device_t, public device_memory_interface
{
public:
	drc_specifictest_mem_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
		: device_t(mconfig, DRC_SPECIFICTEST_MEM, tag, owner, clock)
		, device_memory_interface(mconfig, *this)
		, m_space_config("program", ENDIANNESS_LITTLE, 32, 16, 0)
	{
	}

protected:
	virtual void device_start() override ATTR_COLD { }

	virtual space_config_vector memory_space_config() const override
	{
		return space_config_vector {
			std::make_pair(AS_PROGRAM, &m_space_config),
		};
	}

private:
	address_space_config m_space_config;
};


class drc_specifictest_state : public driver_device
{
public:
	drc_specifictest_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_mem(*this, "mem")
		, m_cache(16 * 1024 * 1024)
	{
	}

	void drc_specifictest(machine_config &config)
	{
		drc_specifictest_mem_device &mem = downcast<drc_specifictest_mem_device &>(*config.device_add("mem", DRC_SPECIFICTEST_MEM, 0));
		mem.set_addrmap(AS_PROGRAM, *this, &drc_specifictest_state::program_map);
	}

protected:
	virtual void machine_start() override ATTR_COLD;

private:
	static constexpr offs_t READ_BASE = 0x0100;
	static constexpr offs_t WRITE_BASE = 0x0200;
	static constexpr offs_t NARROW_WRITE_BASE = 0x0400;
	static constexpr offs_t REGISTER_ADDRESS = 0x0300;
	static constexpr offs_t NARROW_READ_ADDRESS = 0x0310;
	static constexpr offs_t NARROW_WRITE_ADDRESS = 0x0314;
	static constexpr int COUNT = 8;
	static constexpr u32 REGISTER_READ_VALUE = 0xcafef00d;
	static constexpr u32 REGISTER_WRITE_VALUE = 0x13579bdf;
	static constexpr u32 NARROW_REGISTER_VALUE = 0x89abcdef;
	static constexpr u32 NARROW_WRITE_VALUE = 0x5a;
	static constexpr u32 NARROW_WRITE_MASK = 0x0f;
	static constexpr u32 NARROW_SEED = 0x11223344;
	static constexpr u32 FULL_DWORD_MASK = 0xffffffff;

	static constexpr u32 READ_PATTERNS[COUNT] = {
		0x00000000, 0x00000001, 0x7fffffff, 0x80000000,
		0xdeadbeef, 0xffffffff, 0x00ff00ff, 0x5a5a5a5a
	};

	static constexpr u32 WRITE_PATTERNS[COUNT] = {
		0xffffffff, 0x80000000, 0x12345678, 0xdeadbeef,
		0x00000001, 0xa5a5a5a5, 0x7fffffff, 0x00000000
	};

	void program_map(address_map &map) ATTR_COLD
	{
		map(0x0000, 0xffff).ram();
		map(REGISTER_ADDRESS, REGISTER_ADDRESS + 3).rw(
				FUNC(drc_specifictest_state::register_r),
				FUNC(drc_specifictest_state::register_w));
		map(NARROW_READ_ADDRESS, NARROW_READ_ADDRESS + 3).r(
				FUNC(drc_specifictest_state::narrow_read_r));
		map(NARROW_WRITE_ADDRESS, NARROW_WRITE_ADDRESS + 3).w(
				FUNC(drc_specifictest_state::narrow_write_w));
	}

	// Only the masks are observed here; the data path is covered by the
	// RAM reads and writes.  The state is global because address maps are
	// rebuilt for validity checking as well as for the running machine.
	u32 register_r(offs_t offset, u32 mem_mask)
	{
		g_register_read_mask = mem_mask;
		return REGISTER_READ_VALUE;
	}

	void register_w(offs_t offset, u32 data, u32 mem_mask)
	{
		g_register_write_mask = mem_mask;
	}

	u32 narrow_read_r(offs_t offset, u32 mem_mask)
	{
		g_narrow_read_mask = mem_mask;
		return NARROW_REGISTER_VALUE;
	}

	void narrow_write_w(offs_t offset, u32 data, u32 mem_mask)
	{
		g_narrow_write_data = data;
		g_narrow_write_mask = mem_mask;
	}

	required_device<drc_specifictest_mem_device> m_mem;
	drc_cache m_cache;
	std::unique_ptr<drcuml_state> m_drcuml;
	uml::code_handle *m_entry = nullptr;

	std::array<u32, COUNT> m_read_results{};
	std::array<u32, COUNT * 4> m_read_byte_results{};
	std::array<u32, COUNT * 2> m_read_word_results{};
	std::array<u32, 4> m_indexed_storage{};
	u32 m_indexed_result = 0;
	u32 m_narrow_read_result = 0;
};


void drc_specifictest_state::machine_start()
{
	m_cache.allocate_cache(mconfig().options().drc_rwx());

	g_register_read_mask = 0;
	g_register_write_mask = 0;
	g_narrow_read_mask = 0;
	g_narrow_write_mask = 0;
	g_narrow_write_data = 0;

	address_space &space = m_mem->space(AS_PROGRAM);
	address_space::specific_access_info const accessors = space.specific_accessors();
	if (accessors.native_bytes != 4)
		throw emu_fatalerror("drc_specifictest: expected a 32-bit native space, got %u bytes\n", accessors.native_bytes);

	// Seed the backing memory through the normal C++ accessors.  The
	// generated code has to see these values through the fast path.
	for (int i = 0; i < COUNT; i++)
	{
		space.write_dword(READ_BASE + (i * 4), READ_PATTERNS[i]);
		space.write_dword(NARROW_WRITE_BASE + (i * 4), NARROW_SEED);
	}

	uint32_t const flags = 0;
	m_drcuml = std::make_unique<drcuml_state>(*m_mem, m_cache, flags, 8, 16, 0, 0x1000);
	m_entry = m_drcuml->handle_alloc("entry");

	drcuml_block &block = m_drcuml->begin_block(1024);
	UML_HANDLE(block, *m_entry);

	// Match the typical host rounding mode so the FCSR fast path can skip
	// its synchronisation; the memory accesses below must still execute.
	UML_SETFMOD(block, 1);

	// UML_READ takes the specific fast path because SIZE_DWORD matches
	// the 32-bit native width of this space.
	for (int i = 0; i < COUNT; i++)
	{
		UML_MOV(block, I0, u32(READ_BASE + (i * 4)));
		UML_READ(block, I1, I0, SIZE_DWORD, SPACE_PROGRAM);
		UML_STORE(block, &m_read_results[i], 0, I1, SIZE_DWORD, SCALE_x1);
	}

	// Sub-native reads take the narrow specific path; each byte/word must
	// be extracted from the correct position in the native word.
	for (int i = 0; i < COUNT; i++)
	{
		for (int byte = 0; byte < 4; byte++)
		{
			UML_MOV(block, I0, u32(READ_BASE + (i * 4) + byte));
			UML_READ(block, I1, I0, SIZE_BYTE, SPACE_PROGRAM);
			UML_STORE(block, &m_read_byte_results[(i * 4) + byte], 0, I1, SIZE_DWORD, SCALE_x1);
		}

		for (int word = 0; word < 2; word++)
		{
			UML_MOV(block, I0, u32(READ_BASE + (i * 4) + (word * 2)));
			UML_READ(block, I1, I0, SIZE_WORD, SPACE_PROGRAM);
			UML_STORE(block, &m_read_word_results[(i * 2) + word], 0, I1, SIZE_DWORD, SCALE_x1);
		}
	}

	// A mask-sensitive register handler lets us check the synthesised
	// mask directly instead of relying on RAM (whose reads ignore it).
	UML_MOV(block, I0, u32(REGISTER_ADDRESS));
	UML_READ(block, I1, I0, SIZE_DWORD, SPACE_PROGRAM);

	UML_MOV(block, I0, u32(REGISTER_ADDRESS));
	UML_MOV(block, I1, REGISTER_WRITE_VALUE);
	UML_WRITE(block, I0, I1, SIZE_DWORD, SPACE_PROGRAM);

	// Narrow masked accesses must shift both the data and the mask into
	// the native word before calling the handler.
	UML_MOV(block, I0, u32(NARROW_READ_ADDRESS + 1));
	UML_READM(block, I1, I0, u32(0xff), SIZE_BYTE, SPACE_PROGRAM);
	UML_STORE(block, &m_narrow_read_result, 0, I1, SIZE_DWORD, SCALE_x1);

	UML_MOV(block, I0, u32(NARROW_WRITE_ADDRESS + 2));
	UML_MOV(block, I1, NARROW_WRITE_VALUE);
	UML_WRITEM(block, I0, I1, u32(NARROW_WRITE_MASK), SIZE_BYTE, SPACE_PROGRAM);

	// Indexed LOAD/STORE with sign-extended index and scale.
	UML_MOV(block, I0, 2);
	UML_MOV(block, I1, u32(0x13579bdf));
	UML_STORE(block, &m_indexed_storage[0], I0, I1, SIZE_DWORD, SCALE_x4);
	UML_LOAD(block, I2, &m_indexed_storage[0], I0, SIZE_DWORD, SCALE_x4);
	UML_STORE(block, &m_indexed_result, 0, I2, SIZE_DWORD, SCALE_x1);

	// UML_WRITE uses the same fast path and must not drop the data.
	for (int i = 0; i < COUNT; i++)
	{
		UML_MOV(block, I0, u32(WRITE_BASE + (i * 4)));
		UML_MOV(block, I1, WRITE_PATTERNS[i]);
		UML_WRITE(block, I0, I1, SIZE_DWORD, SPACE_PROGRAM);

		UML_MOV(block, I0, u32(NARROW_WRITE_BASE + (i * 4)));
		UML_MOV(block, I1, u32(0xa0 + i));
		UML_WRITE(block, I0, I1, SIZE_BYTE, SPACE_PROGRAM);

		UML_MOV(block, I0, u32(NARROW_WRITE_BASE + (i * 4) + 2));
		UML_MOV(block, I1, u32(0xb000 + i));
		UML_WRITE(block, I0, I1, SIZE_WORD, SPACE_PROGRAM);
	}

	UML_EXIT(block, 0);
	block.end();

	m_cache.codegen_complete();
	int const execute_result = m_drcuml->execute(*m_entry);
	if (execute_result != 0)
		throw emu_fatalerror("drc_specifictest: unexpected execute result %d\n", execute_result);

	int errors = 0;
	for (int i = 0; i < COUNT; i++)
	{
		offs_t const read_address = READ_BASE + (i * 4);
		u32 const read_actual = m_read_results[i];
		if (read_actual != READ_PATTERNS[i])
		{
			osd_printf_error("drc_specifictest: read at %05x: expected %08x, got %08x\n",
					read_address, READ_PATTERNS[i], read_actual);
			errors++;
		}

		offs_t const write_address = WRITE_BASE + (i * 4);
		u32 const write_actual = space.read_dword(write_address);
		if (write_actual != WRITE_PATTERNS[i])
		{
			osd_printf_error("drc_specifictest: write at %05x: expected %08x, got %08x\n",
					write_address, WRITE_PATTERNS[i], write_actual);
			errors++;
		}

		for (int byte = 0; byte < 4; byte++)
		{
			u32 const expected = (READ_PATTERNS[i] >> (8 * byte)) & 0xff;
			u32 const actual = m_read_byte_results[(i * 4) + byte];
			if (actual != expected)
			{
				osd_printf_error("drc_specifictest: byte read at %05x: expected %08x, got %08x\n",
						READ_BASE + (i * 4) + byte, expected, actual);
				errors++;
			}
		}

		for (int word = 0; word < 2; word++)
		{
			u32 const expected = (READ_PATTERNS[i] >> (16 * word)) & 0xffff;
			u32 const actual = m_read_word_results[(i * 2) + word];
			if (actual != expected)
			{
				osd_printf_error("drc_specifictest: word read at %05x: expected %08x, got %08x\n",
						READ_BASE + (i * 4) + (word * 2), expected, actual);
				errors++;
			}
		}

		u32 const narrow_expected = (NARROW_SEED & 0x0000ff00) | u32(0xa0 + i) | (u32(0xb000 + i) << 16);
		u32 const narrow_actual = space.read_dword(NARROW_WRITE_BASE + (i * 4));
		if (narrow_actual != narrow_expected)
		{
			osd_printf_error("drc_specifictest: narrow write at %05x: expected %08x, got %08x\n",
					NARROW_WRITE_BASE + (i * 4), narrow_expected, narrow_actual);
			errors++;
		}
	}

	if (g_narrow_read_mask != 0x0000ff00)
	{
		osd_printf_error("drc_specifictest: narrow read mem_mask: expected %08x, got %08x\n",
				0x0000ff00, g_narrow_read_mask);
		errors++;
	}
	u32 const narrow_read_expected = (NARROW_REGISTER_VALUE >> 8) & 0xff;
	if (m_narrow_read_result != narrow_read_expected)
	{
		osd_printf_error("drc_specifictest: narrow read result: expected %08x, got %08x\n",
				narrow_read_expected, m_narrow_read_result);
		errors++;
	}
	if ((m_indexed_storage[2] != 0x13579bdf) || (m_indexed_result != 0x13579bdf))
	{
		osd_printf_error("drc_specifictest: indexed load/store: expected %08x/%08x, got %08x/%08x\n",
				0x13579bdf, 0x13579bdf, m_indexed_storage[2], m_indexed_result);
		errors++;
	}
	if (g_narrow_write_mask != 0x000f0000)
	{
		osd_printf_error("drc_specifictest: narrow write mem_mask: expected %08x, got %08x\n",
				0x000f0000, g_narrow_write_mask);
		errors++;
	}
	u32 const narrow_write_expected = (NARROW_WRITE_VALUE & 0xff) << 16;
	if (g_narrow_write_data != narrow_write_expected)
	{
		osd_printf_error("drc_specifictest: narrow write data: expected %08x, got %08x\n",
				narrow_write_expected, g_narrow_write_data);
		errors++;
	}

	if (g_register_read_mask != FULL_DWORD_MASK)
	{
		osd_printf_error("drc_specifictest: register read mem_mask: expected %08x, got %08x\n",
				FULL_DWORD_MASK, g_register_read_mask);
		errors++;
	}
	if (g_register_write_mask != FULL_DWORD_MASK)
	{
		osd_printf_error("drc_specifictest: register write mem_mask: expected %08x, got %08x\n",
				FULL_DWORD_MASK, g_register_write_mask);
		errors++;
	}

	if (errors != 0)
		throw emu_fatalerror("drc_specifictest: %d specific-access mismatch(es)\n", errors);

	osd_printf_info("drc_specifictest: PASS (%d dword + %d byte + %d word reads, %d dword + %d narrow writes + mask-sensitive registers, 32-bit native space)\n",
			COUNT, COUNT * 4, COUNT * 2, COUNT, COUNT);

	machine().schedule_exit();
}


DEFINE_DEVICE_TYPE(DRC_SPECIFICTEST_MEM, drc_specifictest_mem_device, "drc_specifictest_mem", "DRC specific-access test memory")


ROM_START( drc_specifictest )
ROM_END


GAME(2026, drc_specifictest, 0, drc_specifictest, 0, drc_specifictest_state, empty_init, ROT0, "MAME", "DRC specific-access mask test", MACHINE_NO_SOUND | MACHINE_NOT_WORKING)
