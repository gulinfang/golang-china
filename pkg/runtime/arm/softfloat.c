// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"

void	abort(void);

static void
fabort(void)
{
	if (1) {
		printf("Unsupported floating point instruction\n");
		abort();
	}
}

static uint32 doabort = 0;
static uint32 trace = 0;

#define DOUBLE_EXPBIAS 1023
#define SINGLE_EXPBIAS 127

static const int8* opnames[] = {
	// binary
	"adf",
	"muf",
	"suf",
	"rsf",
	"dvf",
	"rdf",
	"pow",
	"rpw",
	"rmf",
	"fml",
	"fdv",
	"frd",
	"pol",
	"UNDEFINED",
	"UNDEFINED",
	"UNDEFINED",

	// unary
	"mvf",
	"mnf",
	"abs",
	"rnd",
	"sqt",
	"log",
	"lgn",
	"exp",
	"sin",
	"cos",
	"tan",
	"asn",
	"acs",
	"atn",
	"urd",
	"nrm"
};

static const int8* fpconst[] = {
	"0.0", "1.0", "2.0", "3.0", "4.0", "5.0", "0.5", "10.0",
};

static const uint64 fpdconst[] = {
	0x0000000000000000ll,
	0x3ff0000000000000ll,
	0x4000000000000000ll,
	0x4008000000000000ll,
	0x4010000000000000ll,
	0x4014000000000000ll,
	0x3fe0000000000000ll,
	0x4024000000000000ll
};

static const int8* fpprec[] = {
	"s", "d", "e", "?"
};

static uint32
precision(uint32 i)
{
	switch (i&0x00080080) {
	case 0:
		return 0;
	case 0x80:
		return 1;
	default:
		fabort();
	}
	return 0;
}

static uint64
frhs(uint32 rhs)
{
	if (rhs & 0x8) {
		return  fpdconst[rhs&0x7];
	} else {
		return m->freg[rhs&0x7];
	}
}

static int32
fexp(uint64 f)
{
	return (int32)((uint32)(f >> 52) & 0x7ff) - DOUBLE_EXPBIAS;
}

static uint32
fsign(uint64 f)
{
	return (uint32)(f >> 63) & 0x1;
}

static uint64
fmantissa(uint64 f)
{
	return f &0x000fffffffffffffll;
}

static void
fprint()
{
	uint32 i;
	for (i = 0; i < 8; i++) {
		printf("\tf%d:\t%X\n", i, m->freg[i]);
	}
}

static uint32
d2s(uint64 d)
{
	return (d>>32 & 0x80000000) |	//sign
		((uint32)(fexp(d) + SINGLE_EXPBIAS) & 0xff) << 23 |	// exponent
		(d >> 29 & 0x7fffff);	// mantissa
}

static uint64
s2d(uint32 s)
{
	return (uint64)(s & 0x80000000) << 63 |	// sign
		(uint64)((s >> 23 &0xff) + (DOUBLE_EXPBIAS - SINGLE_EXPBIAS)) << 52  |	// exponent
		(uint64)(s & 0x7fffff) << 29;	// mantissa
}

// cdp, data processing instructions
static void
dataprocess(uint32* pc)
{
	uint32 i, opcode, unary, dest, lhs, rhs, prec;
	uint32 high;
	uint64 fraw0, fraw1, exp, sign;
	uint64 fd, f0, f1;	

	i = *pc;

	// data processing
	opcode = i>>20 & 15;
	unary = i>>15 & 1;

	dest = i>>12 & 7;		
	lhs = i>>16 & 7;
	rhs = i & 15;

	prec = precision(i);
	if (unary) {
		switch (opcode) {
		case 0: // mvf
			m->freg[dest] = frhs(rhs);
			goto ret;
		default:
			goto undef;
		}
	} else {
		switch (opcode) {
		case 1: // muf
			fraw0 = m->freg[lhs];
			fraw1 = frhs(rhs);
			f0 = fraw0>>21 & 0xffffffff | 0x80000000;
			f1 = fraw1>>21 & 0xffffffff | 0x80000000;
			fd = f0*f1;
			high = fd >> 63;
			if (high)
				fd = fd >> 11 & 0x000fffffffffffffll;
			else
				fd = fd >> 10 & 0x000fffffffffffffll;
			exp = (uint64)(fexp(fraw0) + fexp(fraw1) + !!high + DOUBLE_EXPBIAS) & 0x7ff;
			sign = fraw0 >> 63 ^ fraw1 >> 63;
			fd = sign << 63 | exp <<52 | fd;
			m->freg[dest] = fd;
			goto ret;
		default:
			goto undef;
		}
	}

undef:
	doabort = 1;

ret:
	if (trace || doabort) {
		printf(" %p %x\t%s%s\tf%d, ", pc, *pc, opnames[opcode | unary<<4],
			fpprec[prec], dest);
		if (!unary)
			printf("f%d, ", lhs);
		if (rhs & 0x8)
			printf("#%s\n", fpconst[rhs&0x7]);
		else
			printf("f%d\n", rhs&0x7);
	}
	if (doabort)
		fabort();
}

#define CPSR 14
#define FLAGS_N (1 << 31)
#define FLAGS_Z (1 << 30)
#define FLAGS_C (1 << 29)

// cmf, compare floating point
static void
compare(uint32 *pc, uint32 *regs) {
	uint32 i, flags, lhs, rhs, sign0, sign1;
	uint32 f0, f1, mant0, mant1;
	int32 exp0, exp1;

	i = *pc;
	flags = 0;
	lhs = i>>16 & 0x7;
	rhs = i & 0xf;

	f0 = m->freg[lhs];
	f1 = frhs(rhs);
	if (f0 == f1) {
		flags = FLAGS_Z | FLAGS_C;
		goto ret;
	}

	sign0 = fsign(f0);
	sign1 = fsign(f1);
	if (sign0 == 1 && sign1 == 0) {
		flags = FLAGS_N;
		goto ret;
	}
	if (sign0 == 0 && sign1 == 1) {
		flags = FLAGS_C;
		goto ret;
	}

	if (sign0 == 0) {
		exp0 = fexp(f0);
		exp1 = fexp(f1);
		mant0 = fmantissa(f0);
		mant1 = fmantissa(f1);
	} else {
		exp0 = fexp(f1);
		exp1 = fexp(f0);
		mant0 = fmantissa(f1);
		mant1 = fmantissa(f0);
	}

	if (exp0 > exp1) {
		flags = FLAGS_C;
	} else if (exp0 < exp1) {
		flags = FLAGS_N;
	} else {
		if (mant0 > mant1)
			flags = FLAGS_C;
		else
			flags = FLAGS_N;
	}

ret:
	if (trace) {
		printf(" %p %x\tcmf\tf%d, ", pc, *pc, lhs);
		if (rhs & 0x8)
			printf("#%s\n", fpconst[rhs&0x7]);
		else
			printf("f%d\n", rhs&0x7);
	}
	regs[CPSR] = regs[CPSR] & 0x0fffffff | flags;
}

// ldf/stf, load/store floating
static void
loadstore(uint32 *pc, uint32 *regs)
{
	uint32 i, isload, coproc, ud, wb, tlen, p, reg, freg, offset;
	uint32 addr;

	i = *pc;
	coproc = i>>8&0xf;
	isload = i>>20&1;
	p = i>>24&1;
	ud = i>>23&1;
	tlen = i>>(22 - 1)&1 | i>>15&1;
	wb = i>>21&1;
	reg = i>>16 &0xf;
	freg = i>>12 &0x7;
	offset = (i&0xff) << 2;
	
	if (coproc != 1 || p != 1 || wb != 0 || tlen > 1)
		goto undef;
	if (reg > 13)
		goto undef;

	if (ud)
		addr = regs[reg] + offset;
	else
		addr = regs[reg] - offset;

	if (isload)
		if (tlen)
			m->freg[freg] = *((uint64*)addr);
		else
			m->freg[freg] = s2d(*((uint32*)addr));
	else
		if (tlen)
			*((uint64*)addr) = m->freg[freg];
		else
			*((uint32*)addr) = d2s(m->freg[freg]);
	goto ret;

undef:
	doabort = 1;

ret:
	if (trace || doabort) {
		if (isload)
			printf(" %p %x\tldf", pc, *pc);
		else
			printf(" %p %x\tstf", pc, *pc);
		printf("%s\t\tf%d, %s%d(r%d)", fpprec[tlen], freg, ud ? "" : "-", offset, reg);
		printf("\t\t// %p", regs[reg] + (ud ? offset : -offset));
		if (coproc != 1 || p != 1 || wb != 0)
			printf(" coproc: %d pre: %d wb %d", coproc, p, wb);
		printf("\n");
		fprint();
	}
	if (doabort)
		fabort();
}

static void
loadconst(uint32 *pc, uint32 *regs)
{
	uint32 offset;
	uint32 *addr;

	if (*pc & 0xfffff000 != 0xe59fb838 ||
		*(pc+1) != 0xe08bb00c ||
		*(pc+2) & 0xffff8fff != 0xed9b0100)
		goto undef;

	offset = *pc & 0xfff;
	addr = (uint32*)((uint8*)pc + offset + 8);
//printf("DEBUG: addr %p *addr %x final %p\n", addr, *addr, *addr + regs[12]);
	regs[11] = *addr + regs[12];
	loadstore(pc + 2, regs);
	goto ret;

undef:
	doabort = 1;

ret:
	if (trace || doabort) {
		printf(" %p coproc const %x %x %x\n", pc, *pc, *(pc+1), *(pc+2));
	}
	if (doabort)
		fabort();
}


// returns number of words that the fp instruction is occupying, 0 if next instruction isn't float.
// TODO(kaib): insert sanity checks for coproc 1
static uint32
stepflt(uint32 *pc, uint32 *regs)
{
	uint32 i, c;

	i = *pc;
	c = i >> 25 & 7;

	switch(c) {
	case 6: // 110
		loadstore(pc, regs);
		return 1;
	case 7: // 111
		if (i>>24 & 1) return 0; // ignore swi

		if (i>>4 & 1) { //data transfer
			if ((i&0x00f0ff00) != 0x0090f100) {
				printf(" %p %x\n", pc, i);
				fabort();
			}
			compare(pc, regs);
		} else {
			dataprocess(pc);
		}
		return 1;
	}

	// lookahead for virtual instructions that span multiple arm instructions
	c = ((*pc & 0x0f000000) >> 16) |
		((*(pc + 1)  & 0x0f000000) >> 20) |
		((*(pc + 2) & 0x0f000000) >> 24);
	if(c == 0x50d) { // 0101 0000 1101
		loadconst(pc, regs);
		return 3;
	}

	return 0;
}

#pragma textflag 7
uint32*
_sfloat2(uint32 *lr, uint32 r0)
{
	uint32 skip;
	uint32 cpsr;

	while(skip = stepflt(lr, &r0)) {
		lr += skip;
	}
	return lr;
}

