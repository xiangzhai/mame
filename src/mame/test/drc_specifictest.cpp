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
	static constexpr offs_t REGISTER_ADDRESS = 0x0300;
	static constexpr int COUNT = 8;
	static constexpr u32 REGISTER_READ_VALUE = 0xcafef00d;
	static constexpr u32 REGISTER_WRITE_VALUE = 0x13579bdf;
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

	required_device<drc_specifictest_mem_device> m_mem;
	drc_cache m_cache;
	std::unique_ptr<drcuml_state> m_drcuml;
	uml::code_handle *m_entry = nullptr;

	std::array<u32, COUNT> m_read_results{};
};


void drc_specifictest_state::machine_start()
{
	m_cache.allocate_cache(mconfig().options().drc_rwx());

	g_register_read_mask = 0;
	g_register_write_mask = 0;

	address_space &space = m_mem->space(AS_PROGRAM);
	address_space::specific_access_info const accessors = space.specific_accessors();
	if (accessors.native_bytes != 4)
		throw emu_fatalerror("drc_specifictest: expected a 32-bit native space, got %u bytes\n", accessors.native_bytes);

	// Seed the backing memory through the normal C++ accessors.  The
	// generated code has to see these values through the fast path.
	for (int i = 0; i < COUNT; i++)
		space.write_dword(READ_BASE + (i * 4), READ_PATTERNS[i]);

	uint32_t const flags = 0;
	m_drcuml = std::make_unique<drcuml_state>(*m_mem, m_cache, flags, 8, 16, 0, 0x1000);
	m_entry = m_drcuml->handle_alloc("entry");

	drcuml_block &block = m_drcuml->begin_block(1024);
	UML_HANDLE(block, *m_entry);

	// UML_READ takes the specific fast path because SIZE_DWORD matches
	// the 32-bit native width of this space.
	for (int i = 0; i < COUNT; i++)
	{
		UML_MOV(block, I0, u32(READ_BASE + (i * 4)));
		UML_READ(block, I1, I0, SIZE_DWORD, SPACE_PROGRAM);
		UML_STORE(block, &m_read_results[i], 0, I1, SIZE_DWORD, SCALE_x1);
	}

	// A mask-sensitive register handler lets us check the synthesised
	// mask directly instead of relying on RAM (whose reads ignore it).
	UML_MOV(block, I0, u32(REGISTER_ADDRESS));
	UML_READ(block, I1, I0, SIZE_DWORD, SPACE_PROGRAM);

	UML_MOV(block, I0, u32(REGISTER_ADDRESS));
	UML_MOV(block, I1, REGISTER_WRITE_VALUE);
	UML_WRITE(block, I0, I1, SIZE_DWORD, SPACE_PROGRAM);

	// UML_WRITE uses the same fast path and must not drop the data.
	for (int i = 0; i < COUNT; i++)
	{
		UML_MOV(block, I0, u32(WRITE_BASE + (i * 4)));
		UML_MOV(block, I1, WRITE_PATTERNS[i]);
		UML_WRITE(block, I0, I1, SIZE_DWORD, SPACE_PROGRAM);
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

	osd_printf_info("drc_specifictest: PASS (%d dword reads + %d dword writes + mask-sensitive register, 32-bit native space)\n",
			COUNT, COUNT);

	machine().schedule_exit();
}


DEFINE_DEVICE_TYPE(DRC_SPECIFICTEST_MEM, drc_specifictest_mem_device, "drc_specifictest_mem", "DRC specific-access test memory")


ROM_START( drc_specifictest )
ROM_END


GAME(2026, drc_specifictest, 0, drc_specifictest, 0, drc_specifictest_state, empty_init, ROT0, "MAME", "DRC specific-access mask test", MACHINE_NO_SOUND | MACHINE_NOT_WORKING)
