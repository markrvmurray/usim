//
//	hd6309in.cpp
//	Individual HD6309-only instruction implementations.
//	(Dispatch, reset and register dump live in hd6309.cpp.)
//
//	vim: ts=8 sw=8 noet:
//

#include "hd6309.h"

//---------------------------------------------------------------------
//
// EXG / TFR with the HD6309 16-entry register set
//
// Reads return 16 bits; an 8-bit source is duplicated into both halves.
// Writes to an 8-bit register take the high byte for A/DP/E and the low
// byte for B/CC/F. Selectors 12 and 13 are the constant-zero register.
//
//---------------------------------------------------------------------

Word hd6309::read_exgtfr(Byte sel)
{
	switch (sel & 0x0f) {
		case  0: return d;
		case  1: return x;
		case  2: return y;
		case  3: return u;
		case  4: return s;
		case  5: return pc;
		case  6: return w;
		case  7: return v;
		case  8: return ((Word)a << 8) | a;
		case  9: return ((Word)b << 8) | b;
		case 10: return ((Word)cc.value << 8) | cc.value;
		case 11: return ((Word)dp << 8) | dp;
		case 12: return 0;
		case 13: return 0;
		case 14: return ((Word)e << 8) | e;
		case 15: return ((Word)f << 8) | f;
	}
	return 0;	// unreachable (sel masked to 4 bits)
}

void hd6309::write_exgtfr(Byte sel, Word val)
{
	switch (sel & 0x0f) {
		case  0: d = val;			break;
		case  1: x = val;			break;
		case  2: y = val;			break;
		case  3: u = val;			break;
		case  4: s = val;			break;
		case  5: pc = val;			break;
		case  6: w = val;			break;
		case  7: v = val;			break;
		case  8: a = (Byte)(val >> 8);		break;	// high byte
		case  9: b = (Byte)(val >> 0);		break;	// low byte
		case 10: cc.value = (Byte)(val >> 0);	break;	// low byte
		case 11: dp = (Byte)(val >> 8);		break;	// high byte
		case 12:				break;	// zero (discard)
		case 13:				break;	// zero (discard)
		case 14: e = (Byte)(val >> 8);		break;	// high byte
		case 15: f = (Byte)(val >> 0);		break;	// low byte
	}
}

void hd6309::exg()
{
	insn = "EXG";
	Byte post = fetch_operand();
	int r1 = (post & 0xf0) >> 4;
	int r2 = (post & 0x0f);

	Word t1 = read_exgtfr(r1);
	Word t2 = read_exgtfr(r2);
	write_exgtfr(r1, t2);
	write_exgtfr(r2, t1);

	cycles += 6;
}

void hd6309::tfr()
{
	insn = "TFR";
	Byte post = fetch_operand();
	int r1 = (post & 0xf0) >> 4;
	int r2 = (post & 0x0f);

	write_exgtfr(r2, read_exgtfr(r1));

	cycles += 4;
}

//---------------------------------------------------------------------
//
// value-in/value-out ALU cores (for the register-register ops, which
// have no memory operand). Flag logic mirrors the byte/word helpers in
// mc6809in.cpp (help_adc/help_sub, addd/subd).
//
//---------------------------------------------------------------------

Byte hd6309::alu_add8(Byte x, Byte m, int cin, bool set_h)
{
	if (set_h) {
		Byte t = (x & 0x0f) + (m & 0x0f) + cin;
		cc.h = btst(t, 4);
	}
	{
		Byte t = (x & 0x7f) + (m & 0x7f) + cin;
		cc.v = btst(t, 7);
	}
	{
		Word t = x + m + cin;
		cc.c = btst(t, 8);
		x = t & 0xff;
	}
	cc.v ^= cc.c;
	cc.n = btst(x, 7);
	cc.z = !x;
	return x;
}

Byte hd6309::alu_sub8(Byte x, Byte m, int cin)
{
	int t = x - m - cin;
	cc.v = btst((Byte)(x ^ m ^ t ^ (t >> 1)), 7);
	cc.c = btst((Word)t, 8);
	cc.n = btst((Byte)t, 7);
	x = t & 0xff;
	cc.z = !x;
	return x;
}

Byte hd6309::alu_and8(Byte x, Byte m)
{
	x &= m;
	cc.v = 0;
	cc.n = btst(x, 7);
	cc.z = !x;
	return x;
}

Byte hd6309::alu_or8(Byte x, Byte m)
{
	x |= m;
	cc.v = 0;
	cc.n = btst(x, 7);
	cc.z = !x;
	return x;
}

Byte hd6309::alu_eor8(Byte x, Byte m)
{
	x ^= m;
	cc.v = 0;
	cc.n = btst(x, 7);
	cc.z = !x;
	return x;
}

Word hd6309::alu_add16(Word x, Word m, int cin)
{
	{
		Word t = (x & 0x7fff) + (m & 0x7fff) + cin;
		cc.v = btst(t, 15);
	}
	{
		DWord t = (DWord)x + m + cin;
		cc.c = btst(t, 16);
		x = (Word)(t & 0xffff);
	}
	cc.v ^= cc.c;
	cc.n = btst(x, 15);
	cc.z = !x;
	return x;
}

Word hd6309::alu_sub16(Word x, Word m, int cin)
{
	int t = x - m - cin;
	cc.v = btst((DWord)(x ^ m ^ t ^ (t >> 1)), 15);
	cc.c = btst((DWord)t, 16);
	cc.n = btst((DWord)t, 15);
	x = t & 0xffff;
	cc.z = !x;
	return x;
}

Word hd6309::alu_and16(Word x, Word m)
{
	x &= m;
	cc.v = 0;
	cc.n = btst(x, 15);
	cc.z = !x;
	return x;
}

Word hd6309::alu_or16(Word x, Word m)
{
	x |= m;
	cc.v = 0;
	cc.n = btst(x, 15);
	cc.z = !x;
	return x;
}

Word hd6309::alu_eor16(Word x, Word m)
{
	x ^= m;
	cc.v = 0;
	cc.n = btst(x, 15);
	cc.z = !x;
	return x;
}

//---------------------------------------------------------------------
//
// register-register ops ($1030-$1037)
//
// Postbyte: high nibble = source, low nibble = destination (same 16-entry
// numbering as EXG/TFR). When the two operands differ in size the 8-bit
// one is PROMOTED to its containing 16-bit register (A/B->D, E/F->W,
// CC/DP/zero->zero) and the op runs 16-bit. ADDR/ADCR do not affect H.
//
//---------------------------------------------------------------------

Word hd6309::rr_read16(int nib)
{
	switch (nib) {
		case  0: return d;
		case  1: return x;
		case  2: return y;
		case  3: return u;
		case  4: return s;
		case  5: return pc;
		case  6: return w;
		case  7: return v;
		case  8: case  9: return d;	// A/B promote to container D
		case 14: case 15: return w;	// E/F promote to container W
		default: return 0;		// CC/DP/zero
	}
}

void hd6309::rr_write16(int nib, Word val)
{
	switch (nib) {
		case  0: d = val; break;
		case  1: x = val; break;
		case  2: y = val; break;
		case  3: u = val; break;
		case  4: s = val; break;
		case  5: pc = val; break;
		case  6: w = val; break;
		case  7: v = val; break;
		case  8: case  9: d = val; break;
		case 14: case 15: w = val; break;
		default: break;			// CC/DP/zero discard
	}
}

Byte hd6309::rr_read8(int nib)
{
	switch (nib) {
		case  8: return a;
		case  9: return b;
		case 10: return cc.value;
		case 11: return dp;
		case 14: return e;
		case 15: return f;
		default: return 0;		// zero (12, 13)
	}
}

void hd6309::rr_write8(int nib, Byte val)
{
	switch (nib) {
		case  8: a = val; break;
		case  9: b = val; break;
		case 10: cc.value = val; break;
		case 11: dp = val; break;
		case 14: e = val; break;
		case 15: f = val; break;
		default: break;			// zero discard
	}
}

void hd6309::register_register_op()
{
	static const char* names[] = {
		"ADDR", "ADCR", "SUBR", "SBCR", "ANDR", "ORR", "EORR", "CMPR"
	};
	int which = ir & 0x0f;			// 0..7
	insn = names[which];

	Byte post = fetch();
	int src = (post >> 4) & 0x0f;
	int dst = (post >> 0) & 0x0f;
	bool promote = ((post & 0x80) != 0) != ((post & 0x08) != 0);
	bool op16 = promote || (src < 8);	// both-8 -> 8-bit, otherwise 16-bit

	if (op16) {
		Word sv = rr_read16(src);
		Word dv = rr_read16(dst);
		Word r = dv;
		switch (which) {
			case 0: r = alu_add16(dv, sv, 0);	break;
			case 1: r = alu_add16(dv, sv, cc.c);	break;
			case 2: r = alu_sub16(dv, sv, 0);	break;
			case 3: r = alu_sub16(dv, sv, cc.c);	break;
			case 4: r = alu_and16(dv, sv);		break;
			case 5: r = alu_or16(dv, sv);		break;
			case 6: r = alu_eor16(dv, sv);		break;
			case 7: alu_sub16(dv, sv, 0);		break;	// CMPR: flags only
		}
		if (which != 7) rr_write16(dst, r);
	} else {
		Byte sv = rr_read8(src);
		Byte dv = rr_read8(dst);
		Byte r = dv;
		switch (which) {
			case 0: r = alu_add8(dv, sv, 0, false);		break;
			case 1: r = alu_add8(dv, sv, cc.c, false);	break;
			case 2: r = alu_sub8(dv, sv, 0);		break;
			case 3: r = alu_sub8(dv, sv, cc.c);		break;
			case 4: r = alu_and8(dv, sv);			break;
			case 5: r = alu_or8(dv, sv);			break;
			case 6: r = alu_eor8(dv, sv);			break;
			case 7: alu_sub8(dv, sv, 0);			break;	// CMPR: flags only
		}
		if (which != 7) rr_write8(dst, r);
	}
	cycles += 1;
}

//---------------------------------------------------------------------
//
// W accumulator load / store and sign-extend
//
//---------------------------------------------------------------------

void hd6309::ldw()
{
	insn = "LDW";
	help_ld(w);
}

void hd6309::stw()
{
	insn = "STW";
	help_st(w);
}

void hd6309::sexw()
{
	insn = "SEXW";
	// sign-extend W into D to form a 32-bit signed value in Q
	d = (w & 0x8000) ? 0xffff : 0x0000;
	cc.n = btst(d, 15);
	cc.z = (d == 0x0000 && w == 0x0000);
	// V and C are intentionally left untouched
	cycles += 4;
}

//---------------------------------------------------------------------
//
// 16-bit ALU helpers (W/D versions of the byte helpers in mc6809in.cpp)
//
// These mirror the byte helpers widened to 16 bits. NB the HD6309
// differences: DEC16/INC16/COM16 affect the carry flag (the 8-bit forms
// of DEC/INC do not).
//
//---------------------------------------------------------------------

void hd6309::help_neg16(Word& x)
{
	DWord t = (DWord)0 - x;
	cc.v = btst((Word)(x ^ t ^ (t >> 1)), 15);
	cc.c = btst(t, 16);
	x = (Word)t;
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_com16(Word& x)
{
	x = ~x;
	cc.c = 1;
	cc.v = 0;
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_lsr16(Word& x)
{
	cc.c = btst(x, 0);
	x >>= 1;
	cc.n = 0;
	cc.z = !x;
}

void hd6309::help_ror16(Word& x)
{
	int oc = cc.c;
	cc.c = btst(x, 0);
	x >>= 1;
	if (oc) bset(x, 15);
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_asr16(Word& x)
{
	cc.c = btst(x, 0);
	x >>= 1;
	if ((cc.n = btst(x, 14)) != 0) {
		bset(x, 15);
	}
	cc.z = !x;
}

void hd6309::help_asl16(Word& x)
{
	cc.c = btst(x, 15);
	cc.v = btst(x, 15) ^ btst(x, 14);
	x <<= 1;
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_rol16(Word& x)
{
	int oc = cc.c;
	cc.v = btst(x, 15) ^ btst(x, 14);
	cc.c = btst(x, 15);
	x <<= 1;
	if (oc) bset(x, 0);
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_dec16(Word& x)
{
	Word a = x;
	DWord t = (DWord)a - 1;
	cc.v = btst((Word)(a ^ 1 ^ t ^ (t >> 1)), 15);
	cc.c = btst(t, 16);
	x = (Word)t;
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_inc16(Word& x)
{
	Word a = x;
	DWord t = (DWord)a + 1;
	cc.v = btst((Word)(a ^ 1 ^ t ^ (t >> 1)), 15);
	cc.c = btst(t, 16);
	x = (Word)t;
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_tst16(Word& x)
{
	cc.v = 0;
	cc.n = btst(x, 15);
	cc.z = !x;
}

void hd6309::help_clr16(Word& x)
{
	cc.value &= 0xf0;	// clear C, V, Z, N
	cc.value |= 0x04;	// set Z
	x = 0;
}

//---------------------------------------------------------------------
//
// LDQ / STQ - the 32-bit Q accumulator (A:B:E:F)
//
//---------------------------------------------------------------------

void hd6309::ldq()
{
	insn = "LDQ";
	DWord q;
	if (mode == immediate) {
		q  = (DWord)fetch() << 24;
		q |= (DWord)fetch() << 16;
		q |= (DWord)fetch() << 8;
		q |= (DWord)fetch();
	} else {
		Word addr = fetch_effective_address();
		q  = (DWord)read(addr + 0) << 24;
		q |= (DWord)read(addr + 1) << 16;
		q |= (DWord)read(addr + 2) << 8;
		q |= (DWord)read(addr + 3);
	}
	set_q(q);
	cc.n = btst(q, 31);
	cc.z = (q == 0);
	cc.v = 0;
}

void hd6309::stq()
{
	insn = "STQ";
	DWord q = get_q();
	Word addr = fetch_effective_address();
	write(addr + 0, (Byte)(q >> 24));
	write(addr + 1, (Byte)(q >> 16));
	write(addr + 2, (Byte)(q >> 8));
	write(addr + 3, (Byte)(q >> 0));
	cc.n = btst(q, 31);
	cc.z = (q == 0);
	cc.v = 0;
}

//---------------------------------------------------------------------
//
// W-register stack operations (push/pull only the W register; no postbyte)
//
//---------------------------------------------------------------------

void hd6309::pshsw()
{
	insn = "PSHSW";
	do_psh(s, w);
	cycles += 2;
}

void hd6309::pulsw()
{
	insn = "PULSW";
	do_pul(s, w);
	cycles += 2;
}

void hd6309::pshuw()
{
	insn = "PSHUW";
	do_psh(u, w);
	cycles += 2;
}

void hd6309::puluw()
{
	insn = "PULUW";
	do_pul(u, w);
	cycles += 2;
}

//---------------------------------------------------------------------
//
// OIM / AIM / EIM / TIM - memory operated on by an immediate mask
//
// Encoding: opcode, immediate mask byte, then the memory address operand
// (direct/indexed/extended per the opcode). Logic-op flags: N, Z, V=0.
// TIM is test-only (no write-back).
//
//---------------------------------------------------------------------

void hd6309::mem_imm_op()
{
	int lo = ir & 0x0f;			// 1=OIM 2=AIM 5=EIM B=TIM
	Byte mask = fetch();			// immediate mask first
	Word addr = fetch_effective_address();	// then the memory address
	Byte m = read(addr);
	Byte r = m;
	switch (lo) {
		case 0x1: insn = "OIM"; r = m | mask; break;
		case 0x2: insn = "AIM"; r = m & mask; break;
		case 0x5: insn = "EIM"; r = m ^ mask; break;
		case 0xb: insn = "TIM"; r = m & mask; break;	// test only
	}
	cc.n = btst(r, 7);
	cc.z = !r;
	cc.v = 0;
	if (lo != 0xb) {
		write(addr, r);
	}
	cycles += 1;
}

//---------------------------------------------------------------------
//
// page-2 16-bit memory ALU on D and W
// (SUBW/CMPW/SBCD/ANDD/BITD/EORD/ADCD/ORD/ADDW). CMPD/CMPY/LDY/STY in the
// same block are handled by the base MC6809, and LDW/STW are above.
//
//---------------------------------------------------------------------

void hd6309::alu16_mem()
{
	int lo = ir & 0x0f;
	Word m = fetch_word_operand();
	switch (lo) {
		case 0x0: insn = "SUBW"; w = alu_sub16(w, m, 0);    break;
		case 0x1: insn = "CMPW"; alu_sub16(w, m, 0);        break;
		case 0x2: insn = "SBCD"; d = alu_sub16(d, m, cc.c); break;
		case 0x4: insn = "ANDD"; d = alu_and16(d, m);       break;
		case 0x5: insn = "BITD"; alu_and16(d, m);           break;
		case 0x8: insn = "EORD"; d = alu_eor16(d, m);       break;
		case 0x9: insn = "ADCD"; d = alu_add16(d, m, cc.c); break;
		case 0xa: insn = "ORD";  d = alu_or16(d, m);        break;
		case 0xb: insn = "ADDW"; w = alu_add16(w, m, 0);    break;
	}
	cycles += 1;
}

//---------------------------------------------------------------------
//
// E/F register ops (page 3). The 8-bit memory ALU reuses the base byte
// helpers; the inherent ops reuse the base read-modify helpers.
//
//---------------------------------------------------------------------

void hd6309::ef_mem(Byte& reg, bool isF)
{
	static const char* en[] = {"SUBE","CMPE",0,0,0,0,"LDE","STE",0,0,0,"ADDE"};
	static const char* fn[] = {"SUBF","CMPF",0,0,0,0,"LDF","STF",0,0,0,"ADDF"};
	int lo = ir & 0x0f;
	insn = (isF ? fn : en)[lo];
	switch (lo) {
		case 0x0: help_sub(reg); break;		// SUBE/SUBF
		case 0x1: help_cmp(reg); break;		// CMPE/CMPF
		case 0x6: help_ld(reg);  break;		// LDE/LDF
		case 0x7: help_st(reg);  break;		// STE/STF
		case 0xb: help_add(reg); break;		// ADDE/ADDF (sets H)
	}
}

void hd6309::ef_inh(Byte& reg, bool isF)
{
	switch (ir & 0x0f) {
		case 0x3: insn = isF ? "COMF" : "COME"; help_com(reg); break;
		case 0xa: insn = isF ? "DECF" : "DECE"; help_dec(reg); break;
		case 0xc: insn = isF ? "INCF" : "INCE"; help_inc(reg); break;
		case 0xd: insn = isF ? "TSTF" : "TSTE"; help_tst(reg); break;
		case 0xf: insn = isF ? "CLRF" : "CLRE"; help_clr(reg); break;
	}
}

//---------------------------------------------------------------------
//
// illegal-instruction / divide-by-zero trap
//
// Shared by the execute_instruction() default (illegal opcode) and the
// DIVD/DIVQ zero-divisor case. Sets the MD status bit, stacks the entire
// machine state and vectors through $fff0. Unlike a hardware interrupt it
// does NOT mask I/F (it behaves like SWI2/SWI3).
//
//---------------------------------------------------------------------

void hd6309::trap(Byte md_bit)
{
	md |= md_bit;
	psh_state(s, u);	// entire frame (incl E/F in native mode); sets CC.E
	pc = read_word(vector_reserved);
	cycles += 12;
}

//---------------------------------------------------------------------
//
// MULD / DIVD / DIVQ (signed multiply and divide)
//
//---------------------------------------------------------------------

void hd6309::muld()
{
	insn = "MULD";
	Word m = fetch_word_operand();
	int32_t result = (int32_t)(int16_t)d * (int32_t)(int16_t)m;
	set_q((DWord)result);
	cc.n = btst((DWord)result, 31);
	cc.z = (result == 0);
	cc.v = 0;
	cc.c = 0;
	cycles += 28;
}

void hd6309::divd()
{
	insn = "DIVD";
	Byte m = fetch_operand();
	if (m == 0) {
		trap(md_div0);
		return;
	}
	int16_t old_d = (int16_t)d;
	int16_t result = old_d / (int8_t)m;
	a = (Byte)(old_d % (int8_t)m);		// remainder -> A
	b = (Byte)result;			// quotient  -> B
	cc.n = btst(b, 7);
	cc.z = !b;
	cc.c = btst(b, 0);
	if (result > 128 || result < -127) {
		cc.v = 1;
		if (result > 256 || result < -255) {
			// hard overflow: division aborted, D restored to |old D|
			cc.n = btst((Word)old_d, 15);
			cc.z = !(Word)old_d;
			d = (Word)(old_d < 0 ? -old_d : old_d);
		}
	} else {
		cc.v = 0;
	}
	cycles += 25;
}

void hd6309::divq()
{
	insn = "DIVQ";
	Word m = fetch_word_operand();
	if (m == 0) {
		trap(md_div0);
		return;
	}
	int32_t old_q = (int32_t)get_q();
	int32_t result = old_q / (int16_t)m;
	d = (Word)(old_q % (int16_t)m);		// remainder -> D
	w = (Word)result;			// quotient  -> W
	cc.n = btst((Word)result, 15);
	cc.z = !(Word)result;
	cc.c = btst((Word)result, 0);
	if (result > 32768 || result < -32767) {
		cc.v = 1;
		if (result > 65536 || result < -65535) {
			// hard overflow: division aborted, Q restored
			if (old_q < 0) cc.n = 1;
			else if (old_q == 0) cc.z = 1;
			set_q((DWord)old_q);
		}
	} else {
		cc.v = 0;
	}
	cycles += 35;
}

//---------------------------------------------------------------------
//
// MD register: LDMD ($113D immediate) and BITMD ($113C immediate)
//
//---------------------------------------------------------------------

void hd6309::ldmd()
{
	insn = "LDMD";
	md = fetch();			// immediate -> MD (only bits 0/1 are functional)
	cc.n = btst(md, 7);
	cc.z = !md;
	cc.v = 0;
	cycles += 5;
}

void hd6309::bitmd()
{
	insn = "BITMD";
	Byte t = md & fetch();		// test MD against the immediate byte
	cc.n = btst(t, 7);
	cc.z = !t;
	cc.v = 0;
	cycles += 4;
}

//---------------------------------------------------------------------
//
// TFM - block transfer ($1138-$113B)
//
// Postbyte: high nibble = source register, low nibble = destination, where
// only 0=D 1=X 2=Y 3=U 4=S are legal. W is the byte count and counts down.
// The four opcodes select the +/- behaviour of the two pointers.
//
// The transfer is interruptible on real hardware: between bytes the chip
// checks for a pending interrupt and, if one is taken, rewinds PC to the
// instruction so it resumes after the handler. We reproduce that here, but
// note that USim ticks peripherals only between instructions, so a pending
// IRQ/FIRQ is already serviced before TFM runs and the in-loop check is
// effectively a no-op in this model (kept for fidelity).
//
//---------------------------------------------------------------------

Word* hd6309::tfm_reg(int n)
{
	switch (n) {
		case 0: return &d;
		case 1: return &x;
		case 2: return &y;
		case 3: return &u;
		case 4: return &s;
	}
	return nullptr;
}

void hd6309::tfm()
{
	insn = "TFM";
	int variant = ir & 0x03;
	Byte post = fetch();
	Word* src = tfm_reg((post >> 4) & 0x0f);
	Word* dst = tfm_reg((post >> 0) & 0x0f);

	if (!src || !dst) {		// illegal register selector
		trap(md_illegal);
		return;
	}

	while (w != 0) {
		// abort + rewind if an unmasked interrupt is pending (see note above)
		if ((!FIRQ && !cc.f) || (!IRQ && !cc.i)) {
			pc -= 3;	// back to the $11 prefix; re-dispatched next tick
			return;
		}
		write(*dst, read(*src));
		switch (variant) {
			case 0: ++(*src); ++(*dst); break;	// R0+,R1+
			case 1: --(*src); --(*dst); break;	// R0-,R1-
			case 2: ++(*src);           break;	// R0+,R1
			case 3:           ++(*dst); break;	// R0,R1+
		}
		--w;
		cycles += 3;
	}
	cycles += 3;
}

//---------------------------------------------------------------------
//
// bit manipulation ($1130-$1137: BAND/BIAND/BOR/BIOR/BEOR/BIEOR/LDBT/STBT)
//
// Encoding: opcode, then a postbyte (bits 7-6 = register 00=CC/01=A/10=B,
// bits 5-3 = source bit number, bits 2-0 = destination bit number), then a
// direct-page address byte naming the memory operand. None of these affect
// CC (unless CC happens to be the selected register).
//
//---------------------------------------------------------------------

void hd6309::bit_op()
{
	static const char* names[] = {
		"BAND", "BIAND", "BOR", "BIOR", "BEOR", "BIEOR", "LDBT", "STBT"
	};
	int op = ir & 0x0f;			// 0..7
	insn = names[op];

	Byte post = fetch();			// register + bit selectors
	Word addr = ((Word)dp << 8) | fetch();	// direct-page memory operand
	Byte memval = read(addr);

	Byte scratch = 0;
	Byte* reg;
	switch (post & 0xc0) {
		case 0x00: reg = &cc.value; break;
		case 0x40: reg = &a; break;
		case 0x80: reg = &b; break;
		default:   reg = &scratch; break;	// "11" is undefined
	}
	int srcbit = (post >> 3) & 0x07;
	int dstbit = (post >> 0) & 0x07;

	if (op == 7) {				// STBT: memory[dstbit] = register[srcbit]
		int rb = (*reg >> srcbit) & 1;
		if (rb) memval |= (1 << dstbit);
		else    memval &= ~(1 << dstbit);
		write(addr, memval);
	} else {
		int membit = (memval >> srcbit) & 1;
		int regbit = (*reg >> dstbit) & 1;
		int result;
		switch (op) {
			case 0: result = regbit & membit;		break;	// BAND
			case 1: result = regbit & (membit ^ 1);		break;	// BIAND
			case 2: result = regbit | membit;		break;	// BOR
			case 3: result = regbit | (membit ^ 1);		break;	// BIOR
			case 4: result = regbit ^ membit;		break;	// BEOR
			case 5: result = regbit ^ (membit ^ 1);		break;	// BIEOR
			default: result = membit;			break;	// LDBT
		}
		if (result) *reg |= (1 << dstbit);
		else        *reg &= ~(1 << dstbit);
	}
	cycles += 6;
}

// dispatch the $104x (on D) / $105x (on W) 16-bit inherent ops by ir nibble
void hd6309::help_inh16(Word& reg)
{
	switch (ir & 0x0f) {
		case 0x0: insn = "NEGW"; help_neg16(reg); break;
		case 0x3: insn = "COMW"; help_com16(reg); break;
		case 0x4: insn = "LSRW"; help_lsr16(reg); break;
		case 0x6: insn = "RORW"; help_ror16(reg); break;
		case 0x7: insn = "ASRW"; help_asr16(reg); break;
		case 0x8: insn = "ASLW"; help_asl16(reg); break;
		case 0x9: insn = "ROLW"; help_rol16(reg); break;
		case 0xa: insn = "DECW"; help_dec16(reg); break;
		case 0xc: insn = "INCW"; help_inc16(reg); break;
		case 0xd: insn = "TSTW"; help_tst16(reg); break;
		case 0xf: insn = "CLRW"; help_clr16(reg); break;
	}
	cycles += 2;
}
