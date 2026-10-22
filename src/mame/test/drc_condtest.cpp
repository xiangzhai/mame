// license:BSD-3-Clause
// copyright-holders:MAMEdev Team

/***************************************************************************

    drc_condtest.cpp

    Standalone test for the DRC condition codes.  This deliberately does
    not use a guest CPU: every condition is evaluated directly against
    every combination of the C, V, Z, S and U flags, both via SETc and
    via a conditional jump.  That keeps this independent of MIPS3 (or any
    other front-end) test coverage.

***************************************************************************/

#include "emu.h"
#include "emuopts.h"

#include "cpu/drcuml.h"
#include "cpu/drcumlsh.h"
#include "cpu/drccache.h"

#include <array>
#include <memory>


namespace {

class drc_condtest_state : public driver_device
{
public:
	drc_condtest_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_cache(16 * 1024 * 1024)
	{
	}

	void drc_condtest(machine_config &config)
	{
	}

protected:
	virtual void machine_start() override ATTR_COLD;

private:
	static constexpr int COND_COUNT = uml::COND_MAX - uml::COND_Z;
	static constexpr int FLAG_COUNT = 32;

	static bool condition_true(uml::condition_t cond, u8 flags);
	static char const *condition_name(uml::condition_t cond);
	void check_results(char const *what, std::array<u32, COND_COUNT * FLAG_COUNT> const &results, int &errors);

	drc_cache m_cache;
	std::unique_ptr<drcuml_state> m_drcuml;
	uml::code_handle *m_entry = nullptr;
	int m_label = 0;

	std::array<u32, COND_COUNT * FLAG_COUNT> m_set_results{};
	std::array<u32, COND_COUNT * FLAG_COUNT> m_jump_results{};
};


bool drc_condtest_state::condition_true(uml::condition_t cond, u8 flags)
{
	bool const c = bool(flags & uml::FLAG_C);
	bool const v = bool(flags & uml::FLAG_V);
	bool const z = bool(flags & uml::FLAG_Z);
	bool const s = bool(flags & uml::FLAG_S);
	bool const u = bool(flags & uml::FLAG_U);

	switch (cond)
	{
	case uml::COND_Z:  return z;
	case uml::COND_NZ: return !z;
	case uml::COND_S:  return s;
	case uml::COND_NS: return !s;
	case uml::COND_C:  return c;
	case uml::COND_NC: return !c;
	case uml::COND_V:  return v;
	case uml::COND_NV: return !v;
	case uml::COND_U:  return u;
	case uml::COND_NU: return !u;
	case uml::COND_A:  return !z && !c;
	case uml::COND_BE: return z || c;
	case uml::COND_G:  return !z && (s == v);
	case uml::COND_LE: return z || (s != v);
	case uml::COND_L:  return s != v;
	case uml::COND_GE: return s == v;
	default:
		return false;
	}
}


char const *drc_condtest_state::condition_name(uml::condition_t cond)
{
	switch (cond)
	{
	case uml::COND_Z:  return "Z";
	case uml::COND_NZ: return "NZ";
	case uml::COND_S:  return "S";
	case uml::COND_NS: return "NS";
	case uml::COND_C:  return "C";
	case uml::COND_NC: return "NC";
	case uml::COND_V:  return "V";
	case uml::COND_NV: return "NV";
	case uml::COND_U:  return "U";
	case uml::COND_NU: return "NU";
	case uml::COND_A:  return "A";
	case uml::COND_BE: return "BE";
	case uml::COND_G:  return "G";
	case uml::COND_LE: return "LE";
	case uml::COND_L:  return "L";
	case uml::COND_GE: return "GE";
	default:
		return "?";
	}
}


void drc_condtest_state::check_results(char const *what, std::array<u32, COND_COUNT * FLAG_COUNT> const &results, int &errors)
{
	int index = 0;
	for (int c = uml::COND_Z; c < uml::COND_MAX; c++)
	{
		uml::condition_t const cond = uml::condition_t(c);
		for (int f = 0; f < FLAG_COUNT; f++)
		{
			u32 const expected = condition_true(cond, u8(f)) ? 1 : 0;
			u32 const actual = results[index++];
			if (actual != expected)
			{
				if (errors < 32)
				{
					osd_printf_error("drc_condtest: %s %s with C=%d V=%d Z=%d S=%d U=%d: expected %u, got %u\n",
							what, condition_name(cond),
							(f >> uml::FLAG_BIT_C) & 1,
							(f >> uml::FLAG_BIT_V) & 1,
							(f >> uml::FLAG_BIT_Z) & 1,
							(f >> uml::FLAG_BIT_S) & 1,
							(f >> uml::FLAG_BIT_U) & 1,
							expected, actual);
				}
				errors++;
			}
		}
	}
}


void drc_condtest_state::machine_start()
{
	m_label = 0;
	m_cache.allocate_cache(mconfig().options().drc_rwx());

	uint32_t const flags = 0;
	m_drcuml = std::make_unique<drcuml_state>(*this, m_cache, flags, 8, 32, 2, 0x1000);
	m_entry = m_drcuml->handle_alloc("entry");

	drcuml_block &block = m_drcuml->begin_block(16384);
	UML_HANDLE(block, *m_entry);

	int index = 0;
	for (int c = uml::COND_Z; c < uml::COND_MAX; c++)
	{
		uml::condition_t const cond = uml::condition_t(c);
		for (int f = 0; f < FLAG_COUNT; f++)
		{
			// direct condition evaluation (SETc)
			UML_SETFLGS(block, f);
			UML_SETc(block, cond, I0);
			UML_STORE(block, &m_set_results[index], 0, I0, SIZE_DWORD, SCALE_x1);

			// conditional branch path (emit_skip)
			uml::code_label const taken = m_label++;
			uml::code_label const done = m_label++;
			UML_SETFLGS(block, f);
			UML_JMPc(block, cond, taken);
			UML_MOV(block, I0, 0);
			UML_JMP(block, done);
			UML_LABEL(block, taken);
			UML_MOV(block, I0, 1);
			UML_LABEL(block, done);
			UML_STORE(block, &m_jump_results[index], 0, I0, SIZE_DWORD, SCALE_x1);

			index++;
		}
	}

	UML_EXIT(block, 0);
	block.end();

	m_cache.codegen_complete();
	int const execute_result = m_drcuml->execute(*m_entry);
	if (execute_result != 0)
		throw emu_fatalerror("drc_condtest: unexpected execute result %d\n", execute_result);

	int errors = 0;
	check_results("SETc", m_set_results, errors);
	check_results("JMPc", m_jump_results, errors);

	if (errors != 0)
		throw emu_fatalerror("drc_condtest: %d condition/flag mismatch(es)\n", errors);

	osd_printf_info("drc_condtest: PASS (%d conditions x %d flag combinations x 2 paths)\n",
			COND_COUNT, FLAG_COUNT);

	machine().schedule_exit();
}


ROM_START( drc_condtest )
ROM_END


} // anonymous namespace


GAME(2026, drc_condtest, 0, drc_condtest, 0, drc_condtest_state, empty_init, ROT0, "MAME", "DRC condition code test", MACHINE_NO_SOUND | MACHINE_NOT_WORKING)
