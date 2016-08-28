#include <iostream>
#include "x86_tree.h"
#include "util.h"

using namespace std;

MemPool *xpool;

void *XNode::operator new(size_t sz)
{
    return xpool->memAlloc(sz);
}

void XNode::operator delete(void *ptrb)
{
    return xpool->memFree(ptrb);
}

#define IMPLEMENT_ALU_INSTRUCTION_2ARG(opname, opcode, function) \
    bool XI_##opcode::exec(X86Sim *sim, XReference &result) { \
        XReference ref1;                     \
                                             \
        if (!arg1->getReference(sim, ref1))  \
            return false;                    \
                                             \
        if ((ref1.type != RT_Reg) && (ref1.type != RT_Mem)) {   \
            reportError("Invalid destination '%s' for " opname " instruction.\n", arg1->toString().c_str()); \
            return false;   \
        }                   \
                            \
        XReference ref2;    \
                            \
        if (!arg2->getReference(sim, ref2))  \
            return false;                    \
                                             \
        if ((ref1.type == RT_Mem) && (ref2.type == RT_Mem)) {           \
            reportError("Memory-Memory references are not allowed.\n"); \
            return false;                                               \
        }                                                               \
                                                                        \
        if (!sim->getValue(ref2)) {                                     \
            reportError("Unexpected error %d: %s.\n", __LINE__, __FUNCTION__);  \
            return false;   \
        }                   \
                            \
        if (!sim->doOperation(function, ref1, ref2.value)) {   \
            reportError("Invalid arguments for operation.\n");  \
            return false;                                       \
        }                                                       \
                                                                \
        if ((function != XFN_CMP) && (function != XFN_TEST)) {  \
            result = ref1;                                      \
        } else {                                                \
            result.type = RT_None;                              \
        }                                                       \
                                                                \
        return true;                                            \
    }

#define SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref) \
    do {    \
        if (arg->getKind() != XARG_MEMREF &&        \
            arg->getKind() != XARG_REGISTER) {      \
            reportError("Invalid argument '%s' for instruction '%s'\n", \
                        arg->toString().c_str(),    \
                        this->getName());   \
            return false;   \
        }   \
            \
        if (!arg->getReference(sim, a_ref)) {   \
            return false;   \
        }   \
            \
        if (a_ref.bitSize != BS_8) {    \
            reportError("Invalid operand size in instruction '%s'\n", this->getName()); \
            return false;   \
        }   \
    } while (0)


/* XArgument 'eval' method implementation */
bool XArgRegister::eval(X86Sim *xsim, int resultSize, uint8_t flags, uint32_t &result)
{
    if (!WANT_XTEND(flags) && resultSize != regSize) {
        reportError("Operand sizes do not match. Expected %d bits size.\n", BIT_SIZE(resultSize));
        return false;
    }
    if (WANT_XTEND(flags) && regSize >= resultSize) {
        reportError("Invalid size of operand '%s'. Size is %d bits.\n", toString().c_str(), regSize);
        return false;
    }

    uint32_t value;

    if (!xsim->getRegValue(regId, value))
        return false;

    result = (flags & SX_MASK)? signExtend(value, regSize, resultSize) : value;

    return true;
}

bool XArgRegister::getReference(X86Sim *sim, XReference &ref)
{
    UNUSED(sim);

    ref.type = RT_Reg;
    ref.bitSize = this->regSize;
    ref.address = this->regId;

    return true;
}

bool XArgMemRef::eval(X86Sim *xsim, int resultSize, uint8_t flags, uint32_t &result)
{
    int wordSize;

    wordSize = (sizeDirective==0)? resultSize : sizeDirective;

    if (!WANT_XTEND(flags) && resultSize != wordSize) {
        reportError("Error: Operand sizes do not match. Expected %d bits size.\n", BIT_SIZE(resultSize));
        return false;
    }
    if (WANT_XTEND(flags) && wordSize >= resultSize) {
        reportError("Invalid size of operand '%s'\n", toString().c_str());
        return false;
    }

    uint32_t vaddr;

    if (!expr->eval(xsim, vaddr))
        return false;

    uint32_t value;

    if (!xsim->readMem(vaddr, value, wordSize)) {
        reportError("Cannot read address '0x%X'.\n", vaddr);
        return false;
    }

    result = (flags & SX_MASK)? signExtend(value, wordSize, resultSize) : value;

    return true;
}

bool XArgMemRef::getReference(X86Sim *xsim, XReference &ref)
{
    uint32_t vaddr;

    if (!expr->eval(xsim, vaddr))
        return false;

    ref.type = RT_Mem;
    ref.bitSize = this->sizeDirective;
    ref.address = vaddr;

    return true;
}

bool XArgConstant::eval(X86Sim *sim, int resultSize, uint8_t flags, uint32_t &result)
{
    UNUSED(sim);

    if (WANT_XTEND(flags)) {
        reportError("Invalid operand '%s'.\n", toString().c_str());
        return false;
    }

    uint32_t mask = MASK_FOR(resultSize);
    
    result = value & mask;

    return true;
}

bool XArgConstant::getReference(X86Sim *sim, XReference &ref)
{
    UNUSED(sim);

    ref.type = RT_Const;
    ref.bitSize = BS_32;
    ref.address = 0;
    ref.value = value;

    return true;
}

/* XAddrExpr 'eval' method implementation */
bool XAddrExprReg::eval(X86Sim *xsim, uint32_t &result)
{
    if (reg->getKind() != XARG_REGISTER) {
        string str = reg->toString();

        reportError("Invalid register '%s' in memory address expression\n", str.c_str());
        return false;
    }

    XArgRegister *r = (XArgRegister *)reg;

    return xsim->getRegValue(r->regId, result);
}

bool XAddrExprConst::eval(X86Sim *sim, uint32_t &result)
{
    UNUSED(sim);

    result = value;

    return true;
}

bool XAddrExpr2Term::eval(X86Sim *xsim, uint32_t &result)
{
    uint32_t v1, v2;

    if ((op == XOP_MINUS) && !expr2->isA(XADDR_EXPR_CONST)) {
        reportError("Invalid expression '%s' in memory reference.", toString().c_str());
        return false;
    }

    if ( (expr1->isA(XADDR_EXPR_REG) && expr2->isA(XADDR_EXPR_MULT)) ||
         (expr1->isA(XADDR_EXPR_REG) && expr2->isA(XADDR_EXPR_CONST)) ||
         (expr1->isA(XADDR_EXPR_REG) && expr2->isA(XADDR_EXPR_REG)) ||
         (expr1->isA(XADDR_EXPR_CONST) && expr2->isA(XADDR_EXPR_MULT)) ||
         (expr1->isA(XADDR_EXPR_CONST) && expr2->isA(XADDR_EXPR_CONST)) ||
         (expr1->isA(XADDR_EXPR_CONST) && expr2->isA(XADDR_EXPR_REG)) ||
         (expr1->isA(XADDR_EXPR_MULT) && expr2->isA(XADDR_EXPR_CONST)) ||
         (expr1->isA(XADDR_EXPR_MULT) && expr2->isA(XADDR_EXPR_REG)) ) {

        if (!expr1->eval(xsim, v1))
            return false;

        if (!expr2->eval(xsim, v2))
            return false;

        if (op == XOP_PLUS)
            result = v1 + v2;
        else
            result = v1 - v2;

        return true;

    } else {
        reportError("Invalid expression '%s' in memory reference.", toString().c_str());
        return false;
    }
}

bool XAddrExpr3Term::eval(X86Sim *xsim, uint32_t &result)
{
    if ((op1 == XOP_MINUS) && !expr2->isA(XADDR_EXPR_CONST)) {
        reportError("Invalid expression '%s' in memory reference.", toString().c_str());
        return false;
    }
    if ((op2 == XOP_MINUS) && !expr3->isA(XADDR_EXPR_CONST)) {
        reportError("Invalid expression '%s' in memory reference.", toString().c_str());
        return false;
    }

    if ( (expr1->isA(XADDR_EXPR_REG) && expr2->isA(XADDR_EXPR_MULT) && expr3->isA(XADDR_EXPR_CONST)) ||
         (expr1->isA(XADDR_EXPR_REG) && expr2->isA(XADDR_EXPR_CONST) && expr3->isA(XADDR_EXPR_MULT)) ||
         (expr1->isA(XADDR_EXPR_CONST) && expr2->isA(XADDR_EXPR_REG) && expr3->isA(XADDR_EXPR_MULT)) ||
         (expr1->isA(XADDR_EXPR_CONST) && expr2->isA(XADDR_EXPR_MULT) && expr3->isA(XADDR_EXPR_REG)) ||
         (expr1->isA(XADDR_EXPR_MULT) && expr2->isA(XADDR_EXPR_CONST) && expr3->isA(XADDR_EXPR_REG)) ||
         (expr1->isA(XADDR_EXPR_MULT) && expr2->isA(XADDR_EXPR_REG) && expr3->isA(XADDR_EXPR_CONST))
         ) {

        uint32_t v1, v2, v3;

        if (!expr1->eval(xsim, v1))
            return false;

        if (!expr2->eval(xsim, v2))
            return false;

        if (!expr3->eval(xsim, v3))
            return false;

        if (op1 == XOP_PLUS)
            result = v1 + v2;
        else
            result = v1 - v2;

        if (op2 == XOP_PLUS)
            result += v3;
        else
            result -= v3;

        return true;

    } else {
        reportError("Invalid expression '%s' in memory reference.", toString().c_str());
        return false;
    }
}

bool XAddrExprMult::eval(X86Sim *xsim, uint32_t &result)
{
    uint32_t v1, v2;

    if (!expr1->eval(xsim, v1))
        return false;

    if (!expr2->eval(xsim, v2))
        return false;

    if (expr1->isA(XADDR_EXPR_REG) &&
        expr2->isA(XADDR_EXPR_CONST)) {
        uint32_t temp = v2;
        v2 = v1;
        v1 = temp;
    } else if ( expr1->isA(XADDR_EXPR_CONST) &&
                expr2->isA(XADDR_EXPR_REG) ) {
    } else {
        reportError("Invalid expression '%s' in memory reference.", toString().c_str());
        return false;
    }

    if ((v1 != 1) && (v1 != 2) && (v1 != 4) && (v1 != 8)) {
        reportError("Invalid scalar multiplier %d in address expression '%s'. "
                    "Valid values are 1, 2, 4, 8.", v1, toString().c_str());
        return false;
    }

    result = v1 * v2;
    return true;
}

/* Instructions implementation */
IMPLEMENT_INSTRUCTION(Mov) {
    XReference ref1;

    if (!arg1->getReference(sim, ref1))
        return false;

    if ((ref1.type == RT_Mem) && (arg2->getKind() == XARG_MEMREF)) {
        reportError("Memory-Memory references are not allowed.\n");
        return false;
    }

    uint32_t value2;

    if (!arg2->eval(sim, ref1.bitSize, 0, value2)) {
        reportError("Invalid argument '%s' in mov instruction.\n", arg2->toString().c_str());
        return false;
    }

    if (ref1.bitSize == 0) ref1.bitSize = BS_32;
    
    result = ref1;

    return sim->setValue(ref1, value2);
}

IMPLEMENT_INSTRUCTION(Movsx) {
    XReference ref1;

    if (!arg1->getReference(sim, ref1))
        return false;

    if ((ref1.type == RT_Mem) && (arg2->getKind() == XARG_MEMREF)) {
        reportError("Memory-Memory references are not allowed.\n");
        return false;
    }

    uint32_t value2;

    if (!arg2->eval(sim, ref1.bitSize, SX_MASK, value2)) {
        reportError("Invalid argument '%s' in movsx instruction.\n", arg2->toString().c_str());
        return false;
    }

    result = ref1;

    return sim->setValue(ref1, value2);
}

IMPLEMENT_INSTRUCTION(Movzx) {
    XReference ref1;

    if (!arg1->getReference(sim, ref1))
        return false;

    if ((ref1.type == RT_Mem) && (arg2->getKind() == XARG_MEMREF)) {
        reportError("Memory-Memory references are not allowed.\n");
        return false;
    }

    uint32_t value2;

    if (!arg2->eval(sim, ref1.bitSize, ZX_MASK, value2)) {
        reportError("Invalid argument '%s' in movzx instruction.\n", arg2->toString().c_str());
        return false;
    }

    result = ref1;

    return sim->setValue(ref1, value2);
}

IMPLEMENT_INSTRUCTION(Push) {
    XReference ref1;

    if (!arg->getReference(sim, ref1)) {
	reportError("getReference error %s\n", arg->toString().c_str());
        return false;
    }

    if (ref1.bitSize < BS_16) {
        reportError("Expected 32 or 16 bits argument. Found %d bit size.\n", ref1.bitSize);
        return false;
    }

    if (!sim->getValue(ref1)) {
        reportError("getValue error %X\n", ref1.address);
        return false;
    }

    uint32_t esp;

    sim->getRegValue(R_ESP, esp);
    esp -= ref1.bitSize / 8;

    if (!sim->writeMem(esp, ref1.value, ref1.bitSize)) {
        reportError("Invalid address '0x%X'. Maybe stack overflow.\n", esp);
        return false;
    }
    sim->setRegValue(R_ESP, esp);

    result.type = RT_Mem;
    result.bitSize = ref1.bitSize;
    result.address = esp;

    return true;
}

IMPLEMENT_INSTRUCTION(Pop) {
    XReference ref1;

    if (!arg->getReference(sim, ref1))
        return false;

    if (ref1.bitSize < BS_16) {
        reportError("Expected 32 or 16 bits argument. Found %d bit size.\n", ref1.bitSize);
        return false;
    }

    uint32_t esp, value;

    sim->getRegValue(R_ESP, esp);

    if (!sim->readMem(esp, value, ref1.bitSize)) {
        reportError("Invalid address '0x%X'.\n", esp);
        return false;
    }
    if (!sim->setValue(ref1, value)) {
        reportError("Maybe a BUG in the machine.\n", esp);
        return false;
    }
    esp += ref1.bitSize / 8;
    sim->setRegValue(R_ESP, esp);

    result = ref1;

    return true;
}

IMPLEMENT_INSTRUCTION(Lea) {
    XReference ref1;

    if (!arg1->getReference(sim, ref1))
        return false;

    if (ref1.type != RT_Reg) {
        reportError("Invalid destination '%s' for 'lea' instruction.\n", arg1->toString().c_str());
        return false;
    }

    XReference ref2;

    if (!arg2->getReference(sim, ref2))
        return false;

    if (ref2.type != RT_Mem) {
        reportError("Second argument of instruction 'lea' should be a memory reference.\n");
        return false;
    }

    if (!sim->setValue(ref1, ref2.address))
        return false;

    result = ref1;

    return true;
}

IMPLEMENT_ALU_INSTRUCTION_2ARG("'add'", Add, XFN_ADD)
IMPLEMENT_ALU_INSTRUCTION_2ARG("'sub'", Sub, XFN_SUB)
IMPLEMENT_ALU_INSTRUCTION_2ARG("'and'", And, XFN_AND)
IMPLEMENT_ALU_INSTRUCTION_2ARG("'or'", Or, XFN_OR)
IMPLEMENT_ALU_INSTRUCTION_2ARG("'xor'", Xor, XFN_XOR)
IMPLEMENT_ALU_INSTRUCTION_2ARG("'cmp'", Cmp, XFN_CMP)
IMPLEMENT_ALU_INSTRUCTION_2ARG("'test'", Test, XFN_TEST)


IMPLEMENT_INSTRUCTION(Shl) {
    XReference ref1, ref2;

    if (!arg1->getReference(sim, ref1))
        return false;

    if (!arg2->getReference(sim, ref2))
        return false;

    if (ref2.type != RT_Reg && ref2.type != RT_Const) {
        reportError("Invalid argument for shift operation. Expected register register or constant.\n");
        return false;
    }

    if ((ref2.type == RT_Reg) && (ref2.address != R_CL)) {
        reportError("Invalid argument for shift operation. Expected register 'cl'.\n");
        return false;
    }

    if (!sim->getValue(ref2)) {
        reportError("Unexpected error %d: %s.\n", __LINE__, __FUNCTION__);
        return false;
    }

    if (!sim->doOperation(XFN_SHL, ref1, ref2.value)) {
        reportError("Invalid argument for operation.\n");
        return false;
    }

    result = ref1;

    return true;
}

IMPLEMENT_INSTRUCTION(Shr) {
    XReference ref1, ref2;

    if (!arg1->getReference(sim, ref1))
        return false;

    if (!arg2->getReference(sim, ref2))
        return false;

    if (ref2.type != RT_Reg && ref2.type != RT_Const) {
        reportError("Invalid argument for shift operation. Expected register register or constant.\n");
        return false;
    }

    if ((ref2.type == RT_Reg) && (ref2.address != R_CL)) {
        reportError("Invalid argument for shift operation. Expected register 'cl'.\n");
        return false;
    }

    if (!sim->getValue(ref2)) {
        reportError("Unexpected error %d: %s.\n", __LINE__, __FUNCTION__);
        return false;
    }

    if (!sim->doOperation(XFN_SHR, ref1, ref2.value)) {
        reportError("Invalid argument for operation.\n");
        return false;
    }

    result = ref1;

    return true;
}

IMPLEMENT_INSTRUCTION(Leave) {
    UNUSED(result);

    uint32_t ebp_value;
    sim->getRegValue(R_EBP,ebp_value);
    sim->setRegValue(R_ESP,ebp_value);

    uint32_t esp, value;

    sim->getRegValue(R_ESP,esp);

    if (!sim->readMem(esp,value,BS_32))
    {
        reportError("Invalid address '0x%X'.\n", esp);
        return false;
    }

    sim->setRegValue(R_EBP,value);

    esp+=4;
    sim->setRegValue(R_ESP,esp);

    return true;
}

IMPLEMENT_INSTRUCTION(Imul1) {
    UNUSED(result);

    reportError("IMUL instruction not supported.\n");
    return false;
}

IMPLEMENT_INSTRUCTION(Imul2) {
    XReference ref1;

    if (!arg1->getReference(sim, ref1))
        return false;

    if ( (ref1.type != RT_Reg) || ((ref1.bitSize != BS_16) && (ref1.bitSize != BS_32)) ) {
        reportError("Invalid destination '%s' for IMUL instruction.\n", arg1->toString().c_str());
        return false;
    }

    XReference ref2;

    if (!arg2->getReference(sim, ref2))
        return false;
    
    switch (ref2.type) {
        case RT_Mem:
        case RT_Reg: {
            if (ref1.bitSize != ref2.bitSize) {
                reportError("Invalid arguments in IMUL instruction.  Both operands should be 16 bits or 32 bits.\n");
                return false;
            }
            break;
        }
        case RT_Const: {
            break;
        }
    }
    
    if (!sim->getValue(ref1)) {
        reportError("Unexpected error %d: %s.\n", __LINE__, __FUNCTION__);
        return false;
    }

    if (!sim->getValue(ref2)) {
        reportError("Unexpected error %d: %s.\n", __LINE__, __FUNCTION__);
        return false;
    }
    
    uint32_t eflags = 0;
    
    switch (ref1.bitSize) {
        case BS_16: {
            int32_t temp = (int32_t)((int16_t)ref1.value) * (int32_t)((int16_t)ref2.value);
            ref1.value = temp & 0xFFFF;
    
            if ( (int32_t)((int16_t)ref1.value) != temp ) {
                eflags = (1 << CF_POS) | (1 << OF_POS);
            }
            break;            
        }
        case BS_32: {
            int64_t temp = (int64_t)((int32_t)ref1.value) * (int64_t)((int32_t)ref2.value);
            ref1.value = temp & 0xFFFFFFFF;
    
            if ( (int64_t)((int32_t)ref1.value) != temp ) {
                eflags = (1 << CF_POS) | (1 << OF_POS);
            }
            break;
        }
    }
    
    sim->setRegValue(R_EFLAGS, eflags);
    
    if (!sim->setValue(ref1, ref1.value)) {
        return false;
    }

    result = ref1;
    
    return true;  
}

IMPLEMENT_INSTRUCTION(Imul3) {
    UNUSED(sim);
    UNUSED(result);

    reportError("IMUL instruction not supported.\n");
    return false;
}

IMPLEMENT_INSTRUCTION(Idiv) {
    UNUSED(sim);
    UNUSED(result);

    reportError("Idiv instruction not supported.\n");
    return false;
}

IMPLEMENT_INSTRUCTION(Mul) {
    XReference ref;
    XReference rVal;
    rVal.type = RT_Reg;

    if (!arg->getReference(sim, ref))
        return false;
    
    uint32_t eflags = 0;

    switch (ref.type) {
        case RT_Mem: {
            reportError("Memory argument for MUL instruction not supported.\n");
            return false;
        }
        case RT_Reg: {
            sim->getValue(ref);
            switch (ref.bitSize) {
                case BS_8: {
                    rVal.address = R_AL;
                    sim->getValue(rVal);
                    uint32_t temp = (uint32_t)((uint8_t)rVal.value) * (uint32_t)((uint8_t)ref.value);
                    rVal.value = temp & 0xFFFF;
                    sim->setRegValue(R_AX,rVal.value);

                    uint32_t value = temp & 0xFFFFFF00;

                    if ( value != 0 ) {
                        eflags = (1 << CF_POS) | (1 << OF_POS);
                    }
                }
                break;
                case BS_16: {
                    rVal.address = R_AX;
                    sim->getValue(rVal);
                    uint32_t temp = (uint32_t)((uint16_t)rVal.value) * (uint32_t)((uint16_t)ref.value);
                    rVal.value = temp & 0xFFFF;
                    uint32_t dx = (temp & 0xFFFF0000) >> BS_16;
                    sim->setRegValue(R_AX,rVal.value);
                    sim->setRegValue(R_DX,dx);

                    uint32_t value = dx & 0xFFFF;

                    if ( value != 0 ) {
                        eflags = (1 << CF_POS) | (1 << OF_POS);
                    }
                }
                break;
                case BS_32: {
                    rVal.address = R_EAX;
                    sim->getValue(rVal);
                    uint64_t temp = (uint64_t)((uint32_t)rVal.value) * (uint64_t)((uint32_t)ref.value);
                    rVal.value = temp & 0xFFFFFFFF;
                    uint32_t edx = (temp & 0xFFFFFFFF00000000) >> BS_32;
                    sim->setRegValue(R_EAX,rVal.value);
                    sim->setRegValue(R_EDX,edx);

                    uint32_t value = edx & 0xFFFFFFFF;

                    if ( value != 0 ) {
                        eflags = (1 << CF_POS) | (1 << OF_POS);
                    }
                }
                break;
            }
        }
        break;
        default: {
            reportError("Invalid argument for MUL instruction '%s'.\n", arg->toString().c_str());
            return false;
        }
    }
    sim->setRegValue(R_EFLAGS, eflags);
    result = rVal;
    
    return true;
}

IMPLEMENT_INSTRUCTION(Div) {
    UNUSED(sim);
    UNUSED(result);

    reportError("Div instruction not supported.\n");
    return false;
}

IMPLEMENT_INSTRUCTION(Inc) {
    XReference ref1;

    if (!arg->getReference(sim, ref1))
        return false;

    if (!sim->doOperation(XFN_ADD, ref1, 1)) {
        reportError("Invalid argument for operation.\n");
        return false;
    }

    result = ref1;

    return true;
}

IMPLEMENT_INSTRUCTION(Dec) {
    XReference ref1;

    if (!arg->getReference(sim, ref1))
        return false;

    if (!sim->doOperation(XFN_SUB, ref1, 1)) {
        reportError("Invalid argument for operation.\n");
        return false;
    }

    result = ref1;

    return true;
}

IMPLEMENT_INSTRUCTION(Not) {
    XReference ref1;

    if (!arg->getReference(sim, ref1))
        return false;

    if (!sim->doOperation(XFN_NOT, ref1, 0)) {
        reportError("Invalid argument for operation.\n");
        return false;
    }

    result = ref1;

    return true;
}

IMPLEMENT_INSTRUCTION(Neg) {
    XReference ref1;

    if (!arg->getReference(sim, ref1))
        return false;

    if (!sim->doOperation(XFN_NEG, ref1, 1)) {
        reportError("Invalid argument for operation.\n");
        return false;
    }

    result = ref1;

    return true;
}

IMPLEMENT_INSTRUCTION(Jmp) {
    UNUSED(result);
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for jump instruction. Expected register, memory reference, label or constant, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    sim->runtime_context->ip = target_addr;

    return true;
}

IMPLEMENT_INSTRUCTION(Jz) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    if (sim->isFlagSet(ZF_MASK)) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Jump to label is not taken. Zero flag is not set
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jnz) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    if (!sim->isFlagSet(ZF_MASK)) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch is not taken. Zero flag is set
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jl) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);

    if (sf != of) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Sign flag is equal to overflow flag.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jg) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);
    bool zf = sim->isFlagSet(ZF_MASK);

    if ((sf == of) && !zf) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Sign flag is not equal to overflow flag or zero flag is set.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jle) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);
    bool zf = sim->isFlagSet(ZF_MASK);

    if ((sf != of) || zf) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Sign flag is equal to overflow flag and Zero flag is not set.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jge) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);

    if (sf == of) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Sign flag is not equal to overflow flag.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jb) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    bool carry_flag = sim->isFlagSet(CF_MASK);

    if (carry_flag) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Carry flag is not set.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Ja) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    bool carry_flag = sim->isFlagSet(CF_MASK);
    bool zero_flag = sim->isFlagSet(ZF_MASK);

    if (!carry_flag && !zero_flag) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Carry flag is set or zero flag is set.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jbe) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    bool carry_flag = sim->isFlagSet(CF_MASK);
    bool zero_flag = sim->isFlagSet(ZF_MASK);

    if (carry_flag || zero_flag) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Carry flag is not set and zero flag is not set.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Jae) {
    UNUSED(result);

    if (arg->getKind() != XARG_CONST &&
        arg->getKind() != XARG_IDENTIFIER) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }
    
    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for conditional branch instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    bool carry_flag = sim->isFlagSet(CF_MASK);

    if (!carry_flag) {
        sim->runtime_context->ip = target_addr;
    } else {
        //Branch to label is not taken. Carry flag is set.
    }

    return true;
}

IMPLEMENT_INSTRUCTION(Call) {
    UNUSED(result);

    uint32_t target_addr;
    
    if (!arg->eval(sim, BS_32, 0, target_addr)) {
        reportError("Invalid argument for call instruction. Expected address, found '%s'.\n",
                    arg->toString().c_str());
        return false;
    }

    uint32_t esp;
    
    sim->getRegValue(R_ESP, esp);
    esp -= 4;

    if (!sim->writeMem(esp, sim->runtime_context->ip, BS_32)) {
        reportError("Invalid address '0x%X'. Maybe stack overflow.\n", esp);
        return false;
    }
    sim->setRegValue(R_ESP, esp);
    
    sim->runtime_context->ip = target_addr;

    return true;
}

IMPLEMENT_INSTRUCTION(Ret) {
    UNUSED(result);

    uint32_t esp, ret_ip;
    
    sim->getRegValue(R_ESP, esp);

    if (!sim->readMem(esp, ret_ip, BS_32)) {
        reportError("Invalid address '0x%X'. Maybe stack overflow.\n", esp);
        return false;
    }
    sim->setRegValue(R_ESP, esp + 4);
    
    sim->runtime_context->ip = ret_ip;
    
    return true;
}

//SETNBE r/m8       Set byte if not below or equal (CF=0 and ZF=0).
//SETA   r/m8         Set byte if above (CF=0 and ZF=0).
IMPLEMENT_INSTRUCTION(Seta) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool cf = sim->isFlagSet(CF_MASK);
    bool zf = sim->isFlagSet(ZF_MASK);
    
    if (!sim->setValue(a_ref, (!cf && !zf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNC r/m8    Set byte if not carry (CF=0).
//SETNB r/m8    Set byte if not below (CF=0).
//SETAE r/m8    Set byte if above or equal (CF=0).
IMPLEMENT_INSTRUCTION(Setae) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool cf = sim->isFlagSet(CF_MASK);
    
    if (!sim->setValue(a_ref, (!cf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNA r/m8    Set byte if not above (CF=1 or ZF=1).
//SETBE r/m8    Set byte if below or equal (CF=1 or ZF=1).
IMPLEMENT_INSTRUCTION(Setbe) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool cf = sim->isFlagSet(CF_MASK);
    bool zf = sim->isFlagSet(ZF_MASK);
    
    if (!sim->setValue(a_ref, (cf || zf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNAE r/m8    Set byte if not above or equal (CF=1).
//SETB r/m8    Set byte if below (CF=1).
//SETC r/m8    Set if carry (CF=1).
IMPLEMENT_INSTRUCTION(Setb) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);
    
    bool cf = sim->isFlagSet(CF_MASK);
    
    if (!sim->setValue(a_ref, cf? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNLE r/m8    Set byte if not less or equal (ZF=0 and SF=OF).
//SETG   r/m8    Set byte if greater (ZF=0 and SF=OF).
IMPLEMENT_INSTRUCTION(Setg) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool zf = sim->isFlagSet(ZF_MASK);
    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);
    
    if (!sim->setValue(a_ref, (!zf && sf == of)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNL r/m8    Set byte if not less (SF=OF).
//SETGE r/m8    Set byte if greater or equal (SF=OF).
IMPLEMENT_INSTRUCTION(Setge) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);
    
    if (!sim->setValue(a_ref, (sf == of)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNGE r/m8    Set if not greater or equal (SF<>OF).
//SETL   r/m8    Set byte if less (SF<>OF).
IMPLEMENT_INSTRUCTION(Setl) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);
    
    if (!sim->setValue(a_ref, (sf != of)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNG r/m8    Set byte if not greater (ZF=1 or SF<>OF).
//SETLE r/m8    Set byte if less or equal (ZF=1 or SF<>OF).
IMPLEMENT_INSTRUCTION(Setle) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool zf = sim->isFlagSet(ZF_MASK);
    bool sf = sim->isFlagSet(SF_MASK);
    bool of = sim->isFlagSet(OF_MASK);
    
    if (!sim->setValue(a_ref, (zf && sf != of)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

//SETNZ r/m8    Set byte if not zero (ZF=0).
//SETNE r/m8    Set byte if not equal (ZF=0).
IMPLEMENT_INSTRUCTION(Setnz) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool zf = sim->isFlagSet(ZF_MASK);

    if (!sim->setValue(a_ref, (!zf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }    
}

//SETNO r/m8    Set byte if not overflow (OF=0).
IMPLEMENT_INSTRUCTION(Setno) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool of = sim->isFlagSet(OF_MASK);

    if (!sim->setValue(a_ref, (!of)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }    
}

//SETPO r/m8    Set byte if parity odd (PF=0).
//SETNP r/m8    Set byte if not parity (PF=0).
IMPLEMENT_INSTRUCTION(Setnp) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool pf = sim->isFlagSet(PF_MASK);

    if (!sim->setValue(a_ref, (!pf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }     
}

//SETNS r/m8    Set byte if not sign (SF=0).
IMPLEMENT_INSTRUCTION(Setns) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool sf = sim->isFlagSet(SF_MASK);

    if (!sim->setValue(a_ref, (!sf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }     
}

//SETO r/m8    Set byte if overflow (OF=1).
IMPLEMENT_INSTRUCTION(Seto) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool of = sim->isFlagSet(OF_MASK);

    if (!sim->setValue(a_ref, (of)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    } 
}

//SETPE r/m8    Set byte if parity even (PF=1).
//SETP r/m8    Set byte if parity (PF=1).
IMPLEMENT_INSTRUCTION(Setp) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);
    
    bool pf = sim->isFlagSet(PF_MASK);

    if (!sim->setValue(a_ref, (pf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }  
}

//SETS r/m8    Set byte if sign (SF=1).
IMPLEMENT_INSTRUCTION(Sets) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool sf = sim->isFlagSet(SF_MASK);
    
    if (!sim->setValue(a_ref, (sf)? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }     
}

//SETE r/m8    Set byte if equal (ZF=1).
//SETZ r/m8    Set byte if zero (ZF=1). 
IMPLEMENT_INSTRUCTION(Setz) {
    XReference a_ref;

    SET_BYTE_ON_CONDITION_INST_PROLOG(a_ref);

    bool zf = sim->isFlagSet(ZF_MASK);
    
    if (!sim->setValue(a_ref, zf? 1 : 0)) {
        reportError("Unknown error on instruction '%s'\n", getName());
        return false;
    }
}

IMPLEMENT_INSTRUCTION(Cdq) {
	uint32_t eax, edx;
	
	edx = 0;
	sim->getRegValue(R_EAX, eax);
	if (SIGN_BIT(eax, 32) != 0) {
		edx = ~edx;
	}
	sim->setRegValue(R_EDX, edx);
	result.type = RT_Reg;
	result.address = R_EDX;

	return true;
}

bool XCmdSet::exec(X86Sim *sim, XReference &result)
{
    result.type = RT_None;

    switch (arg->getKind()) {
        case XARG_REGISTER: {
            XArgRegister *reg = (XArgRegister *)arg;
            uint32_t value = lvalue.front();

            sim->setRegValue(reg->regId, value);

            return true;
        }
        case XARG_MEMREF: {
            XArgMemRef *arg_mref = (XArgMemRef *)arg;
            XBitSize bitSize = arg_mref->sizeDirective;

            if (bitSize == 0) {
                reportError("No size directive specified.\n");
                return false;
            }
            
            XReference ref;
            
            if (!arg_mref->getReference(sim, ref))
                return false;

            list<int>::iterator it = lvalue.begin();
            
            while (it != lvalue.end()) {
                uint32_t value = *it;
                
                if (!sim->setValue(ref, value)) {
                    reportError("Invalid address '0x%X'.\n", ref.address);
                    return false;
                }
                
                switch (ref.bitSize) {
                    case BS_8: ref.address++; break;
                    case BS_16: ref.address += 2; break;
                    case BS_32: ref.address += 4; break;
                    default:
                        return false;
                }
                it++;
            }

            return true;
        }
        default: {
            reportError("Invalid argument '%s' for #set command.\n", arg->toString().c_str());
            return false;
        }
    }
}

bool XCmdShow::exec(X86Sim *sim, XReference &result)
{
    result.type = RT_None;

    switch (arg->getKind()) {
        case XARG_REGISTER: {
            XArgRegister *reg = (XArgRegister *)arg;

            sim->showRegValue(reg->regId, dataFormat);

            return true;
        }
        case XARG_MEMREF: {
            XArgMemRef *mref = (XArgMemRef *)arg;
            XBitSize bitSize = mref->sizeDirective;
            uint32_t vaddr;

            if (bitSize == 0) {
                reportError("No size directive specified.\n");
                return false;
            }
            if (!mref->expr->eval(sim, vaddr))
                return false;

            int i;
            
            for (i = 0; i < mref->count; i++) {
                if (!sim->showMemValue(vaddr, bitSize, dataFormat)) {
                    reportError("Invalid address '0x%X'.\n", vaddr);
                    return false;
                }
                switch (bitSize) {
                    case BS_8: vaddr ++; break;
                    case BS_16: vaddr += 2; break;
                    case BS_32: vaddr += 4; break;
                    default:
                        reportError("BUG in the machine\n");
                        return false;
                }
            }

            return true;
        }
        case XARG_CONST: {
            XReference ref;

            arg->getReference(sim, ref);

            printNumber(ref.value, ref.bitSize, dataFormat);
            printf("\n");

            return true;
        }
        default: {
            reportError("Invalid argument '%s' for #show command.\n", arg->toString().c_str());
            return false;
        }
    }
}

bool XCmdExec::exec(X86Sim *sim, XReference &result)
{
    ifstream in;
    
    in.open(file_path.c_str(), ifstream::in|ifstream::binary);

    if (!in.is_open()) {
        reportError("Cannot open file '%s'\n", file_path.c_str());
        return false;
    }
    
    bool success = true;
    
    if (!sim->exec(&in)) {
        success = false;
    }
        
    in.close();
    
    return success;
}

bool XCmdStop::exec(X86Sim *sim, XReference &result)
{
    sim->runtime_context->stop = true;
    
    return true;
}