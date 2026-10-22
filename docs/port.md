# MAME DRC LoongArch64 后端移植报告

## 0. 结论与前提

> **实施状态：已完成。** 后端位于 `src/devices/cpu/drcbela64.{h,cpp}`，使用 lagoon v1.0.0，已接入构建，完成 MIPS3 整数/浮点 A/B 回归，并通过 STV/SH-2 游戏 `diehard` 实测确认原生 JIT 实际执行；详细改动和验证结果见第 9 节。

可以移植，但这是一项大工程：现有 ARM64 后端 5.7k 行、x86-64 后端 6.6k 行，LoongArch64 后端大概也要 4–5k 行。指令编码器不需要自己写，直接用 Loongson 社区的 `lagoon` 汇编/反汇编库（见第 4 节）。

先说明一个前提：**MAME 目前在 LoongArch 上会自动回退到 `drcbe_c`（可移植 C 后端）**，功能正确，只是慢；`src/devices/cpu/drcuml.cpp:64` 的 `NATIVE_DRC` 默认分支就是 `drcbe_c`。

## 1. DRC 的分层与后端接口

DRC 的三层是：

```
CPU 前端 drcfe       ->  UML IR  ->  后端 drcbe_*  ->  宿主机器码
```

后端只需要实现 `drcbe_interface`（`src/devices/cpu/drcuml.h:151`）：

```cpp
virtual void reset() = 0;
virtual int  execute(uml::code_handle &entry) = 0;
virtual void generate(drcuml_block &block,
                      uml::instruction const *instlist, u32 numinst) = 0;
virtual bool hash_exists(u32 mode, u32 pc) const noexcept = 0;
virtual void hash_invalidate_range(u32 pcstart, u32 pcend) noexcept = 0;
virtual void get_info(drcbe_info &info) const noexcept = 0;
```

UML 只有 10 个整数寄存器 `I0-I9`、10 个浮点寄存器 `F0-F9`，加上 `flags`、`exp`、`fmod`；指令约 60 种，带尺寸/条件变体后 `uml.h` 里有 243 个枚举项。完整语义见 `docs/source/techspecs/uml_instructions.rst`，而 **`drcbec.cpp` 是最权威的可执行语义参考**。

## 2. 参考实现的选择

- **不要从 `drcbex64.cpp` 开始**，x86 的 flags/变长指令和 LoongArch 差太远。
- 用 **`drcbearm64.cpp`** 当骨架：64 位 RISC、显式软件模拟 flags、near cache 基址、helper 调用、异常入口都有现成结构。
- 用 **`drcbec.cpp`** 当行为规范，尤其是有符号/无符号、除零、移位进位这些边界。
- 复用 `drcbeut.h` 里的 `drc_hash_table`、`drc_map_variables`、`resolved_member_function`，这些都是宿主无关的。

## 3. 核心设计决策

### 3.1 寄存器映射

LoongArch64 ABI（LP64D）：

| 用途 | 寄存器 |
|---|---|
| zero / ra / tp / sp | `$r0` / `$r1` / `$r2` / `$r3` |
| 参数 | `$a0-$a7` = `$r4-$r11` |
| 临时 | `$t0-$t8` = `$r12-$r20` |
| 保留 | `$r21`（不要用） |
| frame pointer | `$fp` = `$r22`（callee-saved） |
| callee-saved | `$s0-$s8` = `$r23-$r31` |
| 浮点 | `$f0-$f31`；`$fa0-$fa7`、`$ft0-$ft15`、`$fs0-$fs7` |

建议：

```cpp
I0..I7   -> $s0..$s7      // 8 个直接映射，够 MIPS3/PPC 前端用
FLAGS    -> $s8
BASE     -> $fp ($r22)    // 指向 cache.near()+0x100，callee-saved，helper 调用不会破坏
TEMP     -> $t0..$t8
F0..F7   -> $fs0..$fs7
```

`get_info()` 返回 `direct_iregs=8, direct_fregs=8`；MIPS3/PPC 前端会根据这个值把 guest 寄存器放进直接映射（见 `src/devices/cpu/mips/mips3.cpp:589`、`powerpc/ppccom.cpp:945`）。`$r21` 和 `$tp` 不要碰。

### 3.2 flags 和进位（最大难点）

LoongArch **没有条件码寄存器，也没有带进位的加减法**。做法和 ARM64 后端一样：在 `$s8` 里软件维护和 UML 相同的位布局：

```cpp
bit0 = C, bit1 = V, bit2 = Z, bit3 = S, bit4 = U   // 与 uml::FLAG_BIT_* 一致
```

需要自己实现：

- `store_carry/load_carry`：`sltu` / `andi`
- `set_flags`：从结果和操作数算 Z/N/C/V
- `emit_skip`：按条件位 `andi` + `bnez/beqz`
- `ADDC/SBB`：用 `sltu` 合成进位链，例如
  `t = a + b; c1 = t < a; u = t + cin; c2 = u < t; carry = c1 | c2`
- `ROLC/RORC`、移位产生的 carry：LoongArch 没有 ARM 的 `S` 后缀，只能显式算

优化点：LoongArch 的条件分支直接比较两个寄存器（`beq/bne/blt/bge/bltu/bgeu`），可以在 `CMP/TEST` 后做 peephole，直接把条件分支翻成比较分支，省掉 flags 的读写。第一版先老老实实写 flags，正确性优先。

### 3.3 32 位语义陷阱

UML 默认 32 位，且 C/ARM64 后端把 32 位结果按**零扩展**存进 64 位 UML 寄存器。LoongArch 的 `add.w/sub.w/slli.w/...` 结果是**符号扩展**到 64 位的。所以每条 32 位整数运算后通常要补一条：

```asm
bstrpick.d rd, rd, 31, 0     // 取低 32 位并零扩展
```

否则 `I` 寄存器高 32 位会带符号位，后续 64 位地址计算/比较会错。这一点很容易漏，建议在 `mov_param_*` / 写回路径统一处理。

### 3.4 常量、地址和分支范围

- 32 位常量：`la_load_immediate32()`（或 `la_li_d()`）
- 任意 64 位常量：`la_load_immediate64()`，最多 4 条指令（`lu12i.w`/`ori`/`lu32i.d`/`lu52i.d`）
- 近地址（near cache、`m_state`）：`la_la_local()` 生成 `pcalau12i` + `addi.d`，范围 ±2 GiB；`m_baseptr` 和 near cache 在同一块虚拟分配里
- 从 near cache 加载指针（helper 的 `{obj, func}`）：`la_la_global()` 生成 `pcalau12i` + `ld.d`
- **分支范围**：`b/bl` 是 26 位偏移（±128 MiB），`beq` 等条件分支是 16 位偏移（**只有 ±128 KiB**）。UML 的 `JMP` 只跳当前 block 内的 label（`uml_instructions.rst:327`），正常 block 不会超范围；但 `EXIT/HASHJMP/CALLH/HANDLE` 这类跳到 block 外的目标，一律用“`la_load_immediate64` 装入目标地址 + `la_jirl(LA_ZERO, reg, 0)`”的间接跳转
- lagoon 的 `la_bind()` 回填分支偏移时**不做范围检查**（立即数越界只在 `LA_DEBUG` 下断言，本仓库已开启），所以更不能拿 16 位条件分支去跳远端

### 3.5 内存访问

直接照搬 ARM64 的 `emit_memaccess_setup` / `emit_narrow_memread` / `emit_narrow_memwrite` 结构：

- UML `READ/WRITE/READM/WRITEM` 走 `m_memory_accessors`（`resolved_member_function` + `specific_access_info`）的快速路径
- 32 位窄访问、非对齐、mask 情况走 helper
- `LOAD/STORE` 只用于 near cache / 寄存器，偏移要保证对齐
- 注意 LoongArch 的 `ld.*/st.*` 要求自然对齐；guest 非对齐访问必须走地址空间 helper，不能直接发 `ld.w`

### 3.6 调用约定和 helper

`OP_CALLH` 调生成的代码 handle，`OP_CALLC` 调 C++ 函数（如 `drc_map_variables::get_value`、内存访问函数）。LoongArch LP64D：

- 整数参数 `$a0-$a7`，浮点参数 `$fa0-$fa7`
- 返回值 `$a0` / `$fa0`
- `$ra` 链接寄存器
- `$sp` 必须 16 字节对齐
- `$s0-$s8`、`$fp` 由被调方保存；`$t0-$t8`、`$a*` 可随意用
- 成员函数调用要把 `this` 放 `$a0`，其余参数顺延
- 入口/出口有 prolog/epilog：保存 `$ra`、`$fp`、`$s0-$s8` 和 FCSR，退出时恢复

ARM64 用 asmjit 的 `FuncFrame` 自动做这些；LoongArch 需要手写 prolog/epilog。

### 3.7 浮点和 FCSR

- 保存/恢复宿主 FCSR：`movfcsr2gr` / `movgr2fcsr`
- UML 的 `fmod` 顺序是 trunc/round/ceil/floor；LoongArch FCSR 的 RM 位（bit 9:8）是 RNE/RZ/RP/RM，**编码顺序不同**，必须做映射
- LoongArch 的 64 位 FPR 对单精度采用 **NaN-boxing**（和 MIPS64 类似），`fmov.s`、`fcvt.s.d`、`movfr2gr.s` 的行为要和 UML 的 32 位浮点语义对齐，不能直接把低 32 位当独立 float
- `FTOINT` 的各舍入模式用 `ftintrz/ftintrne/ftintrp/ftintrm` 对应实现

### 3.8 entry/exit/异常

参考 `drcbearm64.cpp:1633` 的 invariant code：

```cpp
// 伪代码
entry:
    prolog 保存 callee-saved + ra
    BASE = m_baseptr
    保存 FCSR 到 m_near
    设置 m_state.fmod
    call generated entry (a0 = entry.codeptr())
exit:
    恢复 FCSR / epilog
    返回 a0
```

`execute()` 就是 `return (*m_entry)(entry.codeptr())`（`drcbearm64.cpp:1738`）。调试器钩子、`OP_EXH`、`OP_RECOVER` 等需要和 ARM64 一样通过 `resolved_member_function` 调 C++，并保证异常展开路径能回到 `execute()`。

### 3.9 代码缓存和 icache

- `drc_cache` 分配的是 RWX/W^X 虚拟内存；生成完代码后必须刷新指令缓存。
- `osdlib_unix.cpp:277` 的 `osd::invalidate_instruction_cache()` 已经用 `__builtin___clear_cache`，LoongArch GCC/Clang 支持；必要时在它前后加 `dbar 0` / `ibar 0`。
- 如果像 `drcbec` 那样用 `drc_cache::begin_codegen()` / `end_codegen()` 直接生成到 cache，`end_codegen()` 已经负责 invalidate icache，不需要再手动刷新。
- 每次 `execute()` 前 `m_cache.codegen_complete()` 不要漏。

## 4. 汇编器：直接使用 lagoon

`loongson-community/lagoon` 是一个 MIT 许可、无依赖的 C 语言 LoongArch 运行时汇编/反汇编库（v1.0.0，`lagoon.c` + `lagoon.h`），正好覆盖需求：

- 全部整数/浮点/原子/LSX/LASX 助记符，函数名如 `la_add_d()`、`la_beq()`、`la_jirl()`、`la_fld_d()` 等
- `la_init_assembler(&as, buffer, capacity)`
- 前向/后向标签：`la_label(&as, &label)` + `la_bind(&as, &label)`（前向引用先写 0，bind 时回填）
- 常量：`la_load_immediate32/64()`、`la_li_d()`、`la_la_local()`、`la_la_global()`
- 反汇编：`la_disasm_one()`、`la_insn_to_str()`，可直接用于 `-drc_log_native`
- 伪指令：`la_move()`、`la_nop()`、`la_ret()`、`la_jr()`、`la_ud()`

因此 **不需要自己写编码器**，也**不需要给 asmjit 增加 LoongArch emitter**。仓库自带的 asmjit 只有 `Arch::kLA64` 枚举（`3rdparty/asmjit/asmjit/core/archtraits.h:41`）和宿主检测（`api-config.h:287`），实际后端目录只有 `x86/` 和 `arm/`。

### 4.1 lagoon 的边界和坑

1. **没有缓冲区越界检查**：`emit32` 直接写 `cursor`，只有 `la_get_remaining_buffer_size()` 可用。MAME 必须在每次生成后检查剩余空间，不足时安全地 `end_codegen()` 再 `block.abort()`。
2. **立即数范围只在 `LA_DEBUG` 下断言**：`scripts/src/3rdparty.lua` 的 `lagoon` project 已默认定义 `LA_DEBUG`，并在 gmake/ninja/jcdb 构建中追加 `-UNDEBUG`，因此 Release 构建（默认带 `-DNDEBUG`）也会检查立即数范围。若在构建系统之外手工编译 `lagoon.c`，需要同时传 `-DLA_DEBUG -UNDEBUG`，否则 `assert()` 会被 `NDEBUG` 关闭；正规构建仍建议对关键立即数自己做范围检查。
3. **分支回填不检查范围**：`la_bind()` 对 16 位条件分支/26 位 `b/bl` 的偏移直接截断。UML 的 `JMP` 只在本 block 内（`uml_instructions.rst:327`），正常块大小没问题；但不要用 16 位条件分支跳远端。
4. **`la_la_local()` / `la_la_global()` 是汇编时的 PC 相对寻址**：`offset` 相对当前 `cursor`，且 `la_bind()` 回填时按 `assembler->buffer` 计算页偏移。所以必须把代码**直接生成到最终地址**（MAME 的 `drc_cache::begin_codegen()`），不要先在临时 buffer 里汇编再 memcpy 到 cache，否则页内偏移可能错。如果一定要用临时 buffer，就只用普通分支标签，远端地址用 `la_load_immediate64()`。
5. `la_bind()` 靠解码指令位模式识别 `b/bl/beq/bne/.../jirl` 和 `la.local/la.global` 的 `pcalau12i` 序列；`la_label()` 的返回值必须直接喂给这些指令，否则不会被回填。
6. `lagoon_label_t` 必须零初始化，用完调 `la_label_free()`；引用超过 4 次的 label 会 `malloc`，不要泄漏。

### 4.2 在 MAME 里怎么接

推荐直接用 `begin_codegen()` / `end_codegen()` 在 DRC cache 顶部生成，这样 `la_la_local()` / `la_la_global()` 可以直接引用 near cache 里的 `m_state`/helper 指针：

```cpp
void drcbe_la64::generate(drcuml_block &block,
                          const uml::instruction *instlist, uint32_t numinst)
{
    m_hash.block_begin(block, instlist, numinst);
    m_map.block_begin(block);

    // 保守上界；实际实现按最坏 opcode 展开估算
    uint32_t const reserve = numinst * 256 + 64;
    drccodeptr *cachetop = block.invariant()
            ? m_cache.begin_codegen_invariant(reserve)
            : m_cache.begin_codegen(reserve);
    if (!cachetop)
        block.abort();

    uint8_t *base = (uint8_t *)((uintptr_t(*cachetop) + 3) & ~uintptr_t(3));
    size_t capacity = reserve - (base - *cachetop);

    lagoon_assembler_t as;
    la_init_assembler(&as, base, capacity);

    // max_op_bytes 取实测的最坏单条 UML 展开大小（通常 < 512B）
    constexpr size_t max_op_bytes = 1024;

    for (uint32_t i = 0; i < numinst; i++)
    {
        // 先检查再生成，避免生成到一半越界
        if (la_get_remaining_buffer_size(&as) < max_op_bytes)
        {
            // 保持 cache 一致：先提交长度，再放弃这块
            *cachetop = (drccodeptr)as.cursor;
            m_cache.end_codegen();
            block.abort();
        }

        generate_one(as, instlist[i]);
    }

    *cachetop = (drccodeptr)as.cursor;
    m_cache.end_codegen();      // 内部会 invalidate icache

    m_hash.block_end(block);
    m_map.block_end(block);
}
```

near cache 里的绝对值指针（helper 的 `{obj, func}`、`m_state`）直接用：

```cpp
// near 槽里存 { uintptr_t obj; void *func; }
la_la_global(&as, LA_T0,
    (uint8_t *)&m_helper.func - (uint8_t *)as.cursor);
la_la_global(&as, LA_A0,
    (uint8_t *)&m_helper.obj - (uint8_t *)as.cursor);

// 取 m_state 的地址
la_la_local(&as, LA_A1,
    (uint8_t *)&m_state - (uint8_t *)as.cursor);
```

目标在 ±2 GiB 之外时用 `la_load_immediate64()`（最多 4 条指令）。

## 5. 文件和构建接入

新增：

```
src/devices/cpu/drcbela64.h
src/devices/cpu/drcbela64.cpp
```

头文件照抄 `drcbearm64.h` 的工厂声明：

```cpp
namespace drc {
std::unique_ptr<drcbe_interface> make_drcbe_la64(
        drcuml_state &drcuml, device_t &device, drc_cache &cache,
        uint32_t flags, int modes, int addrbits, int ignorebits);
}
```

`src/devices/cpu/drcuml.cpp`：

```cpp
#include "drcbela64.h"

#elif !defined(MAME_NOASM) && defined(__loongarch__) && (__loongarch_grlen == 64)
#define NATIVE_DRC drcbe_la64
```

（宏名和仓库里 asmjit 的判断保持一致；如果你的后端不依赖 asmjit，理论上可以不受 `MAME_NOASM` 限制，但建议先跟随现有约定。）

`scripts/src/cpu.lua`：

```lua
CPU_INCLUDE_DRC_NATIVE = CPU_INCLUDE_DRC
    and (not _OPTIONS["FORCE_DRC_C_BACKEND"])
    and ((_OPTIONS["PLATFORM"] == "x86")
      or (_OPTIONS["PLATFORM"] == "arm64")
      or (_OPTIONS["PLATFORM"] == "loongarch64"))

if CPU_INCLUDE_DRC_NATIVE then
    if _OPTIONS["PLATFORM"] == "loongarch64" then
        files {
            MAME_DIR .. "src/devices/cpu/drcbela64.cpp",
            MAME_DIR .. "src/devices/cpu/drcbela64.h",
        }
    else
        files { ...drcbearm64..., ...drcbex64... }
    end
end
```

`scripts/src/3rdparty.lua:1990` 的 asmjit 构建条件**不要**加 loongarch64（没有 emitter）。`scripts/genie.lua:711` 的 `ASMJIT_STATIC` 也不要加。

把 lagoon 放进 `3rdparty/lagoon/`（保留 `LICENSE`，MIT 与 MAME 的第三方库策略兼容），并 pin 到 `v1.0.0` 或某个固定 commit（库比较新，API 可能变化）。按照 asmjit 的方式加一个静态库 project。注意 `3rdparty.lua` 在 `devices.lua`/`cpu.lua` 之前加载，所以这里不能引用 `CPU_INCLUDE_DRC_NATIVE`，直接用 `PLATFORM` 判断：

```lua
-- scripts/src/3rdparty.lua（asmjit 段之后）
if (not _OPTIONS["FORCE_DRC_C_BACKEND"]) and (_OPTIONS["PLATFORM"] == "loongarch64") then
project "lagoon"
    uuid "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    kind "StaticLib"
    includedirs {
        MAME_DIR .. "3rdparty/lagoon",
    }
    files {
        MAME_DIR .. "3rdparty/lagoon/lagoon.c",
        MAME_DIR .. "3rdparty/lagoon/lagoon.h",
    }
end
```

`scripts/src/main.lua:165` 的链接改成按平台选择：

```lua
if CPU_INCLUDE_DRC_NATIVE then
    links {
        (_OPTIONS["PLATFORM"] == "loongarch64") and "lagoon" or "asmjit",
    }
end
```

如果不想单独建 project，也可以把 `3rdparty/lagoon/lagoon.c` 直接加进 `cpu.lua` 的 loongarch64 文件列表，然后 `#include "lagoon/lagoon.h"`（主目标的 includedirs 已包含 `3rdparty`）。独立 project 更符合 MAME 现有组织方式。

`makefile` 增加平台识别（约 203 行）：

```make
else ifneq ($(filter loongarch64%,$(UNAME_M)),)
PLATFORM := loongarch64
else ifneq ($(filter loongarch64%,$(UNAME_P)),)
PLATFORM := loongarch64
```

以及在 ARCHITECTURE 检测处清空前缀（约 402 行）：

```make
ifeq ($(findstring loongarch64,$(UNAME)),loongarch64)
ARCHITECTURE :=
endif
```

## 6. 实施顺序

1. **集成 lagoon**：把 `lagoon.c`/`lagoon.h` 放进 `3rdparty/lagoon`，先跑通 `la_init_assembler()`、`la_label()/la_bind()`、`la_disasm_one()`，并写一个薄封装处理 buffer 剩余空间、label 生命周期和 `begin_codegen()`/`end_codegen()`；`LA_DEBUG` 已在 lagoon project 中默认开启，用于立即数范围检查。
2. **骨架**：`reset/execute/generate/get_info/hash_*`，`generate` 先只处理 `NOP/EXIT/HANDLE/HASH/LABEL/COMMENT`，跑通 entry/exit 和 ABI。
3. **控制流**：`JMP/HASHJMP/CALLH/RET/EXH`、handle fixup、`m_exit/m_nocode/m_endofblock`。
4. **整数和 flags**：MOV/SET/ADD/ADDC/SUB/SUBB/CMP/TEST/AND/OR/XOR/移位/MUL/DIV/SEXT/BFXU/LZCNT/TZCNT/BSWAP/ROL/ROLC/RORC。
5. **内存和 map variable**：LOAD/LOADS/STORE/READ/WRITE/READM/WRITEM/MAPVAR。
6. **浮点**：FLOAD/FSTORE/FREAD/FWRITE/FMOV/FADD/FSUB/FMUL/FDIV/FCMP/FTOINT/FFRINT/FFRFLT/FRNDS/FCOPYI/ICOPYF，以及 fmod/FCSR。
7. **优化**：直接寄存器、CMP+条件分支融合、常量折叠、分支岛。
8. **回归**：先 MIPS3，再 POWERPC、SH、UNSP、ADSP、DSPP 等。

## 7. 验证方法

- `-drc_log_uml`：输出 UML IR，和 C 后端对照
- `-drc_log_native`：用 lagoon 的 `la_disasm_one()` / `la_insn_to_str()` 输出生成的原生指令，也可以和 `llvm-objdump -d` 对拍
- `-drc_use_c`：强制 C 后端，和你的后端 A/B 对跑
- `-nodrc`：解释器，作为最终 golden reference
- 用同一存档/回放对比帧、音频、状态
- `lagoon.c` 已默认启用 `LA_DEBUG`（并加 `-UNDEBUG`），立即数越界会直接断言
- 建议先让 `drcbe_la64` 对未实现 opcode 调 `block.abort()`，这样只影响测试用例，不会静默算错

## 8. 工作量估算

| 部分 | 约行数 |
|---|---|
| lagoon 集成 + 薄封装 | 0.2–0.5k |
| 后端主体 | 4–5k |
| 测试/调试 | 大量 |

因为用 lagoon，省掉了原先预计 1–2k 行的手写编码器；主要工作集中在 UML 语义映射和后端调试。

难点排序：**软件 flags/进位 > 32 位符号扩展语义 > 浮点 NaN-boxing/FCSR > 分支范围 > 调用约定/prolog**。如果只是想在 LoongArch 上跑 MAME，直接用现有的 `drcbe_c` 就行；只有确实需要 DRC 性能时再动手。

## 9. 实际移植结果与验证

### 9.1 新增和修改的文件

新增：

```
3rdparty/lagoon/LICENSE
3rdparty/lagoon/lagoon.c
3rdparty/lagoon/lagoon.h
src/devices/cpu/drcbela64.cpp
src/devices/cpu/drcbela64.h
```

修改：

```
makefile                         # 识别 loongarch64，并清空 ARCHITECTURE 前缀
scripts/src/3rdparty.lua         # 增加 lagoon 静态库 project
scripts/src/cpu.lua              # loongarch64 纳入原生 DRC，并编译 drcbela64
scripts/src/main.lua             # loongarch64 链接 lagoon 而不是 asmjit
src/devices/cpu/drcuml.cpp       # 识别 __loongarch64 并选择 drcbe_la64
src/lib/util/abi.h               # 修正 LoongArch 成员函数指针布局
```

`src/lib/util/abi.h` 的修改是必需的：LoongArch 的 Itanium C++ 成员函数指针使用 ARM 式布局（虚函数标志在 adjustment 的最低位），如果按标准 Itanium 布局解析，虚拟成员函数会解析成错误的 vtable 偏移，导致具体内存访问函数、`drc_map_variables::get_value`、调试器 hook 等都无法正确调用。

### 9.2 后端实现要点

- 寄存器映射与报告一致：`I0-I7 -> $s0-$s7`，`FLAGS -> $s8`，`BASE -> $fp`，临时寄存器使用 `$t0-$t8`，`F0-F7 -> $fs0-$fs7`；`get_info()` 返回 `direct_iregs = 8`、`direct_fregs = 8`。
- flags 完全软件维护在 `$s8`，位布局与 `uml::FLAG_BIT_*` 一致；ADDC/SUBB 的进位、移位/旋转产生的进位都按 `drcbec.cpp` 的语义实现。
- 所有 32 位整数运算结果用 `bstrpick.d rd, rd, 31, 0` 零扩展到 64 位，避免 LoongArch `*.w` 指令的符号扩展污染后续地址计算和比较。
- 顶层入口建立 160 字节栈帧，保存 `$ra`、`$fp`、`$s0-$s8`、`$fs0-$fs7`；handle 子程序使用 16 字节小帧。`m_near` 保存原始 FCSR 和顶层 SP，退出时恢复 SP、FCSR 和所有 callee-saved 寄存器。
- `m_exit` 会先把 `$sp` 恢复到顶层 SP。这样即使 `EXIT` 发生在 `CALLH`/`EXH` 的 handle 内部而没有 `RET`，也不会读到错误的栈帧。
- FCSR 的 RM 字段按 `fmod ^ 1` 映射（UML trunc/round/ceil/floor -> LoongArch RZ/RNE/RP/RM）；`SETFMOD`、`RESTORE`、入口和 helper 调用前后都会同步宿主 FCSR。
- 内存访问对“访问宽度等于 address space 原生宽度”的情况走 `specific_access_info` 快速路径，窄访问和非原生宽度、masked 访问走 `resolved_memory_accessors` 通用路径。
- `-drc_log_native` 使用 lagoon 的 `la_disasm_one()`/`la_insn_to_str()` 输出反汇编；transient block 会等所有 label 回填完成后再输出，避免日志里看到未回填的 `b 0`。
- lagoon 的生命周期、缓冲区剩余空间检查、`begin_codegen()`/`end_codegen()` 和失败时的 `block.abort()` 都按报告建议处理。

### 9.3 验证环境与方法

验证分为 MIPS3 合成 A/B 对比和真实游戏实测两部分。

#### 9.3.1 MIPS3 A/B 对比

MIPS3 验证使用临时编写的 MIPS R4600BE 测试驱动（`-drc_use_c` 与原生后端 A/B 对比），程序放在物理 `0x1fc00000`（MIPS 复位向量对应的物理地址），RAM 映射在物理 `0x00000000`，通过定时器读取结果并打印。临时测试驱动在验证完成后已删除，不进入仓库。

对比覆盖的 UML 路径包括：

- 整数：ADD/SUB/ADDU/SUBU、CMP、SLT/SLTU/SLTI、32/64 位 SHL/SHR/SAR/DSLL/DSRL/DSRA、变量移位、MULU/MULS（含 HI/LO）、DIVU/DIVS（含商和余数）、AND/OR/XOR/NOR、立即数操作。
- 控制流：条件/无条件分支、循环、JMP/HASHJMP、CALLH/RET、`m_exit`、`m_nocode`、`m_endofblock`、handle 入口。
- 内存：LB/LBU/LH/LHU/LW/LD、SB/SH/SW/SD，覆盖 READ/WRITE 的通用路径和原生宽度快速路径。
- 浮点：`add.s`/`add.d`、`div.s`、`cvt.s.d`/`cvt.d.s`、`trunc.w.s`、`cvt.s.w`、`c.lt.s`/`bc1t`，以及 `mtc1`/`mfc1`/`dmtc1`/`dmfc1` 的单/双精度寄存器搬运。

最终 51 个 32 位结果字全部与 C 后端一致，包括乘法高低位、除法商余数、移位/符号扩展、字节/半字加载、浮点运算和比较分支。MIPS3 A/B 验证过程中发现并修复了两个关键问题：

1. `m_exit` 未先把 `$sp` 恢复到顶层帧，导致在 `CALLH` 帧内执行 `EXIT` 时从错误位置恢复 `$ra`，表现为跳转到 near cache/`m_baseptr` 后崩溃。修复后正常退出。
2. `abi.h` 未把 LoongArch 识别为 ARM 式成员函数指针布局，导致 `specific_access_info` 的虚拟函数入口解析错误；修复后具体内存访问快速路径正常工作。

#### 9.3.2 STV/SH-2 游戏实测

为确认原生后端不只是“能生成代码”，而是真的在游戏执行路径上运行，使用 Sega ST-V 游戏 `diehard`（SH7604/SH-2，DRC 前端）做了实测。测试 ROM 为 `diehard.zip`、`stvbios.zip`、`segabill.zip`，放在 `roms/` 下。命令：

```sh
./mame diehard -rompath roms -video none -sound none -nothrottle -seconds_to_run 30
./mame diehard -rompath roms -video none -sound none -nothrottle -seconds_to_run 8 -drc_log_native
./mame diehard -rompath roms -video none -sound none -nothrottle -seconds_to_run 8 -drc_use_c
```

结果：

- 原生后端：30 秒游戏时间正常退出，退出码 0，平均速度约 176%。
- `-drc_use_c`：同配置平均速度约 55%，可作为“未走原生 JIT”的对照。
- `-drc_log_native` 生成 `drcbela64_sh2_7604_maincpu.asm`（约 13.5 MB、194365 条指令）和 `drcbela64_sh2_7604_slave.asm`。
- 用 GDB 在 `drcbe_la64::execute` 下断点，实际命中，调用栈为：

```
#0 drc::(anonymous namespace)::drcbe_la64::execute(uml::code_handle&)
#1 sh_common_execution::execute_run_drc()
#2 device_scheduler::timeslice()
...
```

这直接证明 `diehard` 的 SH-2 执行走的是 `drcbe_la64` 生成的 LoongArch64 原生代码，而不是 C 后端。

首次实测时游戏在 SH-2 写内存的 fastram 快速路径上段错误，定位为 `emit_condition()` 的无符号条件错误：

- `COND_A` 原本实现为 `C && !Z`，正确语义是 `!C && !Z`；
- `COND_BE` 原本实现为 `!(C && !Z)`，正确语义是 `C || Z`。

在 SH-2 的 `write32` handle 中，地址 `0x60000000` 本应因大于 fastram 结束地址而跳过 workram 快速路径，但错误的 `COND_A` 使它落入快速路径，用 `fastbase + address` 访问未映射的宿主地址并崩溃。修正 `src/devices/cpu/drcbela64.cpp:713` 后，`diehard` 连续运行 30 秒正常退出。

需要说明的是，上面的实测使用 `-video none -sound none`，只能证明“不崩溃、能跑完”，不能证明内存访问和画面正确。后续补做画面与内存内容对比时，发现了一个不会崩溃但会导致黑屏的问题。

#### 9.3.3 STV 黑屏问题与修复（32 位 specific 访问掩码）

问题现象：

- 原生后端运行 `diehard` 时输出纯黑画面，AVI 每帧像素均值均为 0。
- 加 `-drc_use_c` 后可以正常显示 ST-V 的 FBI 启动画面，说明游戏/驱动本身没有问题。
- 逐帧对比共享内存发现：第 18 帧之前 workram_l、workram_h、sound RAM、backup RAM 完全一致；第 19 帧开始 `workram_h` 出现分叉，例如 `0x06002CA4` 处 C 后端为 `0x25A004xx`，原生后端为 `0x25A00000`。进一步定位到指针表构建代码读取 sound RAM 时，C 后端读到 `0x00000700`，原生后端读到 0；VDP/SCU 寄存器也在同一时期开始分叉，最终导致 VDP 未正确初始化而黑屏。

根因：

`drcbe_la64::op_read()` 和 `op_write()` 在“访问宽度等于 address space 原生宽度”的 `specific_access_info` 快速路径中，用 32 位常量构造访问掩码：

```cpp
(1u << (accessors.specific.native_bytes * 8)) - 1
```

当 `native_bytes == 4` 时，`1u << 32` 是 C++ 未定义行为；当前 LoongArch 构建实际得到 0。结果传给 `handler_entry_read/write` 的 `mem_mask` 恒为 0：

- 32 位 specific 读取不再命中任何 subunit，返回未映射值 0（sound RAM、VDP、SCU、SCSP 等）；
- 32 位 specific 写入的数据被掩掉，写不进相应设备；
- fastram 路径不使用该掩码，因此 BIOS/游戏仍能继续执行，表现为“能跑但黑屏”，而不是崩溃。

修复：

```cpp
// src/devices/cpu/drcbela64.cpp
load_imm32(a, REG_PARAM3, uint32_t((uint64_t(1) << (accessors.specific.native_bytes * 8)) - 1)); // op_read
load_imm32(a, REG_PARAM4, uint32_t((uint64_t(1) << (accessors.specific.native_bytes * 8)) - 1)); // op_write
```

即先用 64 位计算 `2^(native_bytes*8)-1`，再截断到 32 位，避免移位位数等于类型宽度时的未定义行为。修改位置为 `src/devices/cpu/drcbela64.cpp:3469` 和 `:3594`。

验证结果：

- 原生后端与 `-drc_use_c` 在 5 秒、10 秒的截图逐像素一致，`magick compare -metric AE` 为 `0 (0)`。
- 第 20、60、120 帧的 workram_l、workram_h、sound RAM、backup RAM 以及 VDP/SCU 寄存器 dump 全部一致。
- 原生后端连续运行 30 秒正常退出，退出码 0。

### 9.4 多前端定向回归与真实 ROM 验证

本轮回归不再只依赖 MIPS3 合成用例，而是分为 UML 定向用例和真实 ROM 两部分。测试驱动和日志均为临时验证代码，完成后已从仓库移除。

#### 9.4.1 UML 定向一致性测试

临时驱动直接构造 `drcuml_state` / `drcuml_block`，用同一份 UML 序列分别走原生后端和 `-drc_use_c`，逐项比较 I0-I9、F0-F9、flags、fmod、exp 和执行结果。共 499 个用例，覆盖：

- `BFXU`/`BFXS` 32/64 位，含可变 shift/width、符号扩展边界和 `DBFXS` 化简为 `DSEXT` 的路径。
- `ROLAND`/`ROLINS` 32/64 位，含寄存器 dst、内存 dst、保留位插入和 `dst == src`。
- `ROLC`/`RORC` 32/64 位，count 覆盖 0、1、2、7、31、32、63、64、65，carry 输入 0/1。
- `MULU`/`MULS`/`MULULW`/`MULSLW`、`DIVU`/`DIVS` 的 32/64 位高/低结果、商/余数、除零置 V，以及 32 位脏高位的零扩展。
- `SAVE`/`RESTORE` 覆盖 I0-I9、F0-F9、flags、fmod、exp。
- `CALLH`/`RET`、`EXH`/`EXHc`、`RECOVER`；其中还验证了嵌套 `CALLH` 子程序中的 `RECOVER` 仍恢复最外层异常调用点的 mapvar。

结果：**499/499 全部一致**，原生后端执行结果 `0x5A`，与 C 后端相同。

回归过程中发现并修复了三个问题：

1. **`op_sext` 破坏源寄存器**：当源是直接映射寄存器且 `size < inst.size()` 时，原实现把源寄存器本身当作临时寄存器，并写入 32 位值，导致高 32 位被截断。UML 优化器把 `DBFXS dst,src,0,8` 化简为 `DSEXT dst,src,byte` 后会触发，例如 `I1 = 0x0123456789abcdef` 被改成 `0x89abcdef`。修复为始终使用 `TEMP_REG1` 作为临时寄存器。
2. **`op_recover` 取错返回地址**：原实现从 `[top_sp - 8]` 取地址，实际取到 entry handle 的返回地址，`drc_map_variables::get_value` 找不到对应 mapvar，`RECOVER` 返回 0。修复为使用最外层生成代码子程序帧的返回地址 `[top_sp - (HANDLE_FRAME_SIZE + 8)]`；这与 C 后端 `callstack[0]` 的语义一致，嵌套 `CALLH` 中的 `RECOVER` 也能恢复最外层 `EXH` 调用点。
3. **C 后端 64 位 `DROLC` 的移位未定义行为**：`(flags & FLAG_C) << (shift - 1)` 使用有符号 `int` 移位，`shift - 1` 为 31/62 时产生未定义行为；在 LoongArch 上会符号扩展到 64 位，例如 count=32、carry=1 得到 `0xffffffffc0000000` 而不是 `0x00000001c0000000`。修复为移位前 cast 到 `uint32_t`/`uint64_t`。该修复同时提高了 C 后端作为 A/B 参考的可靠性。

#### 9.4.2 SH-2 真实游戏 `diehard`

使用已有的 ST-V `diehard`（SH7604/SH-2）分别运行原生后端和 `-drc_use_c`，通过 Lua 脚本在固定帧 dump `workram_l`、`workram_h`、sound RAM：

```sh
./mame diehard -rompath roms -video none -sound none -nothrottle \
    -autoboot_script dump.lua
./mame diehard -rompath roms -video none -sound none -nothrottle -drc_use_c \
    -autoboot_script dump.lua
```

结果：

- 第 20、120 帧的 3 MB dump 完全一致。
- 第 60 帧有 3 字节差异（`workram_h` 的 `0x06000708`、`0x0600070a`、`0x06002864`）；同一配置重复运行结果稳定。该帧 native/C 与 `-nodrc` 也都有差异，初步判断是 DRC 路径的暂态值或调度时序差异，未按后端语义错误处理，后续可用更精确的帧同步继续确认。

#### 9.4.3 SH4 真实硬件诊断 ROM `NaomiDIAG`

使用开源 Sega Naomi 诊断 BIOS `NaomiDIAG_EN.bin`（`Guimli/NaomiDIAG`）替换 `naomi.zip` 中的 `epr-21576h.ic27`。由于没有原版 JVS/MIE/Altera ROM，使用了同尺寸占位文件，MAME 会报告 CRC 不匹配；但 SH-4 代码路径是真实执行的，`-drc_log_native` 生成了 `drcbela64_sh7091_maincpu.asm`，确认走的是 LoongArch64 原生后端。

在固定第 600 帧 dump `0x0C000000` 起的主 RAM 前 1 MB 和 `0x04000000` 起的 texture RAM 前 1 MB，共 2 MB：

- 原生后端与 `-drc_use_c` 的 dump **逐字节一致**。
- 原生后端平均速度约 24%，`-drc_use_c` 约 15%，与“原生 JIT 实际执行”一致。

#### 9.4.4 未完成的真实 ROM 覆盖

- **POWERPC**：没有可直接使用的合法商业 ROM；开源 `trzy/model3`（Sega Model 3 / PPC603e 测试程序）需要 `powerpc-603e-eabi` 交叉工具链，当前环境未构建，因此未做真实运行。
- **UNSP、ADSP(SHARC)、DSPP**：没有对应的合法 ROM/测试 ROM。
- 这些前端重点使用的 `BFXU`/`BFXS`、`ROLAND`/`ROLINS`、`ROLC`/`RORC`、64 位 `DIV`/`MUL`、`SAVE`/`RESTORE`、异常路径和 `RECOVER` 已由 9.4.1 的 UML 定向用例覆盖；后续拿到 ROM 后仍需补做真实前端 A/B。

#### 9.4.5 独立的 DRC 条件码标志位组合测试

为避免 `COND_A`/`COND_BE` 等条件码只靠 MIPS3 或真实游戏间接覆盖，新增了不依赖任何 guest CPU 前端的测试驱动 `src/mame/test/drc_condtest.cpp`。它直接构造 `drc_cache`/`drcuml_state`，对 `COND_Z` 到 `COND_GE` 的全部 16 个条件分别用 `SETc` 和 `JMPc` 两条路径执行，并遍历 C/V/Z/S/U 的全部 32 种标志位组合，共 16 × 32 × 2 = 1024 个检查。期望值由 UML 规范中的条件定义独立计算，不经过任何 DRC 后端。

运行方法：

```sh
tests/devices/cpu/run_drc_condtest.sh
```

脚本会先构建测试目标，然后分别运行原生后端和 C 后端：

```sh
make -j"$(nproc)" SUBTARGET=drc_condtest SOURCES=src/mame/test/drc_condtest.cpp
./drc_condtest drc_condtest -video none -sound none -nothrottle
./drc_condtest drc_condtest -video none -sound none -nothrottle -drc_use_c
```

结果：

- 原生后端和 `-drc_use_c` 都输出 `drc_condtest: PASS (16 conditions x 32 flag combinations x 2 paths)`。
- 临时把 `COND_A`/`COND_BE` 改回修复前的实现重新构建，测试立即报告 64 个不匹配（`COND_A` 和 `COND_BE` 各 16 种标志组合 × 2 条路径），说明该测试确实能捕获这次回归。

测试驱动通过 `src/mame/mame.lst` 注册为 `drc_condtest`，并在 `scripts/src/cpu.lua` 中为这个 subtarget 显式启用 DRC 核心；测试本身不实例化 MIPS3 或其他 guest CPU。

#### 9.4.6 独立的 specific 访问掩码测试与真实游戏双重校验

32 位 specific 访问掩码的问题不会崩溃，只会让读取返回 0、写入被丢弃，因此仅靠“能跑完”或“退出码为 0”无法发现。现在用两个层次覆盖：

1. **不依赖 guest CPU 的独立测试** `src/mame/test/drc_specifictest.cpp`：
   - 创建一个 32 位地址空间（`native_bytes == 4`），通过 `drcuml_state` 生成 UML dword 读写。
   - 8 个 dword 读取和 8 个 dword 写入直接检查 RAM 内容，覆盖 `0x80000000`、`0xdeadbeef`、`0xffffffff` 等高位数据。
   - 另外用一个对掩码敏感的寄存器 handler 记录 `mem_mask`，分别断言读取和写入都收到完整的 `0xffffffff`；这比只检查 RAM 更直接，因为 RAM 读取本身可能忽略掩码。
   - 运行方法：

     ```sh
     tests/devices/cpu/run_drc_specifictest.sh
     ```

     脚本分别运行原生后端和 `-drc_use_c`，两者都输出：

     ```
     drc_specifictest: PASS (8 dword reads + 8 dword writes + mask-sensitive register, 32-bit native space)
     ```

   - 临时把 `op_read`/`op_write` 的掩码改回 0 重新构建，测试立即报告 9 个不匹配（7 个写入数据 + 2 个掩码），退出码非 0，说明该测试确实能捕获“读错/写丢”型回归。

2. **真实游戏画面与内存/寄存器双重校验** `tests/devices/cpu/run_drc_diehard_verify.sh`：
   - 该脚本用 Lua helper `tests/devices/cpu/diehard_drc_verify.lua` 分别以原生后端和 `-drc_use_c` 运行 ST-V `diehard`。
   - 在第 120、180、240 帧同时记录：
     - 屏幕像素缓冲的 FNV-1a 哈希和非黑像素数量；
     - `workram_l`、`workram_h`、sound RAM、backup RAM；
     - VDP1 寄存器、VDP2 寄存器、VDP2 CRAM、SCU 寄存器。
   - 两个后端的屏幕摘要必须完全一致，且非黑像素数必须大于 100（避免“两边都是黑屏也算一致”）；所有内存/寄存器 dump 必须逐字节一致。
   - 结果：原生后端与 C 后端在 3 个帧上全部一致，输出
     `DRC real-game verification: PASS (screen + memories/registers match at frames 120 180 240)`。
   - 早期第 20/60 帧在当前冷启动配置下仍处于 BIOS 初始化阶段（全黑），因此校验帧从显示初始化后的第 120 帧开始。
   - ROM 不在仓库中；缺少 `diehard.zip`/`stvbios.zip`/`segabill.zip` 时脚本会打印 `SKIP` 并返回 0。

#### 9.4.7 `LA_DEBUG` 立即数断言与符号扩展修正

`scripts/src/3rdparty.lua` 的 `lagoon` project 现在固定定义 `LA_DEBUG`，并在 gmake/ninja/jcdb 构建中追加 `-UNDEBUG`，确保 Release 配置（默认带 `-DNDEBUG`）下 lagoon 的立即数范围断言仍然有效。

启用断言后，`drc_condtest` 和 `drc_specifictest` 曾在启动阶段（`drcbe_la64` 构造）立即触发：

```
3rdparty/lagoon/lagoon.c:2166: la_lu12i_w: Assertion `(sj20) >= (-(1LL << ((20) - 1))) && (sj20) < (1LL << ((20) - 1))' failed.
```

调用链为 `la_load_immediate64()` → `la_lu12i_w()`。原因是 `la_load_immediate32/64()` 把 `(val >> 12) & 0xfffff` 等无符号位模式直接作为有符号立即数传入；当 bit 19/11 置位时超出 `la_lu12i_w()`/`la_lu32i_d()`/`la_lu52i_d()`/`la_addi_w()` 的检查范围。

现在 `lagoon.c` 增加 `la_sign_extend()`，在装载 20 位 `lu12i.w`/`lu32i.d` 字段和 12 位 `lu52i.d`/`addi.w` 字段时先做符号扩展，再传给 lagoon 的编码函数。修正后两个测试在原生和 `-drc_use_c` 后端均通过：

```
drc_condtest: PASS (16 conditions x 32 flag combinations x 2 paths)
drc_specifictest: PASS (8 dword reads + 8 dword writes + mask-sensitive register, 32-bit native space)
```

### 9.5 后续工作

- POWERPC、UNSP、ADSP(SHARC)、DSPP 的真实 ROM 回归尚未完成；拿到合法 ROM/可构建的开源测试程序后，继续按 9.4.1 的用例清单做 native vs `-drc_use_c` A/B。
- `lagoon.c` 已默认用 `-DLA_DEBUG` 编译（Release 下通过 `-UNDEBUG` 保持断言）；`la_load_immediate32/64()` 的 20/12 位立即数已按 9.4.7 完成符号扩展修正。
- 当前实现以正确性优先，未做 CMP+条件分支融合、常量折叠和分支岛等优化；后续可以在 `emit_skip`/`op_jmp` 与 CMP/TEST 上做 peephole。
- 如需在同一个后端实例上递归 `execute()`，顶层 SP 目前保存在 near cache 中，建议后续改为保存到保留的 callee-saved 寄存器或显式保存栈，以避免递归覆盖。
