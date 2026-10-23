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
#include <bit>
#include <memory>


namespace {

class drc_condtest_state : public driver_device
{
public:
	drc_condtest_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
		, m_cache(64 * 1024 * 1024)
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

	static constexpr int MAX_FUSION_CASES = 2048;

	static bool condition_true(uml::condition_t cond, u8 flags);
	static char const *condition_name(uml::condition_t cond);
	static u8 sub_flags(u64 a, u64 b, bool dsize);
	static u8 and_flags(u64 value, bool dsize);
	void check_results(char const *what, std::array<u32, COND_COUNT * FLAG_COUNT> const &results, int &errors);

	drc_cache m_cache;
	std::unique_ptr<drcuml_state> m_drcuml;
	uml::code_handle *m_entry = nullptr;
	uml::code_handle *m_aux = nullptr;
	int m_label = 0;
	int m_fusion_count = 0;

	u32 m_cfg_result = 0;
	std::array<u32, COND_COUNT * FLAG_COUNT> m_set_results{};
	std::array<u32, COND_COUNT * FLAG_COUNT> m_jump_results{};
	std::array<u32, MAX_FUSION_CASES> m_fusion_results{};
	std::array<u32, MAX_FUSION_CASES> m_fusion_expected{};
	std::array<u64, MAX_FUSION_CASES> m_fusion_operands{};

	static constexpr int ALU_RESULT_COUNT = 28;
	std::array<u32, ALU_RESULT_COUNT> m_alu_results{};
	std::array<u32, ALU_RESULT_COUNT> m_alu_extra{};

	static constexpr int ALU64_RESULT_COUNT = 8;
	std::array<u64, ALU64_RESULT_COUNT> m_alu64_results{};
	std::array<u64, ALU64_RESULT_COUNT> m_alu64_extra{};
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


u8 drc_condtest_state::sub_flags(u64 a, u64 b, bool dsize)
{
	u8 flags = 0;
	if (!dsize)
	{
		u32 const aa = u32(a);
		u32 const bb = u32(b);
		u32 const rr = aa - bb;
		if (bb > aa)
			flags |= uml::FLAG_C;
		if (u32((aa ^ bb) & (aa ^ rr)) >> 31)
			flags |= uml::FLAG_V;
		if (!rr)
			flags |= uml::FLAG_Z;
		if (rr >> 31)
			flags |= uml::FLAG_S;
	}
	else
	{
		u64 const rr = a - b;
		if (b > a)
			flags |= uml::FLAG_C;
		if (u64((a ^ b) & (a ^ rr)) >> 63)
			flags |= uml::FLAG_V;
		if (!rr)
			flags |= uml::FLAG_Z;
		if (rr >> 63)
			flags |= uml::FLAG_S;
	}
	return flags;
}


u8 drc_condtest_state::and_flags(u64 value, bool dsize)
{
	if (!dsize)
		value = u32(value);
	u8 flags = 0;
	if (!value)
		flags |= uml::FLAG_Z;
	if (value >> (dsize ? 63 : 31))
		flags |= uml::FLAG_S;
	return flags;
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
	m_aux = m_drcuml->handle_alloc("aux");

	drcuml_block &block = m_drcuml->begin_block(65536);
	UML_HANDLE(block, *m_entry);

	int index = 0;
	for (int c = uml::COND_Z; c < uml::COND_MAX; c++)
	{
		uml::condition_t const cond = uml::condition_t(c);
		for (int f = 0; f < FLAG_COUNT; f++)
		{
			// direct condition evaluation (SETc)
			UML_SETFLGS(block, f);
			UML_SETc(block, cond, uml::I0);
			UML_STORE(block, &m_set_results[index], 0, uml::I0, SIZE_DWORD, SCALE_x1);

			// conditional branch path (emit_skip)
			uml::code_label const taken = m_label++;
			uml::code_label const done = m_label++;
			UML_SETFLGS(block, f);
			UML_JMPc(block, cond, taken);
			UML_MOV(block, uml::I0, 0);
			UML_JMP(block, done);
			UML_LABEL(block, taken);
			UML_MOV(block, uml::I0, 1);
			UML_LABEL(block, done);
			UML_STORE(block, &m_jump_results[index], 0, uml::I0, SIZE_DWORD, SCALE_x1);

			index++;
		}
	}

	uml::condition_t const cmp_conditions[] =
	{
		uml::COND_Z, uml::COND_NZ, uml::COND_S, uml::COND_NS,
		uml::COND_C, uml::COND_NC, uml::COND_V, uml::COND_NV,
		uml::COND_A, uml::COND_BE, uml::COND_G, uml::COND_LE,
		uml::COND_L, uml::COND_GE
	};
	uml::condition_t const test_conditions[] =
	{
		uml::COND_Z, uml::COND_NZ, uml::COND_S, uml::COND_NS
	};
	u64 const values[][2] =
	{
		{ 0, 0 },
		{ 1, 2 },
		{ 2, 1 },
		{ 0x12345678, 0x12345678 },
		{ 0xffffffff, 1 },
		{ 0x80000000, 0x7fffffff },
		{ 0xffffffffffffffffull, 1 },
		{ 0x8000000000000000ull, 0x7fffffffffffffffull }
	};

	auto emit_fusion_case = [&](bool is_cmp, bool dsize, uml::condition_t cond, int form, u64 a, u64 b)
	{
		if (m_fusion_count >= MAX_FUSION_CASES)
			throw emu_fatalerror("drc_condtest: too many fusion cases\n");

		int const case_index = m_fusion_count++;
		m_fusion_operands[case_index] = b;
		if (dsize)
		{
			block.append().dmov(uml::I0, a);
			block.append().dmov(uml::I1, b);
		}
		else
		{
			block.append().mov(uml::I0, u32(a));
			block.append().mov(uml::I1, u32(b));
		}

		UML_SETFLGS(block, 0);

		uml::parameter imm_a = a;
		uml::parameter imm_b = b;
		uml::parameter mem_b = uml::mem(&m_fusion_operands[case_index]);
		if (is_cmp)
		{
			switch (form)
			{
			case 0: dsize ? block.append().dcmp(uml::I0, uml::I1) : block.append().cmp(uml::I0, uml::I1); break;
			case 1: dsize ? block.append().dcmp(uml::I0, imm_b) : block.append().cmp(uml::I0, imm_b); break;
			case 2: dsize ? block.append().dcmp(imm_a, uml::I1) : block.append().cmp(imm_a, uml::I1); break;
			case 3: dsize ? block.append().dcmp(imm_a, imm_b) : block.append().cmp(imm_a, imm_b); break;
			case 4: dsize ? block.append().dcmp(uml::I0, mem_b) : block.append().cmp(uml::I0, mem_b); break;
			case 5: dsize ? block.append().dcmp(mem_b, imm_a) : block.append().cmp(mem_b, imm_a); break;
			case 6: dsize ? block.append().dcmp(uml::I0, uml::I0) : block.append().cmp(uml::I0, uml::I0); break;
			default: dsize ? block.append().dcmp(mem_b, mem_b) : block.append().cmp(mem_b, mem_b); break;
			}
		}
		else
		{
			switch (form)
			{
			case 0: dsize ? block.append().dtest(uml::I0, uml::I1) : block.append().test(uml::I0, uml::I1); break;
			case 1: dsize ? block.append().dtest(uml::I0, imm_b) : block.append().test(uml::I0, imm_b); break;
			case 2: dsize ? block.append().dtest(imm_a, imm_b) : block.append().test(imm_a, imm_b); break;
			default: dsize ? block.append().dtest(uml::I0, mem_b) : block.append().test(uml::I0, mem_b); break;
			}
		}

		uml::code_label const taken = m_label++;
		uml::code_label const done = m_label++;
		UML_JMPc(block, cond, taken);
		UML_MOV(block, uml::I2, 0);
		UML_JMP(block, done);
		UML_LABEL(block, taken);
		UML_MOV(block, uml::I2, 1);
		UML_LABEL(block, done);
		UML_STORE(block, &m_fusion_results[case_index], 0, uml::I2, SIZE_DWORD, SCALE_x1);

		u64 cmp_a = a;
		u64 cmp_b = b;
		if (is_cmp)
		{
			switch (form)
			{
			case 5: cmp_a = b; cmp_b = a; break;
			case 6: cmp_b = a; break;
			case 7: cmp_a = b; break;
			default: break;
			}
		}
		u8 const fused_flags = is_cmp ? sub_flags(cmp_a, cmp_b, dsize) : and_flags(a & b, dsize);
		m_fusion_expected[case_index] = condition_true(cond, fused_flags) ? 1 : 0;
	};

	for (auto const &value : values)
	{
		for (bool const dsize : { false, true })
		{
			for (uml::condition_t const cond : cmp_conditions)
				for (int form = 0; form < 8; form++)
					emit_fusion_case(true, dsize, cond, form, value[0], value[1]);

			for (uml::condition_t const cond : test_conditions)
				for (int form = 0; form < 4; form++)
					emit_fusion_case(false, dsize, cond, form, value[0], value[1]);
		}
	}

	{
		uml::code_label const target = m_label++;
		uml::code_label const done = m_label++;

		UML_SETFLGS(block, 0);
		UML_MOV(block, uml::I0, 1);
		UML_MOV(block, uml::I1, 2);
		UML_CMP(block, uml::I0, uml::I1);
		UML_JMPc(block, uml::COND_Z, target);
		UML_HANDLE(block, *m_aux);
		UML_SETc(block, uml::COND_S, uml::I2);
		UML_JMP(block, done);
		UML_LABEL(block, target);
		UML_MOV(block, uml::I2, 7);
		UML_LABEL(block, done);
		UML_STORE(block, &m_cfg_result, 0, uml::I2, SIZE_DWORD, SCALE_x1);
	}

	// ALU immediate/bitfield/multiply/divide lowering.  These exercise the
	// backend's specialised paths and are checked against independently
	// computed C++ results below.
	int ai = 0;
	auto save_i0 = [&](int index) { UML_STORE(block, &m_alu_results[index], 0, uml::I0, SIZE_DWORD, SCALE_x1); };
	auto save_i1 = [&](int index) { UML_STORE(block, &m_alu_extra[index], 0, uml::I1, SIZE_DWORD, SCALE_x1); };

	UML_MOV(block, uml::I0, u32(0x12345678));
	UML_ADD(block, uml::I0, uml::I0, u32(0x123));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x12345678));
	UML_SUB(block, uml::I0, uml::I0, u32(0x123));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0xdeadbeef));
	UML_AND(block, uml::I0, uml::I0, u32(0x7f));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0xdeadbeef));
	UML_AND(block, uml::I0, uml::I0, u32(0x00ff00ff));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x12345678));
	UML_OR(block, uml::I0, uml::I0, u32(0x7f));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x12345678));
	UML_OR(block, uml::I0, uml::I0, u32(0xffffffff));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x12345678));
	UML_XOR(block, uml::I0, uml::I0, u32(0x7f));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x12345678));
	UML_XOR(block, uml::I0, uml::I0, u32(0xffffffff));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x92345678));
	UML_SHL(block, uml::I0, uml::I0, u32(3));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x92345678));
	UML_SHR(block, uml::I0, uml::I0, u32(5));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x92345678));
	UML_SAR(block, uml::I0, uml::I0, u32(7));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x92345678));
	UML_ROR(block, uml::I0, uml::I0, u32(9));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x92345678));
	UML_ROL(block, uml::I0, uml::I0, u32(11));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0xdeadbeef));
	UML_BFXU(block, uml::I0, uml::I0, u32(4), u32(8));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0xdeadbeef));
	UML_BFXU(block, uml::I0, uml::I0, u32(28), u32(8));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0xdeadbeef));
	UML_BFXS(block, uml::I0, uml::I0, u32(4), u32(8));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0xdeadbeef));
	UML_BFXS(block, uml::I0, uml::I0, u32(28), u32(8));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0xdeadbeef));
	UML_ROLAND(block, uml::I0, uml::I0, u32(4), u32(0x00ff0000));
	save_i0(ai++);

	UML_MOV(block, uml::I0, u32(0x11223344));
	UML_MOV(block, uml::I1, u32(0xdeadbeef));
	UML_ROLINS(block, uml::I0, uml::I1, u32(4), u32(0x00ff0000));
	save_i0(ai++);

	UML_MOV(block, uml::I2, u32(0x12345678));
	UML_MOV(block, uml::I3, u32(0x9abcdef0));
	UML_MULU(block, uml::I0, uml::I1, uml::I2, uml::I3);
	save_i0(ai++);
	save_i1(ai - 1);

	UML_MOV(block, uml::I2, u32(0x12345678));
	UML_MOV(block, uml::I3, u32(0x9abcdef0));
	UML_MULULW(block, uml::I0, uml::I2, uml::I3);
	save_i0(ai++);

	UML_MOV(block, uml::I2, u32(0x80000001));
	UML_MOV(block, uml::I3, u32(0x12345678));
	UML_MULS(block, uml::I0, uml::I1, uml::I2, uml::I3);
	save_i0(ai++);
	save_i1(ai - 1);

	UML_MOV(block, uml::I2, u32(0x89abcdef));
	UML_MOV(block, uml::I3, u32(0x00012345));
	UML_DIVU(block, uml::I0, uml::I1, uml::I2, uml::I3);
	save_i0(ai++);
	save_i1(ai - 1);

	UML_MOV(block, uml::I2, u32(0xf9abcdef));
	UML_MOV(block, uml::I3, u32(0x00012345));
	UML_DIVS(block, uml::I0, uml::I1, uml::I2, uml::I3);
	save_i0(ai++);
	save_i1(ai - 1);

	int a64 = 0;
	auto save64 = [&](int index) { UML_DSTORE(block, &m_alu64_results[index], 0, uml::I0, SIZE_QWORD, SCALE_x1); };
	auto save64_extra = [&](int index) { UML_DSTORE(block, &m_alu64_extra[index], 0, uml::I1, SIZE_QWORD, SCALE_x1); };

	UML_DMOV(block, uml::I0, u64(0x123456789abcdef0));
	UML_DADD(block, uml::I0, uml::I0, 0x1234);
	save64(a64++);

	UML_DMOV(block, uml::I0, u64(0x123456789abcdef0));
	UML_DSUB(block, uml::I0, uml::I0, 0x1234);
	save64(a64++);

	UML_DMOV(block, uml::I0, u64(0xfedcba9876543210));
	UML_DAND(block, uml::I0, uml::I0, u64(0x00ff00ff00ff00ff));
	save64(a64++);

	UML_DMOV(block, uml::I0, u64(0x923456789abcdef0));
	UML_DSHL(block, uml::I0, uml::I0, 7);
	save64(a64++);

	UML_DMOV(block, uml::I0, u64(0xfedcba9876543210));
	UML_DBFXU(block, uml::I0, uml::I0, 8, 16);
	save64(a64++);

	UML_DMOV(block, uml::I0, u64(0x0123456789abcdef));
	UML_DROLAND(block, uml::I0, uml::I0, 20, u64(0x0000ffff00000000));
	save64(a64++);

	UML_DMOV(block, uml::I2, u64(0x123456789abcdef0));
	UML_DMOV(block, uml::I3, u64(0x0fedcba987654321));
	UML_DMULU(block, uml::I0, uml::I1, uml::I2, uml::I3);
	save64(a64++);
	save64_extra(a64 - 1);

	UML_DMOV(block, uml::I0, u64(0x923456789abcdef0));
	UML_DSHR(block, uml::I0, uml::I0, 9);
	save64(a64++);

	UML_EXIT(block, 0);
	block.end();

	m_cache.codegen_complete();
	int const execute_result = m_drcuml->execute(*m_entry);
	if (execute_result != 0)
		throw emu_fatalerror("drc_condtest: unexpected execute result %d\n", execute_result);

	int errors = 0;
	check_results("SETc", m_set_results, errors);
	check_results("JMPc", m_jump_results, errors);

	for (int fusion_index = 0; fusion_index < m_fusion_count; fusion_index++)
	{
		if (m_fusion_results[fusion_index] != m_fusion_expected[fusion_index])
		{
			if (errors < 32)
				osd_printf_error("drc_condtest: CMP/TEST case %d: expected %u, got %u\n",
						fusion_index, m_fusion_expected[fusion_index], m_fusion_results[fusion_index]);
			errors++;
		}
	}

	{
		auto const check_alu = [&](int index, u32 expected, char const *what)
		{
			if (m_alu_results[index] != expected)
			{
				osd_printf_error("drc_condtest: %s: expected %08x, got %08x\n",
						what, expected, m_alu_results[index]);
				errors++;
			}
		};
		auto const check_alu_extra = [&](int index, u32 expected, char const *what)
		{
			if (m_alu_extra[index] != expected)
			{
				osd_printf_error("drc_condtest: %s: expected %08x, got %08x\n",
						what, expected, m_alu_extra[index]);
				errors++;
			}
		};
		auto const bfxu = [](u32 src, u32 shift, u32 width)
		{
			return std::rotr(src, (shift + width) & 31) >> ((-int32_t(width)) & 31);
		};
		auto const bfxs = [](u32 src, u32 shift, u32 width)
		{
			return u32(int32_t(std::rotr(src, (shift + width) & 31)) >> ((-int32_t(width)) & 31));
		};

		check_alu(0, u32(0x12345678 + 0x123), "ADD immediate");
		check_alu(1, u32(0x12345678 - 0x123), "SUB immediate");
		check_alu(2, u32(0xdeadbeef & 0x7f), "AND 12-bit immediate");
		check_alu(3, u32(0xdeadbeef & 0x00ff00ff), "AND wide immediate");
		check_alu(4, u32(0x12345678 | 0x7f), "OR 12-bit immediate");
		check_alu(5, u32(0xffffffff), "OR all ones");
		check_alu(6, u32(0x12345678 ^ 0x7f), "XOR 12-bit immediate");
		check_alu(7, u32(~0x12345678), "XOR all ones");
		check_alu(8, u32(0x92345678) << 3, "SHL immediate");
		check_alu(9, u32(0x92345678) >> 5, "SHR immediate");
		check_alu(10, u32(int32_t(0x92345678) >> 7), "SAR immediate");
		check_alu(11, std::rotr(u32(0x92345678), 9), "ROR immediate");
		check_alu(12, std::rotl(u32(0x92345678), 11), "ROL immediate");
		check_alu(13, bfxu(0xdeadbeef, 4, 8), "BFXU immediate");
		check_alu(14, bfxu(0xdeadbeef, 28, 8), "BFXU wrapped");
		check_alu(15, bfxs(0xdeadbeef, 4, 8), "BFXS immediate");
		check_alu(16, bfxs(0xdeadbeef, 28, 8), "BFXS wrapped");
		check_alu(17, std::rotl(u32(0xdeadbeef), 4) & 0x00ff0000, "ROLAND immediate");
		check_alu(18, (0x11223344 & ~u32(0x00ff0000)) | (std::rotl(u32(0xdeadbeef), 4) & 0x00ff0000), "ROLINS immediate");

		u64 const mulu = u64(0x12345678) * u64(0x9abcdef0);
		check_alu(19, u32(mulu), "MULU low");
		check_alu_extra(19, u32(mulu >> 32), "MULU high");
		check_alu(20, u32(mulu), "MULULW");

		s64 const muls = s64(int32_t(0x80000001)) * s64(int32_t(0x12345678));
		check_alu(21, u32(muls), "MULS low");
		check_alu_extra(21, u32(u64(muls) >> 32), "MULS high");

		u32 const divu_q = 0x89abcdef / 0x00012345;
		u32 const divu_r = 0x89abcdef % 0x00012345;
		check_alu(22, divu_q, "DIVU quotient");
		check_alu_extra(22, divu_r, "DIVU remainder");

		int32_t const divs_a = int32_t(0xf9abcdef);
		int32_t const divs_b = int32_t(0x00012345);
		check_alu(23, u32(divs_a / divs_b), "DIVS quotient");
		check_alu_extra(23, u32(divs_a % divs_b), "DIVS remainder");

		auto const check_alu64 = [&](int index, u64 expected, char const *what)
		{
			if (m_alu64_results[index] != expected)
			{
				osd_printf_error("drc_condtest: %s: expected %016llx, got %016llx\n",
						what, static_cast<unsigned long long>(expected),
						static_cast<unsigned long long>(m_alu64_results[index]));
				errors++;
			}
		};
		auto const check_alu64_extra = [&](int index, u64 expected, char const *what)
		{
			if (m_alu64_extra[index] != expected)
			{
				osd_printf_error("drc_condtest: %s: expected %016llx, got %016llx\n",
						what, static_cast<unsigned long long>(expected),
						static_cast<unsigned long long>(m_alu64_extra[index]));
				errors++;
			}
		};
		auto const bfxu64 = [](u64 src, u32 shift, u32 width)
		{
			return std::rotr(src, (shift + width) & 63) >> ((-int32_t(width)) & 63);
		};

		check_alu64(0, u64(0x123456789abcdef0) + 0x1234, "64-bit ADD immediate");
		check_alu64(1, u64(0x123456789abcdef0) - 0x1234, "64-bit SUB immediate");
		check_alu64(2, u64(0xfedcba9876543210) & u64(0x00ff00ff00ff00ff), "64-bit AND immediate");
		check_alu64(3, u64(0x923456789abcdef0) << 7, "64-bit SHL immediate");
		check_alu64(4, bfxu64(0xfedcba9876543210, 8, 16), "64-bit BFXU immediate");
		check_alu64(5, std::rotl(u64(0x0123456789abcdef), 20) & u64(0x0000ffff00000000), "64-bit ROLAND immediate");

		unsigned __int128 const mulu64 = (unsigned __int128)u64(0x123456789abcdef0) * u64(0x0fedcba987654321);
		check_alu64(6, u64(mulu64), "64-bit MULU low");
		check_alu64_extra(6, u64(mulu64 >> 64), "64-bit MULU high");
		check_alu64(7, u64(0x923456789abcdef0) >> 9, "64-bit SHR immediate");
	}

	if (m_cfg_result != 1)
	{
		osd_printf_error("drc_condtest: CFG liveness case: expected 1, got %u\n", m_cfg_result);
		errors++;
	}

	if (errors != 0)
		throw emu_fatalerror("drc_condtest: %d condition/flag mismatch(es)\n", errors);

	osd_printf_info("drc_condtest: PASS (%d conditions x %d flag combinations x 2 paths, %d CMP/TEST + branch cases, %d ALU cases)\n",
			COND_COUNT, FLAG_COUNT, m_fusion_count, ALU_RESULT_COUNT + ALU64_RESULT_COUNT);

	machine().schedule_exit();
}


ROM_START( drc_condtest )
ROM_END


} // anonymous namespace


GAME(2026, drc_condtest, 0, drc_condtest, 0, drc_condtest_state, empty_init, ROT0, "MAME", "DRC condition code test", MACHINE_NO_SOUND | MACHINE_NOT_WORKING)
