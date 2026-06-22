; test6309.asm - HD6309 functional test suite for USim
;
; Assembled with lwtools' lwasm:
;     lwasm --6309 --format=raw -o tests/test6309.bin tests/test6309.asm
;
; Loaded at $0400. The program runs each feature test in turn, storing the
; current test number in RESULT before each block. On the first mismatch it
; branches to 'fail' and self-traps, leaving the failing test number in
; RESULT. If every test passes it stores $FF in RESULT and self-traps.
;
; The host harness (test6309.cpp) reads RESULT ($0010): $FF = pass, otherwise
; the value is the number of the test that failed. The same image also runs
; on MAME's llvm6309 driver for cross-validation.
;
; vim: ts=8 sw=8 noet:

RESULT      equ   $0010             ; pass/fail result cell (host reads this)
STACK_TOP   equ   $0400             ; stack grows down from here; code grows up

            org   $0400

start
            lds   #STACK_TOP        ; set up the stack
            clra                    ; a = 0
            tfr   a,dp              ; direct page = 0
            clr   RESULT            ; result = 0 (not yet passed)

; ---------------------------------------------------------------------
; test 1: plain 6809 sanity - the HD6309 must still run 6809 code
; ---------------------------------------------------------------------
            lda   #1
            sta   RESULT            ; mark "running test 1"
            lda   #$5a              ; load a known value
            cmpa  #$5a              ; compare against itself
            lbne  fail              ; must be equal

; ---------------------------------------------------------------------
; test 2: LDW / STW (immediate, direct, extended) and the W register
; ---------------------------------------------------------------------
            lda   #2
            sta   RESULT
            ldw   #$1234            ; LDW immediate ($1086)
            stw   <$30              ; STW direct ($1097): [$30]=$12 [$31]=$34
            ldw   #0                ; wipe W
            ldw   <$30              ; LDW direct ($1096): W=$1234
            tfr   w,d               ; move W into D so we can compare it
            cmpd  #$1234
            lbne  fail
            ldw   #$5678
            stw   >$2000            ; STW extended ($10b7) to high RAM clear of code
            ldw   #0                ; wipe W
            ldw   >$2000            ; LDW extended ($10b6): W=$5678
            tfr   w,d
            cmpd  #$5678
            lbne  fail

; ---------------------------------------------------------------------
; test 3: TFR with the extended registers (W, E, F)
; ---------------------------------------------------------------------
            lda   #3
            sta   RESULT
            ldw   #$abcd            ; W=$abcd so E=$ab, F=$cd
            tfr   w,d               ; D = W
            cmpd  #$abcd
            lbne  fail
            clra
            tfr   e,a               ; A = E = $ab
            cmpa  #$ab
            lbne  fail
            clrb
            tfr   f,b               ; B = F = $cd
            cmpb  #$cd
            lbne  fail

; ---------------------------------------------------------------------
; test 4: EXG (16-bit swap, D <-> W)
; ---------------------------------------------------------------------
            lda   #4
            sta   RESULT
            ldd   #$1111
            ldw   #$2222
            exg   d,w               ; D=$2222, W=$1111
            cmpd  #$2222
            lbne  fail
            tfr   w,d               ; bring the swapped W back into D
            cmpd  #$1111
            lbne  fail

; ---------------------------------------------------------------------
; test 5: SEXW (sign-extend W into D)
; ---------------------------------------------------------------------
            lda   #5
            sta   RESULT
            ldw   #$8000            ; bit 15 set -> negative
            sexw                    ; D := $ffff
            cmpd  #$ffff
            lbne  fail
            ldw   #$0001            ; bit 15 clear -> positive
            sexw                    ; D := $0000
            cmpd  #$0000
            lbne  fail

; ---------------------------------------------------------------------
; test 6: 16-bit inherent ops on D and W (incl. the carry quirks)
; ---------------------------------------------------------------------
            lda   #6
            sta   RESULT
            ldd   #$0f0f
            comd                    ; COMD ($1043): D=$f0f0, C=1
            lbcc  fail              ; COM must set carry
            cmpd  #$f0f0
            lbne  fail
            ldw   #0
            decw                    ; DECW ($105a): W=$ffff, C set (16-bit dec)
            lbcc  fail              ; 16-bit DEC sets carry
            tfr   w,d
            cmpd  #$ffff
            lbne  fail
            ldd   #$0001
            negd                    ; NEGD ($1040): D=$ffff
            cmpd  #$ffff
            lbne  fail
            ldw   #$0002
            lsrw                    ; LSRW ($1054): W=$0001
            tfr   w,d
            cmpd  #$0001
            lbne  fail
            ldd   #$4000
            asld                    ; ASLD/LSLD ($1048): D=$8000
            cmpd  #$8000
            lbne  fail
            ldw   #$1234
            tstw                    ; TSTW ($105d): N=0, Z=0
            lbeq  fail
            clrw                    ; CLRW ($105f): W=0, Z=1
            lbne  fail
            tfr   w,d
            cmpd  #$0000
            lbne  fail

; ---------------------------------------------------------------------
; test 7: LDQ / STQ (the 32-bit Q accumulator)
; ---------------------------------------------------------------------
            lda   #7
            sta   RESULT
            ldq   #$12345678        ; A=$12 B=$34 E=$56 F=$78
            cmpd  #$1234            ; D = A:B
            lbne  fail
            tfr   w,d               ; W = E:F
            cmpd  #$5678
            lbne  fail
            ldq   #$12345678        ; reload (D was clobbered above)
            stq   <$40              ; STQ direct: [$40..$43]
            ldq   #0                ; wipe Q
            ldq   <$40              ; LDQ direct
            cmpd  #$1234
            lbne  fail
            tfr   w,d
            cmpd  #$5678
            lbne  fail

; ---------------------------------------------------------------------
; test 8: W-register stack push/pull
; ---------------------------------------------------------------------
            lda   #8
            sta   RESULT
            lds   #STACK_TOP        ; sane stack
            ldw   #$cafe
            pshsw                   ; push W
            ldw   #0                ; wipe W
            pulsw                   ; pull W
            tfr   w,d
            cmpd  #$cafe
            lbne  fail

; ---------------------------------------------------------------------
; test 9: register-register ops (8-bit, 16-bit, and 8->16 promotion)
; ---------------------------------------------------------------------
            lda   #9
            sta   RESULT
            lda   #$10
            ldb   #$05
            addr  b,a               ; A = A + B = $15  (8-bit)
            cmpa  #$15
            lbne  fail
            lda   #$20
            ldb   #$03
            subr  b,a               ; A = A - B = $1d
            cmpa  #$1d
            lbne  fail
            lda   #$f0
            ldb   #$3c
            andr  b,a               ; A = $f0 & $3c = $30
            cmpa  #$30
            lbne  fail
            ldx   #$1000
            ldy   #$0234
            addr  x,y               ; Y = Y + X = $1234  (16-bit)
            cmpy  #$1234
            lbne  fail
            ldx   #$5555
            ldy   #$5555
            cmpr  x,y               ; equal -> Z set
            lbne  fail
            ldd   #$0102            ; promotion: A widens to D
            ldx   #$0010
            addr  a,x               ; X = X + D = $0010 + $0102 = $0112
            cmpx  #$0112
            lbne  fail

; ---------------------------------------------------------------------
; test 10: bit manipulation (LDBT / BAND / STBT) on a known byte $A5
; ---------------------------------------------------------------------
            lda   #10
            sta   RESULT
            lda   #$a5              ; %10100101
            sta   <$40
            clra
            ldbt  a,0,7,$40         ; A.bit7 = mem.bit0(=1) -> A=$80
            cmpa  #$80
            lbne  fail
            lda   #$ff
            ldbt  a,1,0,$40         ; A.bit0 = mem.bit1(=0) -> A=$fe
            cmpa  #$fe
            lbne  fail
            lda   #$ff
            band  a,2,3,$40         ; A.bit3 = 1 AND mem.bit2(=1) -> $ff
            cmpa  #$ff
            lbne  fail
            lda   #$ff
            band  a,4,3,$40         ; A.bit3 = 1 AND mem.bit4(=0) -> $f7
            cmpa  #$f7
            lbne  fail
            clr   <$41
            lda   #$01
            stbt  a,0,5,$41         ; mem($41).bit5 = A.bit0(=1) -> $20
            lda   <$41
            cmpa  #$20
            lbne  fail

; ---------------------------------------------------------------------
; test 11: MULD / DIVD / DIVQ (signed multiply and divide)
; ---------------------------------------------------------------------
            lda   #11
            sta   RESULT
            ldd   #100
            muld  #3                ; Q = 300 = $0000012C : D=$0000 W=$012C
            cmpd  #0
            lbne  fail
            tfr   w,d
            cmpd  #300
            lbne  fail
            ldd   #100
            divd  #7                ; 100/7 = 14 rem 2 : B=14 A=2
            cmpb  #14
            lbne  fail
            cmpa  #2
            lbne  fail
            ldq   #$000f4240        ; 1000000
            divq  #1000             ; W=1000 ($03E8) D=0 rem
            tfr   w,d
            cmpd  #1000
            lbne  fail

; ---------------------------------------------------------------------
; test 12: MD register (LDMD/BITMD) and TFM block transfer
; ---------------------------------------------------------------------
            lda   #12
            sta   RESULT
            ldmd  #$01              ; MD = native mode
            bitmd #$01              ; MD & $01 = $01 -> Z=0
            lbeq  fail
            bitmd #$02              ; MD & $02 = 0 -> Z=1
            lbne  fail
            ldmd  #$00              ; back to emulation mode
            ldd   #$1122
            std   <$50
            ldd   #$3344
            std   <$52              ; source: $50..$53 = 11 22 33 44
            ldx   #$0050
            ldy   #$0060
            ldw   #4
            tfm   x+,y+             ; copy 4 bytes $50->$60, W down to 0
            ldd   <$60
            cmpd  #$1122
            lbne  fail
            ldd   <$62
            cmpd  #$3344
            lbne  fail
            tfr   w,d               ; W must be 0
            cmpd  #0
            lbne  fail
            cmpx  #$0054            ; X advanced by 4
            lbne  fail
            cmpy  #$0064            ; Y advanced by 4
            lbne  fail

; ---------------------------------------------------------------------
; test 13: divide-by-zero trap (vector $fff0, MD bit 7)
; ---------------------------------------------------------------------
            lda   #13
            sta   RESULT
            ldx   #dz_handler
            stx   $fff0             ; install the trap vector (RAM here)
            lds   #STACK_TOP        ; fresh stack (the trap stacks state)
            ldd   #100
            divd  #0                ; divide by zero -> trap
            lbra  fail              ; must not fall through
dz_handler
            bitmd #$80              ; divide-by-zero status bit must be set
            lbeq  fail

; ---------------------------------------------------------------------
; test 14: illegal-instruction trap (vector $fff0, MD bit 6)
; ---------------------------------------------------------------------
            lda   #14
            sta   RESULT
            ldx   #ill_handler
            stx   $fff0
            lds   #STACK_TOP
            ldmd  #0                ; clear MD (incl. the div0 bit from test 13)
            fcb   $10,$01           ; illegal opcode $1001 -> trap
            lbra  fail
ill_handler
            bitmd #$40              ; illegal-instruction status bit must be set
            lbeq  fail

; ---------------------------------------------------------------------
; test 15: page-2 16-bit memory ALU on D and W
; ---------------------------------------------------------------------
            lda   #15
            sta   RESULT
            ldd   #$ff0f
            andd  #$0ff0            ; D = $0f00
            cmpd  #$0f00
            lbne  fail
            ord   #$00f0            ; D = $0ff0
            cmpd  #$0ff0
            lbne  fail
            ldd   #$ffff
            eord  #$0f0f            ; D = $f0f0
            cmpd  #$f0f0
            lbne  fail
            ldw   #$1000
            addw  #$0234            ; W = $1234
            tfr   w,d
            cmpd  #$1234
            lbne  fail
            ldw   #$1234
            subw  #$0234            ; W = $1000
            tfr   w,d
            cmpd  #$1000
            lbne  fail
            ldd   #$1000
            andcc #$fe              ; clear carry
            adcd  #$0234            ; D = $1234
            cmpd  #$1234
            lbne  fail
            ldd   #$1234
            andcc #$fe
            sbcd  #$0234            ; D = $1000
            cmpd  #$1000
            lbne  fail
            ldw   #$abcd
            cmpw  #$abcd            ; equal -> Z
            lbne  fail
            ldd   #$ff00
            bitd  #$0f00            ; $ff00 & $0f00 = $0f00, Z=0, D unchanged
            lbeq  fail
            cmpd  #$ff00            ; BITD must not alter D
            lbne  fail

; ---------------------------------------------------------------------
; test 16: E/F register ops (memory ALU + inherent)
; ---------------------------------------------------------------------
            lda   #16
            sta   RESULT
            lde   #$3c
            tfr   e,a
            cmpa  #$3c
            lbne  fail
            adde  #$04              ; E = $40
            sube  #$10              ; E = $30
            tfr   e,a
            cmpa  #$30
            lbne  fail
            ste   <$44              ; store E to $44
            lda   <$44
            cmpa  #$30
            lbne  fail
            ldf   #$7f
            incf                    ; F = $80
            tfr   f,b
            cmpb  #$80
            lbne  fail
            clre                    ; E = 0
            tfr   e,a
            cmpa  #$00
            lbne  fail
            ldf   #$ff
            comf                    ; F = $00
            tfr   f,b
            cmpb  #$00
            lbne  fail

; ---------------------------------------------------------------------
; test 17: OIM / AIM / EIM / TIM (memory and immediate mask)
; ---------------------------------------------------------------------
            lda   #17
            sta   RESULT
            lda   #$0f
            sta   <$45              ; mem($45) = $0f
            oim   #$f0,$45          ; |= $f0 -> $ff
            lda   <$45
            cmpa  #$ff
            lbne  fail
            aim   #$0f,$45          ; &= $0f -> $0f
            lda   <$45
            cmpa  #$0f
            lbne  fail
            eim   #$ff,$45          ; ^= $ff -> $f0
            lda   <$45
            cmpa  #$f0
            lbne  fail
            tim   #$0f,$45          ; $f0 & $0f = 0 -> Z set (no write)
            lbne  fail
            lda   <$45              ; TIM must not have changed memory
            cmpa  #$f0
            lbne  fail
            tim   #$f0,$45          ; $f0 & $f0 != 0 -> Z clear
            lbeq  fail

; ---------------------------------------------------------------------
; test 18: carry-chain / flag boundaries on the 16-bit + 32-bit ops
; ---------------------------------------------------------------------
            lda   #18
            sta   RESULT
            ldw   #$ffff
            addw  #$0001            ; -> $0000, C=1
            lbcc  fail
            tfr   w,d
            cmpd  #$0000
            lbne  fail
            ldw   #$00ff
            addw  #$0001            ; byte-boundary carry -> $0100
            tfr   w,d
            cmpd  #$0100
            lbne  fail
            ldw   #$0000
            subw  #$0001            ; -> $ffff, C=1 (borrow)
            lbcc  fail
            tfr   w,d
            cmpd  #$ffff
            lbne  fail
            ldw   #$0100
            cmpw  #$00ff            ; W>M -> C=0
            lbcs  fail
            ldw   #$00ff
            cmpw  #$0100            ; W<M -> C=1
            lbcc  fail
            ldd   #$00ff
            andcc #$fe              ; clear carry
            adcd  #$0001            ; -> $0100
            cmpd  #$0100
            lbne  fail
            ldd   #$00ff
            orcc  #$01              ; set carry
            adcd  #$0000            ; +carry -> $0100
            cmpd  #$0100
            lbne  fail
            ldd   #$0100
            orcc  #$01              ; set carry (borrow-in)
            sbcd  #$0000            ; -> $00ff
            cmpd  #$00ff
            lbne  fail
            ldd   #$1000
            muld  #$0010            ; $00010000 -> D=$0001 W=$0000
            cmpd  #$0001
            lbne  fail
            tfr   w,d
            cmpd  #$0000
            lbne  fail
            ldq   #$89abcdef
            stq   <$48              ; 32-bit round-trip
            ldq   #0
            ldq   <$48
            cmpd  #$89ab
            lbne  fail
            tfr   w,d
            cmpd  #$cdef
            lbne  fail
            ldx   #$0100
            ldy   #$00ff
            cmpr  y,x               ; x - y > 0 -> C=0
            lbcs  fail

; ---------------------------------------------------------------------
; test 19: HD6309 indexed addressing (W-base + E/F/W accumulator offsets)
; ---------------------------------------------------------------------
            lda   #19
            sta   RESULT
            ldw   #$0070
            lda   #$5a
            sta   ,w                ; [$0070] = $5a   (,W)
            clra
            lda   ,w                ; A = [$0070]
            cmpa  #$5a
            lbne  fail
            lda   #$3c
            sta   2,w               ; [$0072] = $3c   (n,W, 16-bit offset)
            clra
            lda   2,w
            cmpa  #$3c
            lbne  fail
            ldw   #$0080
            lda   #$99
            sta   ,w++              ; [$0080]=$99, W->$0082   (,W++)
            tfr   w,d
            cmpd  #$0082
            lbne  fail
            ldw   #$0082
            lda   #$77
            sta   ,--w              ; W->$0080, [$0080]=$77   (,--W)
            tfr   w,d
            cmpd  #$0080
            lbne  fail
            lda   ,w
            cmpa  #$77
            lbne  fail
            ldx   #$0060
            ldw   #$0010
            lda   #$42
            sta   w,x               ; [$0060+$0010]=[$0070]=$42  (W,R)
            lda   $0070
            cmpa  #$42
            lbne  fail
            ldx   #$0060
            ldw   #$0102            ; E=$01, F=$02
            lda   #$88
            sta   e,x               ; [$0061]=$88   (E,R)
            lda   $0061
            cmpa  #$88
            lbne  fail
            lda   #$aa
            sta   f,x               ; [$0062]=$aa   (F,R)
            lda   $0062
            cmpa  #$aa
            lbne  fail

; ---------------------------------------------------------------------
; all tests passed
; ---------------------------------------------------------------------
pass
            lda   #$ff
            sta   RESULT            ; record overall pass
pass_loop
            bra   pass_loop         ; self-trap; harness halts here

fail
            ; RESULT already holds the number of the failing test
fail_loop
            bra   fail_loop         ; self-trap; harness halts here

            org   $fffe
            fdb   start             ; reset vector

            end
