#include "vu_jittrans.hpp"
#include "vu_jit64.hpp"
#include "vu.hpp"

#include "../errors.hpp"

/**
 * Calling convention notes (needed for calling C++ functions within generated code)
 *
 * x64 - Two conventions exist: Microsoft ABI and System V AMD64 ABI (followed by POSIX systems).
 *
 * Microsoft:
 * Uses registers RCX, RDX, R8, R9 for the first four integer or pointer arguments (in that order), and
 * XMM0, XMM1, XMM2, XMM3 are used for floating point arguments. Additional arguments are pushed onto the
 * stack (right to left). Integer return values (similar to x86) are returned in RAX if 64 bits or less.
 * Floating point return values are returned in XMM0. Parameters less than 64 bits long are not zero extended;
 * the high bits are not zeroed.
 *
 * System V AMD64:
 * The first six integer or pointer arguments are passed in registers RDI, RSI, RDX, RCX, R8,
 * R9 (R10 is used as a static chain pointer in case of nested functions), while XMM0, XMM1, XMM2, XMM3, XMM4,
 * XMM5, XMM6 and XMM7 are used for certain floating point arguments. As in the Microsoft x64 calling convention,
 * additional arguments are passed on the stack. Integral return values up to 64 bits in size are stored in RAX
 * while values up to 128 bit are stored in RAX and RDX. Floating-point return values are similarly stored in XMM0
 * and XMM1.
 *
 * Sources:
 * https://en.wikipedia.org/wiki/X86_calling_conventions#x86-64_calling_conventions
 */

VU_JIT64::VU_JIT64() : emitter(&cache)
{

}

void vu_set_int(VectorUnit& vu, int dest, uint16_t value)
{
    vu.set_int(dest, value);
}

void VU_JIT64::reset()
{
    abi_int_count = 0;
    abi_xmm_count = 0;
    for (int i = 0; i < 16; i++)
    {
        xmm_regs[i].used = false;
        xmm_regs[i].locked = false;
        xmm_regs[i].age = 0;

        int_regs[i].used = false;
        int_regs[i].locked = false;
        int_regs[i].age = 0;
    }

    //Lock special registers to prevent them from being used
    int_regs[REG_64::RAX].locked = true;
    int_regs[REG_64::RSP].locked = true;
    cache.flush_all_blocks();
}

void VU_JIT64::load_const(VectorUnit &vu, IR::Instruction &instr)
{
    REG_64 dest = alloc_int_reg(vu, instr.get_dest(), false);
    emitter.MOV16_REG_IMM(instr.get_source(), dest);
}

void VU_JIT64::move_int_reg(VectorUnit &vu, IR::Instruction &instr)
{
    REG_64 dest = alloc_int_reg(vu, instr.get_dest(), false);
    REG_64 source = alloc_int_reg(vu, instr.get_source(), dest);
    emitter.MOV16_REG(source, dest);
}

void VU_JIT64::jump(VectorUnit& vu, IR::Instruction& instr)
{
    //We just need to set the PC.
    emitter.load_addr((uint64_t)&vu.PC, REG_64::RAX);
    emitter.MOV32_MI_MEM(instr.get_jump_dest(), REG_64::RAX);
}

void VU_JIT64::jump_and_link(VectorUnit& vu, IR::Instruction& instr)
{
    //First set the PC
    emitter.load_addr((uint64_t)&vu.PC, REG_64::RAX);
    emitter.MOV32_MI_MEM(instr.get_jump_dest(), REG_64::RAX);

    //Then set the link register
    REG_64 link = alloc_int_reg(vu, instr.get_dest());
    emitter.MOV64_OI(instr.get_return_addr(), link);
}

void VU_JIT64::jump_indirect(VectorUnit &vu, IR::Instruction &instr)
{
    REG_64 return_reg = alloc_int_reg(vu, instr.get_source());

    //Multiply the address by eight
    emitter.SHL32_REG_IMM(3, return_reg);

    emitter.load_addr((uint64_t)&vu.PC, REG_64::RAX);
    emitter.MOV32_TO_MEM(return_reg, REG_64::RAX);
}

void VU_JIT64::add_unsigned_imm(VectorUnit &vu, IR::Instruction &instr)
{
    REG_64 dest = alloc_int_reg(vu, instr.get_dest(), false);
    REG_64 source = alloc_int_reg(vu, instr.get_source());
    uint16_t imm = instr.get_source2();

    emitter.MOV16_REG(source, dest);
    emitter.ADD16_REG_IMM(imm, dest);
}

void VU_JIT64::mul_vector_by_scalar(VectorUnit &vu, IR::Instruction &instr)
{
    REG_64 dest = alloc_sse_reg(vu, instr.get_dest(), false);
}

REG_64 VU_JIT64::alloc_int_reg(VectorUnit &vu, int vi_reg, bool load_state)
{
    if (vi_reg >= 16)
        Errors::die("[VU_JIT64] Alloc Int error: vi_reg == %d", vi_reg);

    for (int i = 0; i < 16; i++)
    {
        if (int_regs[i].used && int_regs[i].vu_reg == vi_reg)
            return (REG_64)i;
    }

    for (int i = 0; i < 16; i++)
    {
        if (int_regs[i].used)
            int_regs[i].age++;
    }

    int reg = 0;
    int age = 0;
    for (int i = 0; i < 16; i++)
    {
        if (int_regs[i].locked)
            continue;

        if (!int_regs[i].used)
        {
            reg = i;
            break;
        }

        if (int_regs[i].age > age)
        {
            reg = i;
            age = int_regs[i].age;
        }
    }

    if (int_regs[reg].used)
    {
        printf("[VU_JIT64] Flushing %d!\n", reg);
        int old_vi_reg = int_regs[reg].vu_reg;
        emitter.load_addr((uint64_t)&vu.int_gpr[old_vi_reg], REG_64::RAX);
        emitter.MOV64_TO_MEM((REG_64)reg, REG_64::RAX);
    }

    printf("[VU_JIT64] Allocating reg %d\n", reg);

    if (load_state)
    {
        emitter.load_addr((uint64_t)&vu.int_gpr[vi_reg], REG_64::RAX);
        emitter.MOV64_FROM_MEM(REG_64::RAX, (REG_64)reg);
    }

    int_regs[reg].vu_reg = vi_reg;
    int_regs[reg].used = true;
    int_regs[reg].age = 0;

    return (REG_64)reg;
}

REG_64 VU_JIT64::alloc_sse_reg(VectorUnit &vu, int vf_reg, bool load_state)
{
    if (vf_reg >= 32)
        Errors::die("[VU_JIT64] Alloc SSE error: vf_reg == %d", vf_reg);

    //If the register is already used, return it
    for (int i = 0; i < 16; i++)
    {
        if (xmm_regs[i].used && xmm_regs[i].vu_reg == vf_reg)
            return (REG_64)i;
    }

    //Increase the age of each register if it's still allocated
    for (int i = 0; i < 16; i++)
    {
        if (xmm_regs[i].used)
            xmm_regs[i].age++;
    }

    //Find a new register. If no free register is available, replace the oldest register.
    int xmm = 0;
    int age = 0;
    for (int i = 0; i < 16; i++)
    {
        if (!xmm_regs[i].used)
        {
            xmm = i;
            break;
        }

        if (xmm_regs[i].age > age)
        {
            xmm = i;
            age = xmm_regs[i].age;
        }
    }

    //If the chosen register is used, flush it back to the VU state.
    if (xmm_regs[xmm].used)
    {
        int old_vf_reg = xmm_regs[xmm].vu_reg;
        emitter.load_addr((uint64_t)&vu.gpr[old_vf_reg], REG_64::RAX);
        emitter.MOVAPS_TO_MEM((REG_64)xmm, REG_64::RAX);
    }

    if (load_state)
    {
        //Store the VU state register inside the newly allocated XMM register.
        emitter.load_addr((uint64_t)&vu.gpr[vf_reg], REG_64::RAX);
        emitter.MOVAPS_FROM_MEM(REG_64::RAX, (REG_64)xmm);
    }

    xmm_regs[xmm].vu_reg = vf_reg;
    xmm_regs[xmm].used = true;
    xmm_regs[xmm].age = 0;

    return (REG_64)xmm;
}

void VU_JIT64::flush_regs(VectorUnit &vu)
{
    //Store the contents of all allocated x64 registers into the VU state.
    for (int i = 0; i < 16; i++)
    {
        int vf_reg = xmm_regs[i].vu_reg;
        int vi_reg = int_regs[i].vu_reg;
        if (xmm_regs[i].used && vf_reg)
        {
            xmm_regs[i].used = false;
            emitter.load_addr((uint64_t)&vu.gpr[vf_reg], REG_64::RAX);
            emitter.MOVAPS_TO_MEM((REG_64)i, REG_64::RAX);
        }

        if (int_regs[i].used && vi_reg)
        {
            int_regs[i].used = false;
            emitter.load_addr((uint64_t)&vu.int_gpr[vi_reg], REG_64::RAX);
            emitter.MOV64_TO_MEM((REG_64)i, REG_64::RAX);
        }
    }
}

void VU_JIT64::recompile_block(VectorUnit& vu, IR::Block& block)
{
    cache.alloc_block(vu.get_PC());

    //Prologue
    emitter.PUSH(REG_64::RBP);
    emitter.MOV64_MR(REG_64::RSP, REG_64::RBP);

    while (block.get_instruction_count() > 0)
    {
        IR::Instruction instr = block.get_next_instr();

        switch (instr.op)
        {
            case IR::Opcode::LoadConst:
                load_const(vu, instr);
                break;
            case IR::Opcode::MoveIntReg:
                move_int_reg(vu, instr);
                break;
            case IR::Opcode::Jump:
                jump(vu, instr);
                break;
            case IR::Opcode::JumpAndLink:
                jump_and_link(vu, instr);
                break;
            case IR::Opcode::VMulVectorByScalar:
                //mul_vector_by_scalar(vu, instr);
                break;
            case IR::Opcode::JumpIndirect:
                jump_indirect(vu, instr);
                break;
            case IR::Opcode::AddUnsignedImm:
                add_unsigned_imm(vu, instr);
                break;
            default:
                Errors::die("[VU_JIT64] Unknown IR instruction");
        }
    }

    flush_regs(vu);

    //Return the amount of cycles to update the VUs with
    emitter.MOV16_REG_IMM(block.get_cycle_count(), REG_64::RAX);

    //Epilogue
    emitter.POP(REG_64::RBP);
    emitter.RET();

    //Switch the block's privileges from RW to RX.
    cache.set_current_block_rx();
    cache.print_current_block();
    cache.print_literal_pool();
}

uint8_t* VU_JIT64::exec_block(VectorUnit& vu)
{
    printf("[VU_JIT64] Executing block at $%04X\n", vu.PC);
    if (cache.find_block(vu.PC) == -1)
    {
        printf("[VU_JIT64] Block not found: recompiling\n");
        IR::Block block = VU_JitTranslator::translate(vu.PC, vu.get_instr_mem());
        recompile_block(vu, block);
    }
    //if (vu.PC == 0xC38)
        //Errors::die("hi");
    return cache.get_current_block_start();
}

void VU_JIT64::prepare_abi(VectorUnit& vu, uint64_t value)
{
    const static REG_64 regs[] = {RDI, RSI, RDX, RCX, R8, R9};

    if (abi_int_count >= 6)
        Errors::die("[VU_JIT64] ABI integer arguments exceeded 6!");

    REG_64 arg = regs[abi_int_count];

    //If the chosen integer argument is being used, flush it back to the VU state
    if (int_regs[arg].used)
    {
        int vi_reg = int_regs[arg].vu_reg;
        emitter.load_addr((uint64_t)&vu.int_gpr[vi_reg], REG_64::RAX);
        emitter.MOV64_TO_MEM(arg, REG_64::RAX);
        int_regs[arg].used = false;
        int_regs[arg].age = 0;
    }
    emitter.load_addr(value, regs[abi_int_count]);
    abi_int_count++;
}

void VU_JIT64::call_abi_func(uint64_t addr)
{
    emitter.CALL(addr);
    abi_int_count = 0;
    abi_xmm_count = 0;
}

int VU_JIT64::run(VectorUnit& vu)
{
    __asm__ (
        "pushq %rbx\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "pushq %rdi\n"

        "callq __ZN8VU_JIT6410exec_blockER10VectorUnit\n"
        "callq *%rax\n"

        "popq %rdi\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %rbx\n"
    );
}