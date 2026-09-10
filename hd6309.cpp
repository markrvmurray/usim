//
//	hd6309.cpp
//	HD6309 reset, register dump and dispatch overrides.
//	The individual HD6309-only instructions live in hd6309in.cpp.
//
//	vim: ts=8 sw=8 noet:
//

#include "hd6309.h"
#include <cstdio>

hd6309::hd6309()
{
}

hd6309::~hd6309()
{
}

void hd6309::reset()
{
	mc6809::reset();	// shared 6809 reset (vectors, D/X/Y, CC, sync/cwai)
	w  = 0x0000;		// clear the W accumulator (and so E and F)
	v  = 0xffff;		// V powers up to $ffff on a real HD6309
	md = 0x00;		// emulation mode, no traps pending
}

void hd6309::print_regs()
{
	mc6809::print_regs();	// the shared PC/CC/S/U/A/B/X/Y/DP line
	fprintf(stderr, "E:%02X F:%02X W:%04X V:%04X MD:%02X (%s)\r\n",
		e, f, w, v, md, native() ? "native" : "emul");
}

//---------------------------------------------------------------------
//
// instruction dispatch
//
// fetch_instruction() reuses the base decoder, then fixes up the
// addressing mode for the HD6309-only $10/$11-page opcodes the base
// leaves with a stale `mode` (the base has no $11 arm and no default in
// its $10 sub-switch). execute_instruction() handles the new encodings
// and delegates every shared opcode back to the base.
//
//---------------------------------------------------------------------

//---------------------------------------------------------------------
//
// effective-address override: adds the HD6309 indexed addressing modes
//   E,R / F,R / W,R          accumulator offsets (postbytes $87/$8a/$8e)
//   ,W / n16,W / ,W++ / ,--W W-base ($8f non-indirect, $80 indirect)
// Every other mode (direct/extended and the standard 6809 indexed forms)
// delegates to the base machinery.
//
//---------------------------------------------------------------------

Word hd6309::fetch_effective_address()
{
	if (mode != indexed) {
		return mc6809::fetch_effective_address();
	}

	post = fetch();

	// The indexed half of the native-mode saving. Applied here rather
	// than after the adders below because subtraction commutes, and at
	// this point `cycles` has already counted the opcode byte or bytes
	// and the postbyte, so it cannot underflow a delta of at most two.
	if (native()) {
		int d = postbyte_native_delta[post];
		if (d > 0 && cycles >= (uint32_t)d) {
			cycles -= (uint32_t)d;
		}
	}

	Byte pbm = post & 0x8f;
	bool indirect = (post & 0x90) == 0x90;

	bool w_base = (pbm == 0x8f && !indirect) || (pbm == 0x80 && indirect);
	bool acc_off = (pbm == 0x87 || pbm == 0x8a || pbm == 0x8e);

	if (!w_base && !acc_off) {
		// standard 6809 indexed mode: reuse the base machinery
		do_predecrement();
		Word addr = fetch_indexed_operand();
		do_postincrement();
		if (btst(post, 4) && btst(post, 7)) {
			++cycles;
			addr = read_word(addr);
		}
		return addr;
	}

	Word addr = 0;
	if (acc_off) {
		Word base = ix_refreg(post);			// X/Y/U/S from bits 6-5
		// E,R / F,R / W,R all cost one extra cycle over ,R in both MD
		// modes, which is two here once the postbyte's own dead cycle
		// is included (see the indexed table in the 6x09 reference).
		if (pbm == 0x87)      { addr = base + extend8(e); cycles += 2; }	// E,R
		else if (pbm == 0x8a) { addr = base + extend8(f); cycles += 2; }	// F,R
		else                  { addr = base + w;          cycles += 2; }	// W,R
	} else {
		switch ((post >> 5) & 3) {			// W-base sub-mode
			case 0: addr = w;                cycles += 1; break;	// ,W
			// n16,W is two extra cycles over ,W, and fetch_word has
			// already charged the two offset bytes.
			case 1: addr = w + fetch_word(); cycles += 1; break;	// n16,W
			case 2: addr = w; w += 2;        cycles += 2; break;	// ,W++
			case 3: w -= 2; addr = w;        cycles += 2; break;	// ,--W
		}
	}

	if (indirect) {
		++cycles;
		addr = read_word(addr);
	}
	return addr;
}

void hd6309::fetch_instruction()
{
	mc6809::fetch_instruction();

	// HD6309-only opcodes whose addressing mode the base mis-decodes.
	// (Populated as instruction groups are added.)
	switch (ir) {
		case 0x14:			// SEXW
			mode = inherent;
			break;
		default:
			break;
	}
}

// An unrecognised opcode takes the illegal-instruction trap rather than
// aborting the simulator (the base behaviour).
void hd6309::illegal_opcode()
{
	trap(md_illegal);
}

// Push the entire register state for an interrupt or SWI.  In native mode
// the HD6309 stacks E and F as well, inserting them between B and DP so the
// frame matches OS-9's 14-byte register image (CC,A,B,E,F,DP,X,Y,U,PC).  In
// emulation mode this is exactly the 6809 12-byte frame.  Sets the entire
// (E) flag in the saved CC, like the base 6809 interrupt sequence.
void hd6309::psh_state(Word& sp, Word& usp)
{
	cc.e = 1;
	do_psh(sp, pc);
	do_psh(sp, usp);
	do_psh(sp, y);
	do_psh(sp, x);
	do_psh(sp, dp);
	if (native()) {
		do_psh(sp, f);
		do_psh(sp, e);
	}
	do_psh(sp, b);
	do_psh(sp, a);
	do_psh(sp, cc.value);
}

// FIRQ normally stacks only PC and CC (the fast frame). When MD bit 1 is
// set the HD6309 stacks the entire register state instead, like IRQ.
void hd6309::do_firq()
{
	if (firq_entire()) {
		if (!waiting_cwai) {
			psh_state(s, u);
		}
		cc.f = cc.i = 1;
		pc = read_word(vector_firq);
	} else {
		mc6809::do_firq();
	}
}

void hd6309::do_irq()
{
	if (!waiting_cwai) {
		psh_state(s, u);
	}
	cc.f = cc.i = 1;
	pc = read_word(vector_irq);
}

void hd6309::do_nmi()
{
	if (!waiting_cwai) {
		psh_state(s, u);
	}
	cc.f = cc.i = 1;
	pc = read_word(vector_nmi);
}

void hd6309::swi()
{
	insn = "SWI";
	psh_state(s, u);
	cc.f = cc.i = 1;
	pc = read_word(vector_swi);
	cycles += 4;
}

void hd6309::swi2()
{
	insn = "SWI2";
	psh_state(s, u);
	pc = read_word(vector_swi2);
	cycles += 4;
}

void hd6309::swi3()
{
	insn = "SWI3";
	psh_state(s, u);
	pc = read_word(vector_swi3);
	cycles += 4;
}

// Pop the matching interrupt/SWI frame.  In native mode an entire-state
// frame (CC.E set) includes E and F between B and DP.
void hd6309::rti()
{
	insn = "RTI";
	do_pul(s, cc.value);
	if (cc.e) {
		do_pul(s, a);
		do_pul(s, b);
		if (native()) {
			do_pul(s, e);
			do_pul(s, f);
		}
		do_pul(s, dp);
		do_pul(s, x);
		do_pul(s, y);
		do_pul(s, u);
		do_pul(s, pc);
	} else {
		do_pul(s, pc);
	}
	cycles += 2;
}

// CWAI pre-stacks the entire frame and waits for an interrupt; in native
// mode that frame includes E and F, matching psh_state / rti.
void hd6309::cwai()
{
	insn = "CWAI";
	Byte n = fetch_operand();
	cc.value &= n;
	psh_state(s, u);	// entire frame (incl E/F in native mode); sets CC.E
	// 22 in emulation mode, less the opcode and immediate bytes and the
	// twelve stack writes psh_state has already charged. Native mode is
	// 20, but psh_state writes two more bytes there, so the saving is
	// four - see the native delta table.
	cycles += 8;
	waiting_cwai = true;
}


//---------------------------------------------------------------------
//
// Native-mode instruction timing
//
// In emulation mode (MD bit 0 clear) the HD6309 is cycle-identical to
// the MC6809, which is what the shared mc6809 code already charges. In
// native mode it drops dead cycles - never bus cycles - so the whole
// correction is a subtraction, and it splits cleanly in two:
//
//   native_delta[page][opcode]   the instruction body, plus the dead
//                                cycle the direct and extended
//                                addressing preambles carry
//   postbyte_native_delta[post]  the indexed addressing mode
//
// The split is not an invention; it is how both reference tables are
// written. `LDA indexed` is published as "4+" with a blank native
// column because the saving belongs to the postbyte, while `LDA direct`
// is published as "4 (3)" because that one belongs to the opcode. An
// indexed instruction may draw on both tables - the 16-bit ALU ops and
// JSR have a body dead cycle of their own - and the two never overlap.
//
// Derived from Darren Atkinson's "6x09 Instruction Sets" (measured on
// real silicon; the opcode tables and the Indexed Addressing Mode Table)
// cross-checked against Chet Simpson's HD6309 Technical Reference Guide.
// Where the two disagreed the raw text was read and Atkinson followed:
// RTS is 5/4 (the TRG's "1" cannot be right - RTS makes three bus
// accesses), ORCC is 3 flat in both modes (the TRG gives ANDCC 3 flat
// and ORCC 3/2, which cannot both be true of two identically shaped
// instructions), LDW and LDY immediate are 4 flat, and DIVQ immediate
// is 34 (the TRG's 36 would make immediate cost the same as direct,
// which no other multiply or divide does).
//
// Three groups are deliberately zero here because the bus model already
// gets them right on its own: SWI, SWI2, SWI3 and RTI stack two extra
// bytes in native mode (E and F), and those writes are charged as bus
// cycles, which is exactly the published +2. CWAI carries 4 rather than
// its published delta of 2 for the same reason - it pays the +2 too, so
// it has to give back four to land on 20.
//
// Long conditional branches are absent from the table because their
// saving is conditional: the extra cycle exists only when the branch is
// taken, so charge_native_delta works it out from the resulting PC.
//
const int8_t hd6309::native_delta[3][256] = {
    {  // page 0
	 1,  0,  0,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  2,  1,  1,   // $00
	 0,  0,  1,  1,  0,  0,  1,  2,  0,  1,  0,  0,  0,  1,  3,  2,   // $10
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $20
	 0,  0,  0,  0,  1,  1,  1,  1,  0,  1,  2,  0,  4,  1,  0,  0,   // $30
	 1,  0,  0,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  1,  0,  1,   // $40
	 1,  0,  0,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  1,  0,  1,   // $50
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,   // $60
	 1,  0,  0,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  2,  1,  1,   // $70
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  0,  0,   // $80
	 1,  1,  1,  2,  1,  1,  1,  1,  1,  1,  1,  1,  2,  1,  1,  1,   // $90
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  0,  0,   // $A0
	 1,  1,  1,  2,  1,  1,  1,  1,  1,  1,  1,  1,  2,  1,  1,  1,   // $B0
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $C0
	 1,  1,  1,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,   // $D0
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $E0
	 1,  1,  1,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,   // $F0
    },
    {  // page 2 ($10xx)
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $00
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $10
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $20
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $30
	 1,  0,  0,  1,  1,  0,  1,  1,  1,  1,  1,  0,  1,  1,  0,  1,   // $40
	 0,  0,  0,  1,  1,  0,  1,  0,  0,  1,  1,  0,  1,  1,  0,  1,   // $50
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $60
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $70
	 1,  1,  1,  1,  1,  1,  0,  0,  1,  1,  1,  1,  1,  0,  0,  0,   // $80
	 2,  2,  2,  2,  2,  2,  1,  1,  2,  2,  2,  2,  2,  0,  1,  1,   // $90
	 1,  1,  1,  1,  1,  1,  0,  0,  1,  1,  1,  1,  1,  0,  0,  0,   // $A0
	 2,  2,  2,  2,  2,  2,  1,  1,  2,  2,  2,  2,  2,  0,  1,  1,   // $B0
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $C0
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,   // $D0
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $E0
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  1,  1,   // $F0
    },
    {  // page 3 ($11xx)
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $00
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $10
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $20
	 1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0,  0,   // $30
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  1,  0,  1,  1,  0,  1,   // $40
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  1,  0,  1,  1,  0,  1,   // $50
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $60
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $70
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,   // $80
	 1,  1,  0,  2,  0,  0,  1,  1,  0,  0,  0,  1,  2,  1,  1,  1,   // $90
	 0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $A0
	 1,  1,  0,  2,  0,  0,  1,  1,  0,  0,  0,  1,  2,  1,  1,  1,   // $B0
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $C0
	 1,  1,  0,  0,  0,  0,  1,  0,  0,  0,  0,  1,  0,  0,  0,  0,   // $D0
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $E0
	 1,  1,  0,  0,  0,  0,  1,  0,  0,  0,  0,  1,  0,  0,  0,  0,   // $F0
    },
};

const int8_t hd6309::postbyte_native_delta[256] = {
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $00
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $10
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $20
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $30
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $40
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $50
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $60
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // $70
	 1,  1,  1,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  0,   // $80
	 0,  1,  0,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  1,   // $90
	 1,  1,  1,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  0,   // $A0
	 0,  1,  0,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  0,   // $B0
	 1,  1,  1,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  0,   // $C0
	 0,  1,  0,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  0,   // $D0
	 1,  1,  1,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  0,   // $E0
	 0,  1,  0,  1,  0,  0,  0,  0,  0,  1,  0,  2,  0,  2,  0,  0,   // $F0
};

// Subtract this instruction's native-mode saving. Called at the end of
// execute_instruction, so the emulation-mode cost has already been
// charged in full.
void hd6309::charge_native_delta()
{
	if (!native()) {
		return;
	}

	int page, opcode;
	if (ir < 0x0100)		{ page = 0; opcode = ir; }
	else if ((ir >> 8) == 0x10)	{ page = 1; opcode = ir & 0xff; }
	else if ((ir >> 8) == 0x11)	{ page = 2; opcode = ir & 0xff; }
	else				{ return; }

	int d = native_delta[page][opcode];

	// $1021-$102F are the long conditional branches: 5 cycles when not
	// taken in either mode, 6 when taken in emulation mode and 5 when
	// taken in native mode. The not-taken PC is the instruction address
	// plus its four bytes, so anything else means it branched.
	if (page == 1 && opcode >= 0x21 && opcode <= 0x2f) {
		d = (pc != (Word)(insn_pc + 4)) ? 1 : 0;
	}

	// The saving is always dead cycles this instruction has already
	// charged, so it cannot exceed them; the guard is belt and braces
	// against a table entry that is wrong rather than a real case.
	if (d > 0 && cycles >= (uint32_t)d) {
		cycles -= (uint32_t)d;
	}
}

void hd6309::execute_instruction()
{
	switch (ir) {
		case 0x14:				// SEXW
			sexw(); break;
		case 0x1086: case 0x1096: case 0x10a6: case 0x10b6:
			ldw(); break;		// LDW imm/dir/idx/ext
		case 0x1097: case 0x10a7: case 0x10b7:
			stw(); break;		// STW dir/idx/ext

		// 16-bit inherent ops on D ($104x) and W ($105x)
		case 0x1040: case 0x1043: case 0x1044: case 0x1046: case 0x1047:
		case 0x1048: case 0x1049: case 0x104a: case 0x104c: case 0x104d:
		case 0x104f:
			help_inh16(d); break;
		case 0x1050: case 0x1053: case 0x1054: case 0x1056: case 0x1057:
		case 0x1058: case 0x1059: case 0x105a: case 0x105c: case 0x105d:
		case 0x105f:
			help_inh16(w); break;

		// LDQ ($cd immediate; $10dc/ec/fc dir/idx/ext) and STQ ($10dd/ed/fd)
		case 0x00cd: case 0x10dc: case 0x10ec: case 0x10fc:
			ldq(); break;
		case 0x10dd: case 0x10ed: case 0x10fd:
			stq(); break;

		// W-register stack operations
		case 0x1038: pshsw(); break;
		case 0x1039: pulsw(); break;
		case 0x103a: pshuw(); break;
		case 0x103b: puluw(); break;

		// register-register ops: ADDR/ADCR/SUBR/SBCR/ANDR/ORR/EORR/CMPR
		case 0x1030: case 0x1031: case 0x1032: case 0x1033:
		case 0x1034: case 0x1035: case 0x1036: case 0x1037:
			register_register_op(); break;

		// bit manipulation: BAND/BIAND/BOR/BIOR/BEOR/BIEOR/LDBT/STBT
		case 0x1130: case 0x1131: case 0x1132: case 0x1133:
		case 0x1134: case 0x1135: case 0x1136: case 0x1137:
			bit_op(); break;

		// signed multiply / divide (imm/dir/idx/ext)
		case 0x118f: case 0x119f: case 0x11af: case 0x11bf:
			muld(); break;
		case 0x118d: case 0x119d: case 0x11ad: case 0x11bd:
			divd(); break;
		case 0x118e: case 0x119e: case 0x11ae: case 0x11be:
			divq(); break;

		// MD register and block transfer
		case 0x113d: ldmd(); break;
		case 0x113c: bitmd(); break;
		case 0x1138: case 0x1139: case 0x113a: case 0x113b:
			tfm(); break;

		// OIM/AIM/EIM/TIM: memory operated on by an immediate mask
		case 0x0001: case 0x0002: case 0x0005: case 0x000b:	// direct
		case 0x0061: case 0x0062: case 0x0065: case 0x006b:	// indexed
		case 0x0071: case 0x0072: case 0x0075: case 0x007b:	// extended
			mem_imm_op(); break;

		// page-2 16-bit memory ALU on D and W (SUBW/CMPW/SBCD/ANDD/BITD/
		// EORD/ADCD/ORD/ADDW). LDW/STW handled above; CMPD/CMPY/LDY/STY base.
		case 0x1080: case 0x1081: case 0x1082: case 0x1084: case 0x1085:
		case 0x1088: case 0x1089: case 0x108a: case 0x108b:
		case 0x1090: case 0x1091: case 0x1092: case 0x1094: case 0x1095:
		case 0x1098: case 0x1099: case 0x109a: case 0x109b:
		case 0x10a0: case 0x10a1: case 0x10a2: case 0x10a4: case 0x10a5:
		case 0x10a8: case 0x10a9: case 0x10aa: case 0x10ab:
		case 0x10b0: case 0x10b1: case 0x10b2: case 0x10b4: case 0x10b5:
		case 0x10b8: case 0x10b9: case 0x10ba: case 0x10bb:
			alu16_mem(); break;

		// E-register inherent and memory ops
		case 0x1143: case 0x114a: case 0x114c: case 0x114d: case 0x114f:
			ef_inh(e, false); break;
		case 0x1180: case 0x1181: case 0x1186: case 0x118b:
		case 0x1190: case 0x1191: case 0x1196: case 0x1197: case 0x119b:
		case 0x11a0: case 0x11a1: case 0x11a6: case 0x11a7: case 0x11ab:
		case 0x11b0: case 0x11b1: case 0x11b6: case 0x11b7: case 0x11bb:
			ef_mem(e, false); break;

		// F-register inherent and memory ops
		case 0x1153: case 0x115a: case 0x115c: case 0x115d: case 0x115f:
			ef_inh(f, true); break;
		case 0x11c0: case 0x11c1: case 0x11c6: case 0x11cb:
		case 0x11d0: case 0x11d1: case 0x11d6: case 0x11d7: case 0x11db:
		case 0x11e0: case 0x11e1: case 0x11e6: case 0x11e7: case 0x11eb:
		case 0x11f0: case 0x11f1: case 0x11f6: case 0x11f7: case 0x11fb:
			ef_mem(f, true); break;

		// EXG ($1e) / TFR ($1f) are dispatched by the base to the now
		// virtual exg()/tfr() overrides, so they need no case here.
		default:
			mc6809::execute_instruction();
			break;
	}

	charge_native_delta();
}
