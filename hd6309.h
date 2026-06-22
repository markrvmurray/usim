//
//	hd6309.h
//	Class definition for the Hitachi HD6309 microprocessor
//
//	Derives from mc6809 and adds the HD6309 extensions: the E/F/W/Q/V/MD
//	registers, the extra $10/$11-page instruction set, and the
//	illegal-instruction / divide-by-zero trap (vector $fff0).
//
//	vim: ts=8 sw=8 noet:
//

#pragma once

#include "mc6809.h"

class hd6309 : public mc6809 {

public:		// MD register bit masks
	enum : Byte {
		md_native	= 0x01,	// native (1) vs emulation (0) mode
		md_firq_entire	= 0x02,	// FIRQ stacks the entire register state
		md_illegal	= 0x40,	// set by the illegal-instruction trap
		md_div0		= 0x80,	// set by the divide-by-zero trap
	};

protected:	// HD6309-only registers

	// W accumulator = E:F (E high byte, F low byte), mirroring the base
	// A:B = D union. NB: intentional UB type-aliasing, exactly as mc6809's
	// d/a/b union does it.
	union {
		Word		w;	// Combined accumulator (E:F)
		struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
			Byte	f;	// Accumulator f (W low byte)
			Byte	e;	// Accumulator e (W high byte)
#elif __BYTE_ORDER == __BIG_ENDIAN
			Byte	e;	// Accumulator e (W high byte)
			Byte	f;	// Accumulator f (W low byte)
#else
#error "target byte order cannot be determined"
#endif
		};
	};

	Word			v;	// V register (undocumented on 6809, real here)
	Byte			md;	// MD mode / status register

	// Q = A:B:E:F = D:W. d and w live in separate unions, so Q is composed
	// on demand rather than overlaid; never assume struct adjacency.
	DWord			get_q() const { return ((DWord)d << 16) | w; }
	void			set_q(DWord q) { d = (Word)(q >> 16); w = (Word)q; }

	bool			native() const { return md & md_native; }
	bool			firq_entire() const { return md & md_firq_entire; }

protected:	// dispatch overrides (extend the base, delegate everything shared)
	virtual void		fetch_instruction() override;
	virtual void		execute_instruction() override;

	// HD6309 adds E,R / F,R / W,R accumulator-offset indexed modes and the
	// ,W / n,W / ,W++ / ,--W W-base modes (with indirect variants)
	virtual Word		fetch_effective_address() override;

	// EXG/TFR extended to the HD6309 16-entry register set, with the
	// asymmetric 8<->16 promotion rules (see hd6309in.cpp)
	virtual void		exg() override;
	virtual void		tfr() override;

protected:	// HD6309 register-selector access (value semantics, 16-entry)
	Word			read_exgtfr(Byte sel);
	void			write_exgtfr(Byte sel, Word val);

protected:	// value-in/value-out ALU cores for register-register ops
	Byte			alu_add8(Byte x, Byte m, int cin, bool set_h);
	Byte			alu_sub8(Byte x, Byte m, int cin);
	Byte			alu_and8(Byte x, Byte m);
	Byte			alu_or8(Byte x, Byte m);
	Byte			alu_eor8(Byte x, Byte m);
	Word			alu_add16(Word x, Word m, int cin);
	Word			alu_sub16(Word x, Word m, int cin);
	Word			alu_and16(Word x, Word m);
	Word			alu_or16(Word x, Word m);
	Word			alu_eor16(Word x, Word m);

	// register-register operand access (promote maps A/B->D, E/F->W)
	Word			rr_read16(int nib);
	void			rr_write16(int nib, Word val);
	Byte			rr_read8(int nib);
	void			rr_write8(int nib, Byte val);
	void			register_register_op();	// $1030-$1037

protected:	// 16-bit (W/D) versions of the byte ALU helpers
	void			help_neg16(Word&);
	void			help_com16(Word&);
	void			help_lsr16(Word&);
	void			help_ror16(Word&);
	void			help_asr16(Word&);
	void			help_asl16(Word&);
	void			help_rol16(Word&);
	void			help_dec16(Word&);
	void			help_inc16(Word&);
	void			help_tst16(Word&);
	void			help_clr16(Word&);
	void			help_inh16(Word&);	// dispatch $104x/$105x by ir nibble

protected:	// HD6309-only instruction implementations (hd6309in.cpp)
	void			ldw(), stw();
	void			sexw();
	void			ldq(), stq();
	void			pshsw(), pulsw(), pshuw(), puluw();
	void			bit_op();		// $1130-$1137 BAND..STBT
	void			muld(), divd(), divq();
	void			ldmd(), bitmd();	// $113D / $113C
	void			tfm();			// $1138-$113B block transfer
	Word*			tfm_reg(int n);		// TFM register select (D/X/Y/U/S)

	void			mem_imm_op();		// OIM/AIM/EIM/TIM (memory & immediate)
	void			alu16_mem();		// page-2 16-bit D/W memory ALU
	void			ef_mem(Byte& reg, bool isF);	// E/F 8-bit memory ALU
	void			ef_inh(Byte& reg, bool isF);	// E/F 8-bit inherent ops

protected:	// illegal-instruction / divide-by-zero trap (vector $fff0)
	void			trap(Byte md_bit);
	virtual void		illegal_opcode() override;	// take the trap
	virtual void		do_firq() override;		// MD bit1 entire-state

public:
				hd6309();
	virtual			~hd6309();

	virtual void		reset() override;
	virtual void		print_regs() override;

};
