/*
 * cpu_interp.c — full 6502 interpreter
 *
 * Used as a fallback when call_by_address() receives an address
 * not in the dispatch table (bank-switched code, dynamic JMP, etc.)
 *
 * cpu_interp_run(pc) — execute instructions from pc until RTS/RTI
 * cpu_interp_step()  — execute one instruction, advance cpu.PC
 */

#include "runner.h"

/* Canonical NMOS 6502 base cycle counts per opcode (without page-cross /
 * branch-taken penalties). Used for FCEUX-style PPU "catch-up" before a PPU
 * register access in interp mode: a $2002 (etc.) read is observed with the PPU
 * advanced to the read's cycle, matching FCEUX's FCEUPPU_LineUpdate, instead of
 * the PPU lagging a whole instruction behind. See ppu_read() / runner_run(). */
const uint8_t cpu_base_cycles[256] = {
/*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/*00*/   7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
/*10*/   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*20*/   6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
/*30*/   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*40*/   6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
/*50*/   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*60*/   6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
/*70*/   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*80*/   2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
/*90*/   2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
/*A0*/   2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
/*B0*/   2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
/*C0*/   2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
/*D0*/   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
/*E0*/   2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
/*F0*/   2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
};

/* =========================================================================
   Helpers
   ========================================================================= */
#define RD(a)    mem_read(a)
#define WR(a,v)  mem_write(a,v)

static inline uint16_t rd16(uint16_t a) {
    return RD(a) | ((uint16_t)RD((uint16_t)(a+1)) << 8);
}
/* 6502 page-wrap bug for indirect JMP */
static inline uint16_t rd16_bug(uint16_t a) {
    uint16_t hi = (a & 0xFF00) | ((a+1) & 0x00FF);
    return RD(a) | ((uint16_t)RD(hi) << 8);
}

/* =========================================================================
   Execute one instruction at cpu.PC, advance cpu.PC
   Returns cycle count for that instruction (approximate)
   ========================================================================= */
int cpu_interp_step(void) {
    uint16_t pc = cpu.PC;
    uint8_t  op = RD(pc);
    int cycles = 2;

#define IMM  (cpu.PC += 2, RD(pc+1))
#define ZP   (cpu.PC += 2, RD(pc+1))
#define ZPX  (cpu.PC += 2, (uint8_t)(RD(pc+1) + cpu.X))
#define ZPY  (cpu.PC += 2, (uint8_t)(RD(pc+1) + cpu.Y))
#define ABS  (cpu.PC += 3, (uint16_t)(RD(pc+1) | ((uint16_t)RD(pc+2)<<8)))
#define ABSX (cpu.PC += 3, (uint16_t)((RD(pc+1) | ((uint16_t)RD(pc+2)<<8)) + cpu.X))
#define ABSY (cpu.PC += 3, (uint16_t)((RD(pc+1) | ((uint16_t)RD(pc+2)<<8)) + cpu.Y))
#define IZX  (cpu.PC += 2, rd16((uint8_t)(RD(pc+1)+cpu.X)))
#define IZY  (cpu.PC += 2, (uint16_t)(rd16(RD(pc+1)) + cpu.Y))

/* read value for given mode */
#define RV_IMM  RD(pc+1)
#define RV_ZP   RD(ZP)
#define RV_ZPX  RD(ZPX)
#define RV_ZPY  RD(ZPY)
#define RV_ABS  RD(ABS)
#define RV_ABSX RD(ABSX)
#define RV_ABSY RD(ABSY)
#define RV_IZX  RD(IZX)
#define RV_IZY  RD(IZY)

    uint8_t  t8;
    uint16_t t16, addr;
    int8_t   rel;

    switch (op) {
    /* --- LDA --- */
    case 0xA9: cpu.A=RD(pc+1);    cpu.PC+=2; SET_NZ(cpu.A); break;
    case 0xA5: cpu.A=RD(RD(pc+1));cpu.PC+=2; SET_NZ(cpu.A); cycles=3; break;
    case 0xB5: cpu.A=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; SET_NZ(cpu.A); cycles=4; break;
    case 0xAD: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; cpu.A=RD(addr); cpu.PC+=3; SET_NZ(cpu.A); cycles=4; break;
    case 0xBD: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.X; cpu.A=RD(addr); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); break; }
    case 0xB9: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y; cpu.A=RD(addr); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    case 0xA1: cpu.A=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; SET_NZ(cpu.A); cycles=6; break;
    case 0xB1: { uint16_t _b=rd16(RD(pc+1)); addr=_b+cpu.Y; cpu.A=RD(addr); cpu.PC+=2; SET_NZ(cpu.A); cycles=5+(((_b&0xFF)+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    /* --- LDX --- */
    case 0xA2: cpu.X=RD(pc+1); cpu.PC+=2; SET_NZ(cpu.X); break;
    case 0xA6: cpu.X=RD(RD(pc+1)); cpu.PC+=2; SET_NZ(cpu.X); cycles=3; break;
    case 0xB6: cpu.X=RD((uint8_t)(RD(pc+1)+cpu.Y)); cpu.PC+=2; SET_NZ(cpu.X); cycles=4; break;
    case 0xAE: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; cpu.X=RD(addr); cpu.PC+=3; SET_NZ(cpu.X); cycles=4; break;
    case 0xBE: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y; cpu.X=RD(addr); cpu.PC+=3; SET_NZ(cpu.X); cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    /* --- LDY --- */
    case 0xA0: cpu.Y=RD(pc+1); cpu.PC+=2; SET_NZ(cpu.Y); break;
    case 0xA4: cpu.Y=RD(RD(pc+1)); cpu.PC+=2; SET_NZ(cpu.Y); cycles=3; break;
    case 0xB4: cpu.Y=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; SET_NZ(cpu.Y); cycles=4; break;
    case 0xAC: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; cpu.Y=RD(addr); cpu.PC+=3; SET_NZ(cpu.Y); cycles=4; break;
    case 0xBC: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.X; cpu.Y=RD(addr); cpu.PC+=3; SET_NZ(cpu.Y); cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); break; }
    /* --- STA --- */
    case 0x85: WR(RD(pc+1),cpu.A); cpu.PC+=2; cycles=3; break;
    case 0x95: WR((uint8_t)(RD(pc+1)+cpu.X),cpu.A); cpu.PC+=2; cycles=4; break;
    case 0x8D: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; WR(addr,cpu.A); cpu.PC+=3; cycles=4; break;
    case 0x9D: addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; WR(addr,cpu.A); cpu.PC+=3; cycles=5; break;
    case 0x99: addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.Y; WR(addr,cpu.A); cpu.PC+=3; cycles=5; break;
    case 0x81: WR(rd16((uint8_t)(RD(pc+1)+cpu.X)),cpu.A); cpu.PC+=2; cycles=6; break;
    case 0x91: WR(rd16(RD(pc+1))+cpu.Y,cpu.A); cpu.PC+=2; cycles=6; break;
    /* --- STX --- */
    case 0x86: WR(RD(pc+1),cpu.X); cpu.PC+=2; cycles=3; break;
    case 0x96: WR((uint8_t)(RD(pc+1)+cpu.Y),cpu.X); cpu.PC+=2; cycles=4; break;
    case 0x8E: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; WR(addr,cpu.X); cpu.PC+=3; cycles=4; break;
    /* --- STY --- */
    case 0x84: WR(RD(pc+1),cpu.Y); cpu.PC+=2; cycles=3; break;
    case 0x94: WR((uint8_t)(RD(pc+1)+cpu.X),cpu.Y); cpu.PC+=2; cycles=4; break;
    case 0x8C: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; WR(addr,cpu.Y); cpu.PC+=3; cycles=4; break;
    /* --- Transfer --- */
    case 0xAA: cpu.X=cpu.A; cpu.PC++; SET_NZ(cpu.X); break;
    case 0x8A: cpu.A=cpu.X; cpu.PC++; SET_NZ(cpu.A); break;
    case 0xA8: cpu.Y=cpu.A; cpu.PC++; SET_NZ(cpu.Y); break;
    case 0x98: cpu.A=cpu.Y; cpu.PC++; SET_NZ(cpu.A); break;
    case 0xBA: cpu.X=cpu.SP; cpu.PC++; SET_NZ(cpu.X); break;
    case 0x9A: cpu.SP=cpu.X; cpu.PC++; break;
    /* --- Stack --- */
    case 0x48: stack_push(cpu.A); cpu.PC++; cycles=3; break;
    case 0x68: cpu.A=stack_pop(); cpu.PC++; SET_NZ(cpu.A); cycles=4; break;
    case 0x08: stack_push(get_P()|0x30); cpu.PC++; cycles=3; break;
    case 0x28: set_P(stack_pop()); cpu.PC++; cycles=4; break;
    /* --- ADC --- */
    case 0x69: t8=RD(pc+1); cpu.PC+=2; goto do_adc;
    case 0x65: t8=RD(RD(pc+1)); cpu.PC+=2; cycles=3; goto do_adc;
    case 0x75: t8=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; cycles=4; goto do_adc;
    case 0x6D: t8=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; cycles=4; goto do_adc;
    case 0x7D: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr); cpu.PC+=3; cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); goto do_adc; }
    case 0x79: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y; t8=RD(addr); cpu.PC+=3; cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); goto do_adc; }
    case 0x61: t8=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; cycles=6; goto do_adc;
    case 0x71: { uint16_t _b=rd16(RD(pc+1)); t8=RD(_b+cpu.Y); cpu.PC+=2; cycles=5+(((_b&0xFF)+(uint16_t)cpu.Y>0xFF)?1:0); }
    do_adc: { uint16_t r=cpu.A+t8+cpu.C;
              cpu.V=((~(cpu.A^t8))&(cpu.A^r)&0x80)?1:0;
              cpu.C=(r>0xFF)?1:0; cpu.A=(uint8_t)r; SET_NZ(cpu.A); break; }
    /* --- SBC --- */
    case 0xE9: t8=RD(pc+1); cpu.PC+=2; goto do_sbc;
    case 0xE5: t8=RD(RD(pc+1)); cpu.PC+=2; cycles=3; goto do_sbc;
    case 0xF5: t8=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; cycles=4; goto do_sbc;
    case 0xED: t8=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; cycles=4; goto do_sbc;
    case 0xFD: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr); cpu.PC+=3; cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); goto do_sbc; }
    case 0xF9: { uint8_t _lo=RD(pc+1); addr=(_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y; t8=RD(addr); cpu.PC+=3; cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); goto do_sbc; }
    case 0xE1: t8=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; cycles=6; goto do_sbc;
    case 0xF1: { uint16_t _b=rd16(RD(pc+1)); t8=RD(_b+cpu.Y); cpu.PC+=2; cycles=5+(((_b&0xFF)+(uint16_t)cpu.Y>0xFF)?1:0); }
    do_sbc: { uint16_t r=cpu.A-t8-(1-cpu.C);
              cpu.V=(((cpu.A^t8))&(cpu.A^r)&0x80)?1:0;
              cpu.C=(r<0x100)?1:0; cpu.A=(uint8_t)r; SET_NZ(cpu.A); break; }
    /* --- AND --- */
    case 0x29: cpu.A&=RD(pc+1); cpu.PC+=2; SET_NZ(cpu.A); break;
    case 0x25: cpu.A&=RD(RD(pc+1)); cpu.PC+=2; SET_NZ(cpu.A); cycles=3; break;
    case 0x35: cpu.A&=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; SET_NZ(cpu.A); cycles=4; break;
    case 0x2D: cpu.A&=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; SET_NZ(cpu.A); cycles=4; break;
    case 0x3D: { uint8_t _lo=RD(pc+1); cpu.A&=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.X); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); break; }
    case 0x39: { uint8_t _lo=RD(pc+1); cpu.A&=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    case 0x21: cpu.A&=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; SET_NZ(cpu.A); cycles=6; break;
    case 0x31: { uint16_t _b=rd16(RD(pc+1)); cpu.A&=RD(_b+cpu.Y); cpu.PC+=2; SET_NZ(cpu.A); cycles=5+(((_b&0xFF)+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    /* --- ORA --- */
    case 0x09: cpu.A|=RD(pc+1); cpu.PC+=2; SET_NZ(cpu.A); break;
    case 0x05: cpu.A|=RD(RD(pc+1)); cpu.PC+=2; SET_NZ(cpu.A); cycles=3; break;
    case 0x15: cpu.A|=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; SET_NZ(cpu.A); cycles=4; break;
    case 0x0D: cpu.A|=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; SET_NZ(cpu.A); cycles=4; break;
    case 0x1D: { uint8_t _lo=RD(pc+1); cpu.A|=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.X); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); break; }
    case 0x19: { uint8_t _lo=RD(pc+1); cpu.A|=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    case 0x01: cpu.A|=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; SET_NZ(cpu.A); cycles=6; break;
    case 0x11: { uint16_t _b=rd16(RD(pc+1)); cpu.A|=RD(_b+cpu.Y); cpu.PC+=2; SET_NZ(cpu.A); cycles=5+(((_b&0xFF)+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    /* --- EOR --- */
    case 0x49: cpu.A^=RD(pc+1); cpu.PC+=2; SET_NZ(cpu.A); break;
    case 0x45: cpu.A^=RD(RD(pc+1)); cpu.PC+=2; SET_NZ(cpu.A); cycles=3; break;
    case 0x55: cpu.A^=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; SET_NZ(cpu.A); cycles=4; break;
    case 0x4D: cpu.A^=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; SET_NZ(cpu.A); cycles=4; break;
    case 0x5D: { uint8_t _lo=RD(pc+1); cpu.A^=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.X); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); break; }
    case 0x59: { uint8_t _lo=RD(pc+1); cpu.A^=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y); cpu.PC+=3; SET_NZ(cpu.A); cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    case 0x41: cpu.A^=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; SET_NZ(cpu.A); cycles=6; break;
    case 0x51: { uint16_t _b=rd16(RD(pc+1)); cpu.A^=RD(_b+cpu.Y); cpu.PC+=2; SET_NZ(cpu.A); cycles=5+(((_b&0xFF)+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    /* --- BIT --- */
    case 0x24: t8=RD(RD(pc+1)); cpu.PC+=2; cpu.N=(t8>>7)&1; cpu.V=(t8>>6)&1; cpu.Z=(cpu.A&t8)?0:1; cycles=3; break;
    case 0x2C: t8=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; cpu.N=(t8>>7)&1; cpu.V=(t8>>6)&1; cpu.Z=(cpu.A&t8)?0:1; cycles=4; break;
    /* --- CMP --- */
    case 0xC9: t8=RD(pc+1); cpu.PC+=2; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); break;
    case 0xC5: t8=RD(RD(pc+1)); cpu.PC+=2; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); cycles=3; break;
    case 0xD5: t8=RD((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); cycles=4; break;
    case 0xCD: t8=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); cycles=4; break;
    case 0xDD: { uint8_t _lo=RD(pc+1); t8=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.X); cpu.PC+=3; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); cycles=4+((_lo+(uint16_t)cpu.X>0xFF)?1:0); break; }
    case 0xD9: { uint8_t _lo=RD(pc+1); t8=RD((_lo|(uint16_t)RD(pc+2)<<8)+cpu.Y); cpu.PC+=3; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); cycles=4+((_lo+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    case 0xC1: t8=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); cycles=6; break;
    case 0xD1: { uint16_t _b=rd16(RD(pc+1)); t8=RD(_b+cpu.Y); cpu.PC+=2; cpu.C=(cpu.A>=t8)?1:0; SET_NZ((uint8_t)(cpu.A-t8)); cycles=5+(((_b&0xFF)+(uint16_t)cpu.Y>0xFF)?1:0); break; }
    /* --- CPX --- */
    case 0xE0: t8=RD(pc+1); cpu.PC+=2; cpu.C=(cpu.X>=t8)?1:0; SET_NZ((uint8_t)(cpu.X-t8)); break;
    case 0xE4: t8=RD(RD(pc+1)); cpu.PC+=2; cpu.C=(cpu.X>=t8)?1:0; SET_NZ((uint8_t)(cpu.X-t8)); cycles=3; break;
    case 0xEC: t8=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; cpu.C=(cpu.X>=t8)?1:0; SET_NZ((uint8_t)(cpu.X-t8)); cycles=4; break;
    /* --- CPY --- */
    case 0xC0: t8=RD(pc+1); cpu.PC+=2; cpu.C=(cpu.Y>=t8)?1:0; SET_NZ((uint8_t)(cpu.Y-t8)); break;
    case 0xC4: t8=RD(RD(pc+1)); cpu.PC+=2; cpu.C=(cpu.Y>=t8)?1:0; SET_NZ((uint8_t)(cpu.Y-t8)); cycles=3; break;
    case 0xCC: t8=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; cpu.C=(cpu.Y>=t8)?1:0; SET_NZ((uint8_t)(cpu.Y-t8)); cycles=4; break;
    /* --- INC --- */
    case 0xE6: addr=RD(pc+1); t8=RD(addr)+1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=5; break;
    case 0xF6: addr=(uint8_t)(RD(pc+1)+cpu.X); t8=RD(addr)+1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=6; break;
    case 0xEE: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; t8=RD(addr)+1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=6; break;
    case 0xFE: addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr)+1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=7; break;
    /* --- DEC --- */
    case 0xC6: addr=RD(pc+1); t8=RD(addr)-1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=5; break;
    case 0xD6: addr=(uint8_t)(RD(pc+1)+cpu.X); t8=RD(addr)-1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=6; break;
    case 0xCE: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; t8=RD(addr)-1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=6; break;
    case 0xDE: addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr)-1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=7; break;
    /* --- INX/INY/DEX/DEY --- */
    case 0xE8: cpu.X++; cpu.PC++; SET_NZ(cpu.X); break;
    case 0xC8: cpu.Y++; cpu.PC++; SET_NZ(cpu.Y); break;
    case 0xCA: cpu.X--; cpu.PC++; SET_NZ(cpu.X); break;
    case 0x88: cpu.Y--; cpu.PC++; SET_NZ(cpu.Y); break;
    /* --- ASL --- */
    case 0x0A: cpu.C=(cpu.A>>7)&1; cpu.A<<=1; cpu.PC++; SET_NZ(cpu.A); break;
    case 0x06: addr=RD(pc+1); t8=RD(addr); cpu.C=(t8>>7)&1; t8<<=1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=5; break;
    case 0x16: addr=(uint8_t)(RD(pc+1)+cpu.X); t8=RD(addr); cpu.C=(t8>>7)&1; t8<<=1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=6; break;
    case 0x0E: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; t8=RD(addr); cpu.C=(t8>>7)&1; t8<<=1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=6; break;
    case 0x1E: addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr); cpu.C=(t8>>7)&1; t8<<=1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=7; break;
    /* --- LSR --- */
    case 0x4A: cpu.C=cpu.A&1; cpu.A>>=1; cpu.PC++; SET_NZ(cpu.A); break;
    case 0x46: addr=RD(pc+1); t8=RD(addr); cpu.C=t8&1; t8>>=1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=5; break;
    case 0x56: addr=(uint8_t)(RD(pc+1)+cpu.X); t8=RD(addr); cpu.C=t8&1; t8>>=1; WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=6; break;
    case 0x4E: addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; t8=RD(addr); cpu.C=t8&1; t8>>=1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=6; break;
    case 0x5E: addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr); cpu.C=t8&1; t8>>=1; WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=7; break;
    /* --- ROL --- */
    case 0x2A: { uint8_t c=cpu.C; cpu.C=(cpu.A>>7)&1; cpu.A=(uint8_t)((cpu.A<<1)|c); cpu.PC++; SET_NZ(cpu.A); break; }
    case 0x26: { uint8_t c=cpu.C; addr=RD(pc+1); t8=RD(addr); cpu.C=(t8>>7)&1; t8=(uint8_t)((t8<<1)|c); WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=5; break; }
    case 0x36: { uint8_t c=cpu.C; addr=(uint8_t)(RD(pc+1)+cpu.X); t8=RD(addr); cpu.C=(t8>>7)&1; t8=(uint8_t)((t8<<1)|c); WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=6; break; }
    case 0x2E: { uint8_t c=cpu.C; addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; t8=RD(addr); cpu.C=(t8>>7)&1; t8=(uint8_t)((t8<<1)|c); WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=6; break; }
    case 0x3E: { uint8_t c=cpu.C; addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr); cpu.C=(t8>>7)&1; t8=(uint8_t)((t8<<1)|c); WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=7; break; }
    /* --- ROR --- */
    case 0x6A: { uint8_t c=cpu.C; cpu.C=cpu.A&1; cpu.A=(uint8_t)((cpu.A>>1)|(c<<7)); cpu.PC++; SET_NZ(cpu.A); break; }
    case 0x66: { uint8_t c=cpu.C; addr=RD(pc+1); t8=RD(addr); cpu.C=t8&1; t8=(uint8_t)((t8>>1)|(c<<7)); WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=5; break; }
    case 0x76: { uint8_t c=cpu.C; addr=(uint8_t)(RD(pc+1)+cpu.X); t8=RD(addr); cpu.C=t8&1; t8=(uint8_t)((t8>>1)|(c<<7)); WR(addr,t8); cpu.PC+=2; SET_NZ(t8); cycles=6; break; }
    case 0x6E: { uint8_t c=cpu.C; addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; t8=RD(addr); cpu.C=t8&1; t8=(uint8_t)((t8>>1)|(c<<7)); WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=6; break; }
    case 0x7E: { uint8_t c=cpu.C; addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; t8=RD(addr); cpu.C=t8&1; t8=(uint8_t)((t8>>1)|(c<<7)); WR(addr,t8); cpu.PC+=3; SET_NZ(t8); cycles=7; break; }
    /* --- Flags --- */
    case 0x18: cpu.C=0; cpu.PC++; break;
    case 0x38: cpu.C=1; cpu.PC++; break;
    case 0x58: cpu.I=0; cpu.PC++; break;
    case 0x78: cpu.I=1; cpu.PC++; break;
    case 0xD8: cpu.D=0; cpu.PC++; break;
    case 0xF8: cpu.D=1; cpu.PC++; break;
    case 0xB8: cpu.V=0; cpu.PC++; break;
    /* --- NOP (official + common undoc) --- */
    case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA:
        cpu.PC++; break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:
        cpu.PC+=2; break;  /* NOP imm */
    case 0x04: case 0x44: case 0x64:
        cpu.PC+=2; cycles=3; break;  /* NOP zp */
    case 0x0C:
        cpu.PC+=3; cycles=4; break;  /* NOP abs */
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4:
        cpu.PC+=2; cycles=4; break;  /* NOP zpx */
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        cpu.PC+=3; cycles=4; break;  /* NOP abx */
    /* --- Branches --- */
    /* branches: 2 cycles not taken, 3 taken, 4 taken+page-cross */
#define BRANCH(cond) do { \
    rel=(int8_t)RD(pc+1); cpu.PC+=2; \
    if(cond){ uint16_t old=cpu.PC; cpu.PC+=(uint16_t)rel; \
              cycles = ((old^cpu.PC)&0xFF00) ? 4 : 3; } \
    } while(0)
    case 0x90: BRANCH(!cpu.C); break;
    case 0xB0: BRANCH( cpu.C); break;
    case 0xF0: BRANCH( cpu.Z); break;
    case 0xD0: BRANCH(!cpu.Z); break;
    case 0x30: BRANCH( cpu.N); break;
    case 0x10: BRANCH(!cpu.N); break;
    case 0x50: BRANCH(!cpu.V); break;
    case 0x70: BRANCH( cpu.V); break;
#undef BRANCH
    /* --- JMP --- */
    case 0x4C: cpu.PC=RD(pc+1)|(uint16_t)RD(pc+2)<<8; cycles=3; break;
    case 0x6C: cpu.PC=rd16_bug(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cycles=5; break;
    /* --- JSR --- */
    case 0x20: {
        uint16_t tgt=RD(pc+1)|(uint16_t)RD(pc+2)<<8;
        uint16_t ret=pc+2;
        stack_push((ret>>8)&0xFF);
        stack_push(ret&0xFF);
        cpu.PC=tgt;
        cycles=6;
        break;
    }
    /* --- RTS --- */
    case 0x60: {
        uint16_t lo=stack_pop();
        uint16_t hi=stack_pop();
        cpu.PC=(lo|(hi<<8))+1;
        cycles=6;
        break;
    }
    /* --- RTI --- */
    case 0x40: {
        set_P(stack_pop());
        uint16_t lo=stack_pop();
        uint16_t hi=stack_pop();
        cpu.PC=lo|(hi<<8);
        cycles=6;
        break;
    }
    /* --- BRK --- */
    case 0x00: {
        uint16_t ret=pc+2;
        stack_push((ret>>8)&0xFF);
        stack_push(ret&0xFF);
        stack_push(get_P()|0x30);
        cpu.I=1;
        cpu.PC=RD(0xFFFE)|(uint16_t)RD(0xFFFF)<<8;
        cycles=7;
        break;
    }
    /* --- LAX (undoc) --- */
    case 0xA7: cpu.A=cpu.X=RD(RD(pc+1)); cpu.PC+=2; SET_NZ(cpu.A); cycles=3; break;
    case 0xB7: cpu.A=cpu.X=RD((uint8_t)(RD(pc+1)+cpu.Y)); cpu.PC+=2; SET_NZ(cpu.A); cycles=4; break;
    case 0xAF: cpu.A=cpu.X=RD(RD(pc+1)|(uint16_t)RD(pc+2)<<8); cpu.PC+=3; SET_NZ(cpu.A); cycles=4; break;
    case 0xBF: cpu.A=cpu.X=RD((RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.Y); cpu.PC+=3; SET_NZ(cpu.A); cycles=4; break;
    case 0xA3: cpu.A=cpu.X=RD(rd16((uint8_t)(RD(pc+1)+cpu.X))); cpu.PC+=2; SET_NZ(cpu.A); cycles=6; break;
    case 0xB3: cpu.A=cpu.X=RD(rd16(RD(pc+1))+cpu.Y); cpu.PC+=2; SET_NZ(cpu.A); cycles=5; break;
    /* --- SAX (undoc) --- */
    case 0x87: WR(RD(pc+1),(uint8_t)(cpu.A&cpu.X)); cpu.PC+=2; cycles=3; break;
    case 0x97: WR((uint8_t)(RD(pc+1)+cpu.Y),(uint8_t)(cpu.A&cpu.X)); cpu.PC+=2; cycles=4; break;
    case 0x8F: WR(RD(pc+1)|(uint16_t)RD(pc+2)<<8,(uint8_t)(cpu.A&cpu.X)); cpu.PC+=3; cycles=4; break;
    case 0x83: WR(rd16((uint8_t)(RD(pc+1)+cpu.X)),(uint8_t)(cpu.A&cpu.X)); cpu.PC+=2; cycles=6; break;
    /* --- ISB / ISC (INC + SBC, undoc) --- */
#define DO_ISB(ea) do { uint8_t _t=(uint8_t)(RD(ea)+1); WR((ea),_t); \
    { uint16_t _r=(uint16_t)cpu.A-_t-(1-cpu.C); \
      cpu.V=(((cpu.A^_t))&(cpu.A^_r)&0x80)?1:0; \
      cpu.C=(_r<0x100)?1:0; cpu.A=(uint8_t)_r; SET_NZ(cpu.A); } } while(0)
    case 0xE3: { addr=rd16((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; cycles=8; DO_ISB(addr); break; }
    case 0xE7: { addr=RD(pc+1); cpu.PC+=2; cycles=5; DO_ISB(addr); break; }
    case 0xEF: { addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; cpu.PC+=3; cycles=6; DO_ISB(addr); break; }
    case 0xF3: { addr=rd16(RD(pc+1))+cpu.Y; cpu.PC+=2; cycles=8; DO_ISB(addr); break; }
    case 0xF7: { addr=(uint8_t)(RD(pc+1)+cpu.X); cpu.PC+=2; cycles=6; DO_ISB(addr); break; }
    case 0xFB: { addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.Y; cpu.PC+=3; cycles=7; DO_ISB(addr); break; }
    case 0xFF: { addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; cpu.PC+=3; cycles=7; DO_ISB(addr); break; }
#undef DO_ISB
    /* --- SLO (ASL + ORA, undoc) --- */
#define DO_SLO(ea) do { uint8_t _t=RD(ea); cpu.C=(_t>>7)&1; _t=(uint8_t)(_t<<1); WR((ea),_t); cpu.A|=_t; SET_NZ(cpu.A); } while(0)
    case 0x03: { addr=rd16((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; cycles=8; DO_SLO(addr); break; }
    case 0x07: { addr=RD(pc+1); cpu.PC+=2; cycles=5; DO_SLO(addr); break; }
    case 0x0F: { addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; cpu.PC+=3; cycles=6; DO_SLO(addr); break; }
    case 0x13: { addr=rd16(RD(pc+1))+cpu.Y; cpu.PC+=2; cycles=8; DO_SLO(addr); break; }
    case 0x17: { addr=(uint8_t)(RD(pc+1)+cpu.X); cpu.PC+=2; cycles=6; DO_SLO(addr); break; }
    case 0x1B: { addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.Y; cpu.PC+=3; cycles=7; DO_SLO(addr); break; }
    case 0x1F: { addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; cpu.PC+=3; cycles=7; DO_SLO(addr); break; }
#undef DO_SLO
    /* --- RRA (ROR + ADC, undoc) --- */
#define DO_RRA(ea) do { uint8_t _t=RD(ea),_c=cpu.C; cpu.C=_t&1; _t=(uint8_t)((_t>>1)|(_c<<7)); WR((ea),_t); \
    { uint16_t _r=(uint16_t)cpu.A+_t+cpu.C; \
      cpu.V=((~(cpu.A^_t))&(cpu.A^_r)&0x80)?1:0; \
      cpu.C=(_r>0xFF)?1:0; cpu.A=(uint8_t)_r; SET_NZ(cpu.A); } } while(0)
    case 0x63: { addr=rd16((uint8_t)(RD(pc+1)+cpu.X)); cpu.PC+=2; cycles=8; DO_RRA(addr); break; }
    case 0x67: { addr=RD(pc+1); cpu.PC+=2; cycles=5; DO_RRA(addr); break; }
    case 0x6F: { addr=RD(pc+1)|(uint16_t)RD(pc+2)<<8; cpu.PC+=3; cycles=6; DO_RRA(addr); break; }
    case 0x73: { addr=rd16(RD(pc+1))+cpu.Y; cpu.PC+=2; cycles=8; DO_RRA(addr); break; }
    case 0x77: { addr=(uint8_t)(RD(pc+1)+cpu.X); cpu.PC+=2; cycles=6; DO_RRA(addr); break; }
    case 0x7B: { addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.Y; cpu.PC+=3; cycles=7; DO_RRA(addr); break; }
    case 0x7F: { addr=(RD(pc+1)|(uint16_t)RD(pc+2)<<8)+cpu.X; cpu.PC+=3; cycles=7; DO_RRA(addr); break; }
#undef DO_RRA

    default:
        /* Unknown opcode — skip 1 byte */
        cpu.PC++;
        break;
    }

    (void)t16; /* suppress unused warning */
    g_cpu_cycles += cycles;
    return cycles;
}

/* =========================================================================
   cpu_interp_run_cycles — FCEUX X6502_Run analogue for the --interp=fceux
   chunk-driven loop: execute whole instructions until at least `cyc` CPU cycles
   have run, stepping the APU per cycle. Does NOT step the PPU and does NOT
   deliver interrupts — the fceux main loop owns PPU timing and fires NMI/IRQ at
   scanline-chunk boundaries. `g_fceux_chunk_cyc` exposes cycles run so far in
   the current chunk so a mid-chunk $2002 read can resolve the exact PPU dot
   (FCEUX's lazy FCEUPPU_LineUpdate). Returns actual cycles run (may overshoot
   `cyc` by the last instruction).
   ========================================================================= */
int g_fceux_chunk_cyc = 0;
int cpu_interp_run_cycles(int cyc) {
    int ran = 0;
    g_fceux_chunk_cyc = 0;
    while (ran < cyc) {
        g_cpu_cycles = 0;
        cpu_interp_step();
        int c = g_cpu_cycles ? (int)g_cpu_cycles : 1;
        for (int i = 0; i < c; i++) apu_step();
        ran += c;
        g_fceux_chunk_cyc = ran;
        g_cpu_cycles = 0;
    }
    return ran;
}

/* =========================================================================
   cpu_interp_run — interpret from addr until RTS/RTI
   Used as fallback from call_by_address()
   ========================================================================= */
void cpu_interp_run(uint16_t entry) {
    /* Save current SP — return when stack is back to the initial level
       (simulates return from JSR) */
    cpu.PC = entry;
    uint8_t base_sp = cpu.SP;
    int limit = 0x200000;  /* hang guard */
    while (limit-- > 0) {
        uint8_t op = mem_read(cpu.PC);
        /* RTS — return to the stack level we entered at */
        if (op == 0x60) {
            uint16_t lo = stack_pop();
            uint16_t hi = stack_pop();
            cpu.PC = (lo | (hi << 8)) + 1;
            /* SP back to base_sp — we have returned from our function */
            if (cpu.SP == base_sp) return;
            continue;
        }
        /* RTI */
        if (op == 0x40) {
            set_P(stack_pop());
            uint16_t lo = stack_pop();
            uint16_t hi = stack_pop();
            cpu.PC = lo | (hi << 8);
            if (cpu.SP >= base_sp) return;
            continue;
        }
        cpu_interp_step();
    }
}
