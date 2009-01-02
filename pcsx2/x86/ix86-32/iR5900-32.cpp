/*  Pcsx2 - Pc Ps2 Emulator
 *  Copyright (C) 2002-2008  Pcsx2 Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "PrecompiledHeader.h"

#include "Common.h"
#include "Memory.h"
#include "InterTables.h"
#include "ix86/ix86.h"
#include "iR5900.h"
#include "iR5900AritImm.h"
#include "iR5900Arit.h"
#include "iR5900MultDiv.h"
#include "iR5900Shift.h"
#include "iR5900Branch.h"
#include "iR5900Jump.h"
#include "iR5900LoadStore.h"
#include "iR5900Move.h"
#include "iMMI.h"
#include "iFPU.h"
#include "iCP0.h"
#include "iVUmicro.h"
#include "VU.h"
#include "VUmicro.h"

#include "iVUzerorec.h"

#include "vtlb.h"
#include "SamplProf.h"

#ifdef _WIN32
#pragma warning(disable:4244)
#pragma warning(disable:4761)
#endif

u32 maxrecmem = 0;
uptr *recLUT = NULL;

#define X86
#define RECSTACK_SIZE 0x00010000

#define EE_NUMBLOCKS (1<<15)

static u8 *recMem = NULL;			// the recompiled blocks will be here
static u8* recStack = NULL;			// stack mem
static BASEBLOCK *recRAM = NULL;		// and the ptr to the blocks here
static BASEBLOCK *recROM = NULL;		// and here
static BASEBLOCK *recROM1 = NULL;		// also here
static BASEBLOCKEX *recBlocks = NULL;
static u8* recPtr = NULL, *recStackPtr = NULL;
static EEINST* s_pInstCache = NULL;
static u32 s_nInstCacheSize = 0;

// used to disable register freezing during cpuBranchTests (registers
// are safe then since they've been completely flushed)
bool g_EEFreezeRegs = false;

static BASEBLOCK* s_pCurBlock = NULL;
static BASEBLOCKEX* s_pCurBlockEx = NULL;
static BASEBLOCK* s_pDispatchBlock = NULL;
static u32 s_nEndBlock = 0; // what pc the current block ends	
static u32 s_nHasDelay = 0;

static u32 s_nNextBlock = 0; // next free block in recBlocks

// save states for branches
static u16 s_savex86FpuState, s_saveiCWstate;
static GPR_reg64 s_ConstGPRreg;
static u32 s_saveConstGPRreg = 0, s_saveHasConstReg = 0, s_saveFlushedConstReg = 0, s_saveRegHasLive1 = 0, s_saveRegHasSignExt = 0;
static EEINST* s_psaveInstInfo = NULL;

u32 s_nBlockCycles = 0; // cycles of current block recompiling

static u32 s_savenBlockCycles = 0;

void recCOP2RecompileInst();
int recCOP2AnalyzeBlock(u32 startpc, u32 endpc);
void recCOP2EndBlock(void);
u8* dyna_block_discard_recmem=0;

#ifdef _DEBUG
u32 dumplog = 0;
#else
#define dumplog 0
#endif

u32 pc;			         // recompiler pc
int branch;		         // set for branch

//#ifdef PCSX2_DEVBUILD
LARGE_INTEGER lbase = {0}, lfinal = {0};
//static u32 s_startcount = 0;
//#endif

static const char *txt0 = "EAX = %x : ECX = %x : EDX = %x\n";
static const char *txt0RC = "EAX = %x : EBX = %x : ECX = %x : EDX = %x : ESI = %x : EDI = %x\n";
static const char *txt1 = "REG[%d] = %x_%x\n";
static const char *txt2 = "M32 = %x\n";

static void iBranchTest(u32 newpc, u32 cpuBranch);
void recCOP22( void );
#ifdef __LINUX__
extern "C" {
#endif
void recRecompile( u32 startpc );
#ifdef __LINUX__
}
#endif
BASEBLOCKEX* PC_GETBLOCKEX(BASEBLOCK* p)
{
//	BASEBLOCKEX* pex = *(BASEBLOCKEX**)(p+1);
//	if( pex >= recBlocks && pex < recBlocks+EE_NUMBLOCKS )
//		return pex;

	// otherwise, use the sorted list
	return GetBaseBlockEx(p->startpc, 0);
}

////////////////////////////////////////////////////
static void iDumpBlock( int startpc, u8 * ptr )
{
	FILE *f;
	char filename[ g_MaxPath ];
	u32 i, j;
	EEINST* pcur;
	extern const char *disRNameGPR[];
	u8 used[34];
	u8 fpuused[33];
	int numused, count, fpunumused;

	SysPrintf( "dump1 %x:%x, %x\n", startpc, pc, cpuRegs.cycle );
#ifdef _WIN32
	CreateDirectory("dumps", NULL);
	sprintf_s( filename, g_MaxPath, "dumps\\dump%.8X.txt", startpc);
#else
	mkdir("dumps", 0755);
	sprintf( filename, "dumps/dump%.8X.txt", startpc);
#endif

	fflush( stdout );
//	f = fopen( "dump1", "wb" );
//	fwrite( ptr, 1, (u32)x86Ptr - (u32)ptr, f );
//	fclose( f );
//
//	sprintf( command, "objdump -D --target=binary --architecture=i386 dump1 > %s", filename );
//	system( command );

	f = fopen( filename, "w" );

	std::string output;

    if( disR5900GetSym(startpc) != NULL )
        fprintf(f, "%s\n", disR5900GetSym(startpc));
	for ( i = startpc; i < s_nEndBlock; i += 4 ) {
		disR5900Fasm( output, PSMu32( i ), i );
		fprintf( f, output.c_str() );
	}

	// write the instruction info

	fprintf(f, "\n\nlive0 - %x, live1 - %x, live2 - %x, lastuse - %x\nmmx - %x, xmm - %x, used - %x\n",
		EEINST_LIVE0, EEINST_LIVE1, EEINST_LIVE2, EEINST_LASTUSE, EEINST_MMX, EEINST_XMM, EEINST_USED);

	memset(used, 0, sizeof(used));
	numused = 0;
	for(i = 0; i < ARRAYSIZE(s_pInstCache->regs); ++i) {
		if( s_pInstCache->regs[i] & EEINST_USED ) {
			used[i] = 1;
			numused++;
		}
	}

	memset(fpuused, 0, sizeof(fpuused));
	fpunumused = 0;
	for(i = 0; i < ARRAYSIZE(s_pInstCache->fpuregs); ++i) {
		if( s_pInstCache->fpuregs[i] & EEINST_USED ) {
			fpuused[i] = 1;
			fpunumused++;
		}
	}

	fprintf(f, "       ");
	for(i = 0; i < ARRAYSIZE(s_pInstCache->regs); ++i) {
		if( used[i] ) fprintf(f, "%2d ", i);
	}
	for(i = 0; i < ARRAYSIZE(s_pInstCache->fpuregs); ++i) {
		if( fpuused[i] ) fprintf(f, "%2d ", i);
	}
	fprintf(f, "\n");

	fprintf(f, "       ");
	for(i = 0; i < ARRAYSIZE(s_pInstCache->regs); ++i) {
		if( used[i] ) fprintf(f, "%s ", disRNameGPR[i]);
	}
	for(i = 0; i < ARRAYSIZE(s_pInstCache->fpuregs); ++i) {
		if( fpuused[i] ) fprintf(f, "%s ", i<32?"FR":"FA");
	}
	fprintf(f, "\n");

	pcur = s_pInstCache+1;
	for( i = 0; i < (s_nEndBlock-startpc)/4; ++i, ++pcur) {
		fprintf(f, "%2d: %2.2x ", i+1, pcur->info);
		
		count = 1;
		for(j = 0; j < ARRAYSIZE(s_pInstCache->regs); j++) {
			if( used[j] ) {
				fprintf(f, "%2.2x%s", pcur->regs[j], ((count%8)&&count<numused)?"_":" ");
				++count;
			}
		}
		count = 1;
		for(j = 0; j < ARRAYSIZE(s_pInstCache->fpuregs); j++) {
			if( fpuused[j] ) {
				fprintf(f, "%2.2x%s", pcur->fpuregs[j], ((count%8)&&count<fpunumused)?"_":" ");
				++count;
			}
		}
		fprintf(f, "\n");
	}
	fclose( f );
}

u8 _eeLoadWritesRs(u32 tempcode)
{
	switch(tempcode>>26) {
		case 26: // ldl
		case 27: // ldr
		case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
		case 55: // LD
		case 30: // lq
			return ((tempcode>>21)&0x1f)==((tempcode>>16)&0x1f); // rs==rt
	}
	return 0;
}

u8 _eeIsLoadStoreCoIssue(u32 firstcode, u32 secondcode)
{
	switch(firstcode>>26) {
		case 34: // lwl
			return (secondcode>>26)==38;
		case 38: // lwr
			return (secondcode>>26)==34;
		case 42: // swl
			return (secondcode>>26)==46;
		case 46: // swr
			return (secondcode>>26)==42;
		case 26: // ldl
			return (secondcode>>26)==27;
		case 27: // ldr
			return (secondcode>>26)==26;
		case 44: // sdl
			return (secondcode>>26)==45;
		case 45: // sdr
			return (secondcode>>26)==44;

		case 32: case 33: case 35: case 36: case 37: case 39:
		case 55: // LD

		// stores
		case 40: case 41: case 43:
		case 63: // sd
			return (secondcode>>26)==(firstcode>>26);

		case 30: // lq
		case 31: // sq
		case 49: // lwc1
		case 57: // swc1
		case 54: // lqc2
		case 62: // sqc2
			return (secondcode>>26)==(firstcode>>26);
	}
	return 0;
}

u8 _eeIsLoadStoreCoX(u32 tempcode)
{
	switch( tempcode>>26 ) {
		case 30: case 31: case 49: case 57: case 55: case 63:
			return 1;
	}
	return 0;
}

void _eeFlushAllUnused()
{
	int i;
	for(i = 0; i < 34; ++i) {
		if( pc < s_nEndBlock ) {
			if( (g_pCurInstInfo[1].regs[i]&EEINST_USED) )
				continue;
		}
		else if( (g_pCurInstInfo[0].regs[i]&EEINST_USED) )
			continue;

		if( i < 32 && GPR_IS_CONST1(i) ) _flushConstReg(i);
		else {	
			_deleteMMXreg(MMX_GPR+i, 1);
			_deleteGPRtoXMMreg(i, 1);
		}
	}

	//TODO when used info is done for FPU and VU0
	for(i = 0; i < XMMREGS; ++i) {
		if( xmmregs[i].inuse && xmmregs[i].type != XMMTYPE_GPRREG )
			_freeXMMreg(i);
	}
}

u32* _eeGetConstReg(int reg)
{
	assert( GPR_IS_CONST1( reg ) );

	if( g_cpuFlushedConstReg & (1<<reg) )
		return &cpuRegs.GPR.r[ reg ].UL[0];

	// if written in the future, don't flush
	if( _recIsRegWritten(g_pCurInstInfo+1, (s_nEndBlock-pc)/4, XMMTYPE_GPRREG, reg) ) {
		u32* ptempmem;
		ptempmem = recAllocStackMem(8, 4);
		ptempmem[0] = g_cpuConstRegs[ reg ].UL[0];
		ptempmem[1] = g_cpuConstRegs[ reg ].UL[1];
		return ptempmem;
	}
	
	_flushConstReg(reg);
	return &cpuRegs.GPR.r[ reg ].UL[0];
}

int _flushXMMunused()
{
	int i;
	for (i=0; i<XMMREGS; i++) {
		if (!xmmregs[i].inuse || xmmregs[i].needed || !(xmmregs[i].mode&MODE_WRITE) ) continue;
		
		if (xmmregs[i].type == XMMTYPE_GPRREG ) {
			//if( !(g_pCurInstInfo->regs[xmmregs[i].reg]&EEINST_USED) ) {
			if( !_recIsRegWritten(g_pCurInstInfo+1, (s_nEndBlock-pc)/4, XMMTYPE_GPRREG, xmmregs[i].reg) ) {
				_freeXMMreg(i);
				xmmregs[i].inuse = 1;
				return 1;
			}
		}
	}

	return 0;
}

int _flushMMXunused()
{
	int i;
	for (i=0; i<MMXREGS; i++) {
		if (!mmxregs[i].inuse || mmxregs[i].needed || !(mmxregs[i].mode&MODE_WRITE) ) continue;
		
		if( MMX_ISGPR(mmxregs[i].reg) ) {
			//if( !(g_pCurInstInfo->regs[mmxregs[i].reg-MMX_GPR]&EEINST_USED) ) {
			if( !_recIsRegWritten(g_pCurInstInfo+1, (s_nEndBlock-pc)/4, XMMTYPE_GPRREG, mmxregs[i].reg-MMX_GPR) ) {
				_freeMMXreg(i);
				mmxregs[i].inuse = 1;
				return 1;
			}
		}
	}

	return 0;
}

int _flushUnusedConstReg()
{
	int i;
	for(i = 1; i < 32; ++i) {
		if( (g_cpuHasConstReg & (1<<i)) && !(g_cpuFlushedConstReg&(1<<i)) &&
			!_recIsRegWritten(g_pCurInstInfo+1, (s_nEndBlock-pc)/4, XMMTYPE_GPRREG, i) ) {

			// check if will be written in the future
			MOV32ItoM((uptr)&cpuRegs.GPR.r[i].UL[0], g_cpuConstRegs[i].UL[0]);
			MOV32ItoM((uptr)&cpuRegs.GPR.r[i].UL[1], g_cpuConstRegs[i].UL[1]);
			g_cpuFlushedConstReg |= 1<<i;
			return 1;
		}
	}

	return 0;
}

void _flushCachedRegs()
{
	_flushConstRegs();
	_flushMMXregs();
	_flushXMMregs();
}

void _flushConstReg(int reg)
{
	if( GPR_IS_CONST1( reg ) && !(g_cpuFlushedConstReg&(1<<reg)) ) {
		MOV32ItoM((int)&cpuRegs.GPR.r[reg].UL[0], g_cpuConstRegs[reg].UL[0]);
		MOV32ItoM((int)&cpuRegs.GPR.r[reg].UL[1], g_cpuConstRegs[reg].UL[1]);
		g_cpuFlushedConstReg |= (1<<reg);
	}
}

void _flushConstRegs()
{
	int i;

	// flush constants

	// ignore r0
	for(i = 1; i < 32; ++i) {
		if( g_cpuHasConstReg & (1<<i) ) {
			
			if( !(g_cpuFlushedConstReg&(1<<i)) ) {
				MOV32ItoM((uptr)&cpuRegs.GPR.r[i].UL[0], g_cpuConstRegs[i].UL[0]);
				MOV32ItoM((uptr)&cpuRegs.GPR.r[i].UL[1], g_cpuConstRegs[i].UL[1]);
				g_cpuFlushedConstReg |= 1<<i;
			}
#if defined(_DEBUG)&&0
			else {
				// make sure the const regs are the same
				u8* ptemp[3];
				CMP32ItoM((u32)&cpuRegs.GPR.r[i].UL[0], g_cpuConstRegs[i].UL[0]);
				ptemp[0] = JNE8(0);
				if( EEINST_ISLIVE1(i) ) {
					CMP32ItoM((u32)&cpuRegs.GPR.r[i].UL[1], g_cpuConstRegs[i].UL[1]);
					ptemp[1] = JNE8(0);
				}
				ptemp[2] = JMP8(0);

				x86SetJ8( ptemp[0] );
				if( EEINST_ISLIVE1(i) ) x86SetJ8( ptemp[1] );
				CALLFunc((uptr)checkconstreg);

				x86SetJ8( ptemp[2] );
			}
#else
			if( g_cpuHasConstReg == g_cpuFlushedConstReg )
				break;
#endif
		}
	}
}

u32* recAllocStackMem(int size, int align)
{
	// write to a temp loc, trick
	if( (u32)recStackPtr % align ) recStackPtr += align - ((u32)recStackPtr%align);
	recStackPtr += size;
	return (u32*)(recStackPtr-size);
}

////////////////////
// Code Templates //
////////////////////

void CHECK_SAVE_REG(int reg)
{
	if( s_saveConstGPRreg == 0xffffffff ) {
		if( GPR_IS_CONST1(reg) ) {
			s_saveConstGPRreg = reg;
			s_ConstGPRreg = g_cpuConstRegs[reg];
		}
	}
	else {
		assert( s_saveConstGPRreg == 0 || s_saveConstGPRreg == reg );
	}
}

void _eeProcessHasLive(int reg, int signext)
{
	g_cpuPrevRegHasLive1 = g_cpuRegHasLive1;
	g_cpuRegHasLive1 |= 1<<reg;

	g_cpuPrevRegHasSignExt = g_cpuRegHasSignExt;

	if( signext ) {
		EEINST_SETSIGNEXT(reg);
	}
	else {
		EEINST_RESETSIGNEXT(reg);
	}
}

void _eeOnWriteReg(int reg, int signext)
{
	CHECK_SAVE_REG(reg);
	GPR_DEL_CONST(reg);
	_eeProcessHasLive(reg, signext);
}

void _deleteEEreg(int reg, int flush)
{
	if( !reg ) return;
	if( flush && GPR_IS_CONST1(reg) ) {
		_flushConstReg(reg);
		return;
	}
	GPR_DEL_CONST(reg);
	_deleteGPRtoXMMreg(reg, flush ? 0 : 2);
	_deleteMMXreg(MMX_GPR+reg, flush ? 0 : 2);
}

// if not mmx, then xmm
int eeProcessHILO(int reg, int mode, int mmx)
{
    int usemmx = mmx && _hasFreeMMXreg();
	if( (usemmx || _hasFreeXMMreg()) || !(g_pCurInstInfo->regs[reg]&EEINST_LASTUSE) ) {
		if( usemmx ) return _allocMMXreg(-1, MMX_GPR+reg, mode);
		return _allocGPRtoXMMreg(-1, reg, mode);
	}

	return -1;
}

#define PROCESS_EE_SETMODES(mmreg) ((mmxregs[mmreg].mode&MODE_WRITE)?PROCESS_EE_MODEWRITES:0)
#define PROCESS_EE_SETMODET(mmreg) ((mmxregs[mmreg].mode&MODE_WRITE)?PROCESS_EE_MODEWRITET:0)

// ignores XMMINFO_READS, XMMINFO_READT, and XMMINFO_READD_LO from xmminfo
// core of reg caching
void eeRecompileCode0(R5900FNPTR constcode, R5900FNPTR_INFO constscode, R5900FNPTR_INFO consttcode, R5900FNPTR_INFO noconstcode, int xmminfo)
{
	int mmreg1, mmreg2, mmreg3, mmtemp, moded;

	if ( ! _Rd_ && (xmminfo&XMMINFO_WRITED) ) return;

	if( xmminfo&XMMINFO_WRITED) {
		CHECK_SAVE_REG(_Rd_);
		_eeProcessHasLive(_Rd_, 0);
		EEINST_RESETSIGNEXT(_Rd_);
	}

	if( GPR_IS_CONST2(_Rs_, _Rt_) ) {
		if( xmminfo & XMMINFO_WRITED ) {
			_deleteMMXreg(MMX_GPR+_Rd_, 2);
			_deleteGPRtoXMMreg(_Rd_, 2);
		}
		if( xmminfo&XMMINFO_WRITED ) GPR_SET_CONST(_Rd_);
		constcode();
		return;
	}

	moded = MODE_WRITE|((xmminfo&XMMINFO_READD)?MODE_READ:0);

	// test if should write mmx
	if( g_pCurInstInfo->info & EEINST_MMX ) {

		if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) _addNeededMMXreg(MMX_GPR+MMX_LO);
		if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) _addNeededMMXreg(MMX_GPR+MMX_HI);
		_addNeededMMXreg(MMX_GPR+_Rs_);
		_addNeededMMXreg(MMX_GPR+_Rt_);

		if( GPR_IS_CONST1(_Rs_) || GPR_IS_CONST1(_Rt_) ) {
			int creg = GPR_IS_CONST1(_Rs_) ? _Rs_ : _Rt_;
			int vreg = creg == _Rs_ ? _Rt_ : _Rs_;

//			if(g_pCurInstInfo->regs[vreg]&EEINST_MMX) {
//				mmreg1 = _allocMMXreg(-1, MMX_GPR+vreg, MODE_READ);
//				_addNeededMMXreg(MMX_GPR+vreg);
//			}
			mmreg1 = _allocCheckGPRtoMMX(g_pCurInstInfo, vreg, MODE_READ);

			if( mmreg1 >= 0 ) {
				int info = PROCESS_EE_MMX;
				
				if( GPR_IS_CONST1(_Rs_) ) info |= PROCESS_EE_SETMODET(mmreg1);
				else info |= PROCESS_EE_SETMODES(mmreg1);

				if( xmminfo & XMMINFO_WRITED ) {
					_addNeededMMXreg(MMX_GPR+_Rd_);
					mmreg3 = _checkMMXreg(MMX_GPR+_Rd_, moded);

					if( !(xmminfo&XMMINFO_READD) && mmreg3 < 0 && ((g_pCurInstInfo->regs[vreg] & EEINST_LASTUSE) || !EEINST_ISLIVE64(vreg)) ) {
						if( EEINST_ISLIVE64(vreg) ) {
							_freeMMXreg(mmreg1);
							if( GPR_IS_CONST1(_Rs_) ) info &= ~PROCESS_EE_MODEWRITET;
							else info &= ~PROCESS_EE_MODEWRITES;
						}
						_deleteGPRtoXMMreg(_Rd_, 2);
						mmxregs[mmreg1].inuse = 1;
						mmxregs[mmreg1].reg = _Rd_;
						mmxregs[mmreg1].mode = moded;
						mmreg3 = mmreg1;
					}
					else if( mmreg3 < 0 ) mmreg3 = _allocMMXreg(-1, MMX_GPR+_Rd_, moded);

					info |= PROCESS_EE_SET_D(mmreg3);
				}

				if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) {
					mmtemp = eeProcessHILO(MMX_LO, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 1);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_LO(mmtemp);
				}
				if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) {
					mmtemp = eeProcessHILO(MMX_HI, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 1);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_HI(mmtemp);
				}

				SetMMXstate();
				if( creg == _Rs_ ) constscode(info|PROCESS_EE_SET_T(mmreg1));
				else consttcode(info|PROCESS_EE_SET_S(mmreg1));
				_clearNeededMMXregs();
				if( xmminfo & XMMINFO_WRITED ) GPR_DEL_CONST(_Rd_);
				return;
			}
		}
		else {
			// no const regs
			mmreg1 = _allocCheckGPRtoMMX(g_pCurInstInfo, _Rs_, MODE_READ);
			mmreg2 = _allocCheckGPRtoMMX(g_pCurInstInfo, _Rt_, MODE_READ);

			if( mmreg1 >= 0 || mmreg2 >= 0 ) {
				int info = PROCESS_EE_MMX;

				// do it all in mmx
				if( mmreg1 < 0 ) mmreg1 = _allocMMXreg(-1, MMX_GPR+_Rs_, MODE_READ);
				if( mmreg2 < 0 ) mmreg2 = _allocMMXreg(-1, MMX_GPR+_Rt_, MODE_READ);

				info |= PROCESS_EE_SETMODES(mmreg1)|PROCESS_EE_SETMODET(mmreg2);

				// check for last used, if so don't alloc a new MMX reg
				if( xmminfo & XMMINFO_WRITED ) {
					_addNeededMMXreg(MMX_GPR+_Rd_);
					mmreg3 = _checkMMXreg(MMX_GPR+_Rd_, moded);

					if( mmreg3 < 0 ) {
						if( !(xmminfo&XMMINFO_READD) && ((g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE) || !EEINST_ISLIVE64(_Rt_)) ) {
							if( EEINST_ISLIVE64(_Rt_) ) {
								_freeMMXreg(mmreg2);
								info &= ~PROCESS_EE_MODEWRITET;
							}
							_deleteGPRtoXMMreg(_Rd_, 2);
							mmxregs[mmreg2].inuse = 1;
							mmxregs[mmreg2].reg = _Rd_;
							mmxregs[mmreg2].mode = moded;
							mmreg3 = mmreg2;
						}
						else if( !(xmminfo&XMMINFO_READD) && ((g_pCurInstInfo->regs[_Rs_] & EEINST_LASTUSE) || !EEINST_ISLIVE64(_Rs_)) ) {
							if( EEINST_ISLIVE64(_Rs_) ) {
								_freeMMXreg(mmreg1);
								info &= ~PROCESS_EE_MODEWRITES;
							}
							_deleteGPRtoXMMreg(_Rd_, 2);
							mmxregs[mmreg1].inuse = 1;
							mmxregs[mmreg1].reg = _Rd_;
							mmxregs[mmreg1].mode = moded;
							mmreg3 = mmreg1;
						}
						else mmreg3 = _allocMMXreg(-1, MMX_GPR+_Rd_, moded);
					}

					info |= PROCESS_EE_SET_D(mmreg3);
				}

				if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) {
					mmtemp = eeProcessHILO(MMX_LO, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 1);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_LO(mmtemp);
				}
				if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) {
					mmtemp = eeProcessHILO(MMX_HI, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 1);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_HI(mmtemp);
				}

				SetMMXstate();
				noconstcode(info|PROCESS_EE_SET_S(mmreg1)|PROCESS_EE_SET_T(mmreg2));
				_clearNeededMMXregs();
				if( xmminfo & XMMINFO_WRITED ) GPR_DEL_CONST(_Rd_);
				return;
			}
		}

		_clearNeededMMXregs();
	}

	// test if should write xmm, mirror to mmx code
	if( g_pCurInstInfo->info & EEINST_XMM ) {

		if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) _addNeededGPRtoXMMreg(XMMGPR_LO);
		if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) _addNeededGPRtoXMMreg(XMMGPR_HI);
		_addNeededGPRtoXMMreg(_Rs_);
		_addNeededGPRtoXMMreg(_Rt_);

		if( GPR_IS_CONST1(_Rs_) || GPR_IS_CONST1(_Rt_) ) {
			int creg = GPR_IS_CONST1(_Rs_) ? _Rs_ : _Rt_;
			int vreg = creg == _Rs_ ? _Rt_ : _Rs_;

//			if(g_pCurInstInfo->regs[vreg]&EEINST_XMM) {
//				mmreg1 = _allocGPRtoXMMreg(-1, vreg, MODE_READ);
//				_addNeededGPRtoXMMreg(vreg);
//			}
			mmreg1 = _allocCheckGPRtoXMM(g_pCurInstInfo, vreg, MODE_READ);

			if( mmreg1 >= 0 ) {
				int info = PROCESS_EE_XMM;

				if( GPR_IS_CONST1(_Rs_) ) info |= PROCESS_EE_SETMODET(mmreg1);
				else info |= PROCESS_EE_SETMODES(mmreg1);

				if( xmminfo & XMMINFO_WRITED ) {

					_addNeededGPRtoXMMreg(_Rd_);
					mmreg3 = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_WRITE);

					if( !(xmminfo&XMMINFO_READD) && mmreg3 < 0 && ((g_pCurInstInfo->regs[vreg] & EEINST_LASTUSE) || !EEINST_ISLIVEXMM(vreg)) ) {
						_freeXMMreg(mmreg1);
						if( GPR_IS_CONST1(_Rs_) ) info &= ~PROCESS_EE_MODEWRITET;
						else info &= ~PROCESS_EE_MODEWRITES;
						_deleteMMXreg(MMX_GPR+_Rd_, 2);
						xmmregs[mmreg1].inuse = 1;
						xmmregs[mmreg1].reg = _Rd_;
						xmmregs[mmreg1].mode = moded;
						mmreg3 = mmreg1;
					}
					else if( mmreg3 < 0 ) mmreg3 = _allocGPRtoXMMreg(-1, _Rd_, moded);

					info |= PROCESS_EE_SET_D(mmreg3);
				}

				if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) {
					mmtemp = eeProcessHILO(XMMGPR_LO, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 0);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_LO(mmtemp);
				}
				if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) {
					mmtemp = eeProcessHILO(XMMGPR_HI, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 0);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_HI(mmtemp);
				}

				if( creg == _Rs_ ) constscode(info|PROCESS_EE_SET_T(mmreg1));
				else consttcode(info|PROCESS_EE_SET_S(mmreg1));
				_clearNeededXMMregs();
				if( xmminfo & XMMINFO_WRITED ) GPR_DEL_CONST(_Rd_);
				return;
			}
		}
		else {
			// no const regs
			mmreg1 = _allocCheckGPRtoXMM(g_pCurInstInfo, _Rs_, MODE_READ);
			mmreg2 = _allocCheckGPRtoXMM(g_pCurInstInfo, _Rt_, MODE_READ);

			if( mmreg1 >= 0 || mmreg2 >= 0 ) {
				int info = PROCESS_EE_XMM;

				// do it all in xmm
				if( mmreg1 < 0 ) mmreg1 = _allocGPRtoXMMreg(-1, _Rs_, MODE_READ);
				if( mmreg2 < 0 ) mmreg2 = _allocGPRtoXMMreg(-1, _Rt_, MODE_READ);

				info |= PROCESS_EE_SETMODES(mmreg1)|PROCESS_EE_SETMODET(mmreg2);

				if( xmminfo & XMMINFO_WRITED ) {
					// check for last used, if so don't alloc a new XMM reg
					_addNeededGPRtoXMMreg(_Rd_);
					mmreg3 = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, moded);

					if( mmreg3 < 0 ) {
						if( !(xmminfo&XMMINFO_READD) && ((g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE) || !EEINST_ISLIVEXMM(_Rt_)) ) {
							_freeXMMreg(mmreg2);
							info &= ~PROCESS_EE_MODEWRITET;
							_deleteMMXreg(MMX_GPR+_Rd_, 2);
							xmmregs[mmreg2].inuse = 1;
							xmmregs[mmreg2].reg = _Rd_;
							xmmregs[mmreg2].mode = moded;
							mmreg3 = mmreg2;
						}
						else if( !(xmminfo&XMMINFO_READD) && ((g_pCurInstInfo->regs[_Rs_] & EEINST_LASTUSE) || !EEINST_ISLIVEXMM(_Rs_)) ) {
							_freeXMMreg(mmreg1);
							info &= ~PROCESS_EE_MODEWRITES;
							_deleteMMXreg(MMX_GPR+_Rd_, 2);
							xmmregs[mmreg1].inuse = 1;
							xmmregs[mmreg1].reg = _Rd_;
							xmmregs[mmreg1].mode = moded;
							mmreg3 = mmreg1;
						}
						else mmreg3 = _allocGPRtoXMMreg(-1, _Rd_, moded);
					}

					info |= PROCESS_EE_SET_D(mmreg3);
				}

				if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) {
					mmtemp = eeProcessHILO(XMMGPR_LO, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 0);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_LO(mmtemp);
				}
				if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) {
					mmtemp = eeProcessHILO(XMMGPR_HI, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0), 0);
					if( mmtemp >= 0 ) info |= PROCESS_EE_SET_HI(mmtemp);
				}

				noconstcode(info|PROCESS_EE_SET_S(mmreg1)|PROCESS_EE_SET_T(mmreg2));
				_clearNeededXMMregs();
				if( xmminfo & XMMINFO_WRITED ) GPR_DEL_CONST(_Rd_);
				return;
			}
		}

		_clearNeededXMMregs();
	}

	// regular x86
	_deleteGPRtoXMMreg(_Rs_, 1);
	_deleteGPRtoXMMreg(_Rt_, 1);
	if( xmminfo&XMMINFO_WRITED )
		_deleteGPRtoXMMreg(_Rd_, (xmminfo&XMMINFO_READD)?0:2);
	_deleteMMXreg(MMX_GPR+_Rs_, 1);
	_deleteMMXreg(MMX_GPR+_Rt_, 1);
	if( xmminfo&XMMINFO_WRITED )
		_deleteMMXreg(MMX_GPR+_Rd_, (xmminfo&XMMINFO_READD)?0:2);

	// don't delete, fn will take care of them
//	if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) {
//		_deleteGPRtoXMMreg(XMMGPR_LO, (xmminfo&XMMINFO_READLO)?1:0);
//		_deleteMMXreg(MMX_GPR+MMX_LO, (xmminfo&XMMINFO_READLO)?1:0);
//	}
//	if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) {
//		_deleteGPRtoXMMreg(XMMGPR_HI, (xmminfo&XMMINFO_READHI)?1:0);
//		_deleteMMXreg(MMX_GPR+MMX_HI, (xmminfo&XMMINFO_READHI)?1:0);
//	}

	if( GPR_IS_CONST1(_Rs_) ) {
		constscode(0);
		if( xmminfo&XMMINFO_WRITED ) GPR_DEL_CONST(_Rd_);
		return;
	}

	if( GPR_IS_CONST1(_Rt_) ) {
		consttcode(0);
		if( xmminfo&XMMINFO_WRITED ) GPR_DEL_CONST(_Rd_);
		return;
	}

	noconstcode(0);
	if( xmminfo&XMMINFO_WRITED ) GPR_DEL_CONST(_Rd_);
}

// rt = rs op imm16
void eeRecompileCode1(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode)
{
	int mmreg1, mmreg2;
	if ( ! _Rt_ ) return;

	CHECK_SAVE_REG(_Rt_);
	_eeProcessHasLive(_Rt_, 0);
	EEINST_RESETSIGNEXT(_Rt_);

	if( GPR_IS_CONST1(_Rs_) ) {
		_deleteMMXreg(MMX_GPR+_Rt_, 2);
		_deleteGPRtoXMMreg(_Rt_, 2);
		GPR_SET_CONST(_Rt_);
		constcode();
		return;
	}

	// test if should write mmx
	if( g_pCurInstInfo->info & EEINST_MMX ) {

		// no const regs
		mmreg1 = _allocCheckGPRtoMMX(g_pCurInstInfo, _Rs_, MODE_READ);

		if( mmreg1 >= 0 ) {
			int info = PROCESS_EE_MMX|PROCESS_EE_SETMODES(mmreg1);

			// check for last used, if so don't alloc a new MMX reg
			_addNeededMMXreg(MMX_GPR+_Rt_);
			mmreg2 = _checkMMXreg(MMX_GPR+_Rt_, MODE_WRITE);

			if( mmreg2 < 0 ) {
				if( (g_pCurInstInfo->regs[_Rs_] & EEINST_LASTUSE) || !EEINST_ISLIVE64(_Rs_) ) {
					if( EEINST_ISLIVE64(_Rs_) ) {
						_freeMMXreg(mmreg1);
						info &= ~PROCESS_EE_MODEWRITES;
					}
					_deleteGPRtoXMMreg(_Rt_, 2);
					mmxregs[mmreg1].inuse = 1;
					mmxregs[mmreg1].reg = _Rt_;
					mmxregs[mmreg1].mode = MODE_WRITE|MODE_READ;
					mmreg2 = mmreg1;
				}
				else mmreg2 = _allocMMXreg(-1, MMX_GPR+_Rt_, MODE_WRITE);
			}

			SetMMXstate();
			noconstcode(info|PROCESS_EE_SET_S(mmreg1)|PROCESS_EE_SET_T(mmreg2));
			_clearNeededMMXregs();
			GPR_DEL_CONST(_Rt_);
			return;
		}

		_clearNeededMMXregs();
	}

	// test if should write xmm, mirror to mmx code
	if( g_pCurInstInfo->info & EEINST_XMM ) {

		// no const regs
		mmreg1 = _allocCheckGPRtoXMM(g_pCurInstInfo, _Rs_, MODE_READ);

		if( mmreg1 >= 0 ) {
			int info = PROCESS_EE_XMM|PROCESS_EE_SETMODES(mmreg1);

			// check for last used, if so don't alloc a new XMM reg
			_addNeededGPRtoXMMreg(_Rt_);
			mmreg2 = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_WRITE);

			if( mmreg2 < 0 ) {
				if( (g_pCurInstInfo->regs[_Rs_] & EEINST_LASTUSE) || !EEINST_ISLIVEXMM(_Rs_) ) {
					_freeXMMreg(mmreg1);
					info &= ~PROCESS_EE_MODEWRITES;
					_deleteMMXreg(MMX_GPR+_Rt_, 2);
					xmmregs[mmreg1].inuse = 1;
					xmmregs[mmreg1].reg = _Rt_;
					xmmregs[mmreg1].mode = MODE_WRITE|MODE_READ;
					mmreg2 = mmreg1;
				}
				else mmreg2 = _allocGPRtoXMMreg(-1, _Rt_, MODE_WRITE);
			}

			noconstcode(info|PROCESS_EE_SET_S(mmreg1)|PROCESS_EE_SET_T(mmreg2));
			_clearNeededXMMregs();
			GPR_DEL_CONST(_Rt_);
			return;
		}

		_clearNeededXMMregs();
	}

	// regular x86
	_deleteGPRtoXMMreg(_Rs_, 1);
	_deleteGPRtoXMMreg(_Rt_, 2);
	_deleteMMXreg(MMX_GPR+_Rs_, 1);
	_deleteMMXreg(MMX_GPR+_Rt_, 2);

	noconstcode(0);
	GPR_DEL_CONST(_Rt_);
}

// rd = rt op sa
void eeRecompileCode2(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode)
{
	int mmreg1, mmreg2;
	if ( ! _Rd_ ) return;

	CHECK_SAVE_REG(_Rd_);
	_eeProcessHasLive(_Rd_, 0);
	EEINST_RESETSIGNEXT(_Rd_);

	if( GPR_IS_CONST1(_Rt_) ) {
		_deleteMMXreg(MMX_GPR+_Rd_, 2);
		_deleteGPRtoXMMreg(_Rd_, 2);
		GPR_SET_CONST(_Rd_);
		constcode();
		return;
	}

	// test if should write mmx
	if( g_pCurInstInfo->info & EEINST_MMX ) {

		// no const regs
		mmreg1 = _allocCheckGPRtoMMX(g_pCurInstInfo, _Rt_, MODE_READ);

		if( mmreg1 >= 0 ) {
			int info = PROCESS_EE_MMX|PROCESS_EE_SETMODET(mmreg1);

			// check for last used, if so don't alloc a new MMX reg
			_addNeededMMXreg(MMX_GPR+_Rd_);
			mmreg2 = _checkMMXreg(MMX_GPR+_Rd_, MODE_WRITE);

			if( mmreg2 < 0 ) {
				if( (g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE) || !EEINST_ISLIVE64(_Rt_) ) {
					if( EEINST_ISLIVE64(_Rt_) ) {
						_freeMMXreg(mmreg1);
						info &= ~PROCESS_EE_MODEWRITET;
					}
					_deleteGPRtoXMMreg(_Rd_, 2);
					mmxregs[mmreg1].inuse = 1;
					mmxregs[mmreg1].reg = _Rd_;
					mmxregs[mmreg1].mode = MODE_WRITE|MODE_READ;
					mmreg2 = mmreg1;
				}
				else mmreg2 = _allocMMXreg(-1, MMX_GPR+_Rd_, MODE_WRITE);
			}

			SetMMXstate();
			noconstcode(info|PROCESS_EE_SET_T(mmreg1)|PROCESS_EE_SET_D(mmreg2));
			_clearNeededMMXregs();
			GPR_DEL_CONST(_Rd_);
			return;
		}

		_clearNeededMMXregs();
	}

	// test if should write xmm, mirror to mmx code
	if( g_pCurInstInfo->info & EEINST_XMM ) {

		// no const regs
		mmreg1 = _allocCheckGPRtoXMM(g_pCurInstInfo, _Rt_, MODE_READ);

		if( mmreg1 >= 0 ) {
			int info = PROCESS_EE_XMM|PROCESS_EE_SETMODET(mmreg1);

			// check for last used, if so don't alloc a new XMM reg
			_addNeededGPRtoXMMreg(_Rd_);
			mmreg2 = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_WRITE);

			if( mmreg2 < 0 ) {
				if( (g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE) || !EEINST_ISLIVE64(_Rt_) ) {
					_freeXMMreg(mmreg1);
					info &= ~PROCESS_EE_MODEWRITET;
					_deleteMMXreg(MMX_GPR+_Rd_, 2);
					xmmregs[mmreg1].inuse = 1;
					xmmregs[mmreg1].reg = _Rd_;
					xmmregs[mmreg1].mode = MODE_WRITE|MODE_READ;
					mmreg2 = mmreg1;
				}
				else mmreg2 = _allocGPRtoXMMreg(-1, _Rd_, MODE_WRITE);
			}

			noconstcode(info|PROCESS_EE_SET_T(mmreg1)|PROCESS_EE_SET_D(mmreg2));
			_clearNeededXMMregs();
			GPR_DEL_CONST(_Rd_);
			return;
		}

		_clearNeededXMMregs();
	}

	// regular x86
	_deleteGPRtoXMMreg(_Rt_, 1);
	_deleteGPRtoXMMreg(_Rd_, 2);
	_deleteMMXreg(MMX_GPR+_Rt_, 1);
	_deleteMMXreg(MMX_GPR+_Rd_, 2);

	noconstcode(0);
	GPR_DEL_CONST(_Rd_);
}

// rt op rs 
void eeRecompileCode3(R5900FNPTR constcode, R5900FNPTR_INFO multicode)
{
	assert(0);
	// for now, don't support xmm
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);

	if( GPR_IS_CONST2(_Rs_, _Rt_) ) {
		constcode();
		return;
	}

	if( GPR_IS_CONST1(_Rs_) ) {
		//multicode(PROCESS_EE_CONSTT);
		return;
	}

	if( GPR_IS_CONST1(_Rt_) ) {
		//multicode(PROCESS_EE_CONSTT);
		return;
	}

	multicode(0);
}

// Simple Code Templates //

// rd = rs op rt
void eeRecompileCodeConst0(R5900FNPTR constcode, R5900FNPTR_INFO constscode, R5900FNPTR_INFO consttcode, R5900FNPTR_INFO noconstcode)
{
	if ( ! _Rd_ ) return;

	// for now, don't support xmm
	CHECK_SAVE_REG(_Rd_);

	_deleteGPRtoXMMreg(_Rs_, 1);
	_deleteGPRtoXMMreg(_Rt_, 1);
	_deleteGPRtoXMMreg(_Rd_, 0);
	_deleteMMXreg(MMX_GPR+_Rs_, 1);
	_deleteMMXreg(MMX_GPR+_Rt_, 1);
	_deleteMMXreg(MMX_GPR+_Rd_, 0);

	if( GPR_IS_CONST2(_Rs_, _Rt_) ) {
		GPR_SET_CONST(_Rd_);
		constcode();
		return;
	}

	if( GPR_IS_CONST1(_Rs_) ) {
		constscode(0);
		GPR_DEL_CONST(_Rd_);
		return;
	}

	if( GPR_IS_CONST1(_Rt_) ) {
		consttcode(0);
		GPR_DEL_CONST(_Rd_);
		return;
	}

	noconstcode(0);
	GPR_DEL_CONST(_Rd_);
}

// rt = rs op imm16
void eeRecompileCodeConst1(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode)
{
    if ( ! _Rt_ )
        return;

	// for now, don't support xmm
	CHECK_SAVE_REG(_Rt_);

	_deleteGPRtoXMMreg(_Rs_, 1);
	_deleteGPRtoXMMreg(_Rt_, 0);

	if( GPR_IS_CONST1(_Rs_) ) {
		GPR_SET_CONST(_Rt_);
		constcode();
		return;
	}

	noconstcode(0);
	GPR_DEL_CONST(_Rt_);
}

// rd = rt op sa
void eeRecompileCodeConst2(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode)
{
	if ( ! _Rd_ ) return;

	// for now, don't support xmm
	CHECK_SAVE_REG(_Rd_);

	_deleteGPRtoXMMreg(_Rt_, 1);
	_deleteGPRtoXMMreg(_Rd_, 0);

	if( GPR_IS_CONST1(_Rt_) ) {
		GPR_SET_CONST(_Rd_);
		constcode();
		return;
	}

	noconstcode(0);
	GPR_DEL_CONST(_Rd_);
}

// rd = rt MULT rs  (SPECIAL)
void eeRecompileCodeConstSPECIAL(R5900FNPTR constcode, R5900FNPTR_INFO multicode, int MULT)
{
	assert(0);
	// for now, don't support xmm
	if( MULT ) {
		CHECK_SAVE_REG(_Rd_);
		_deleteGPRtoXMMreg(_Rd_, 0);
	}

	_deleteGPRtoXMMreg(_Rs_, 1);
	_deleteGPRtoXMMreg(_Rt_, 1);

	if( GPR_IS_CONST2(_Rs_, _Rt_) ) {
		if( MULT && _Rd_ ) GPR_SET_CONST(_Rd_);
		constcode();
		return;
	}

	if( GPR_IS_CONST1(_Rs_) ) {
		//multicode(PROCESS_EE_CONSTS);
		if( MULT && _Rd_ ) GPR_DEL_CONST(_Rd_);
		return;
	}

	if( GPR_IS_CONST1(_Rt_) ) {
		//multicode(PROCESS_EE_CONSTT);
		if( MULT && _Rd_ ) GPR_DEL_CONST(_Rd_);
		return;
	}

	multicode(0);
	if( MULT && _Rd_ ) GPR_DEL_CONST(_Rd_);
}

// EE XMM allocation code
int eeRecompileCodeXMM(int xmminfo)
{
	int info = PROCESS_EE_XMM;

	// save state
	if( xmminfo & XMMINFO_WRITED ) {
		CHECK_SAVE_REG(_Rd_);
		_eeProcessHasLive(_Rd_, 0);
		EEINST_RESETSIGNEXT(_Rd_);
	}

	// flush consts
	if( xmminfo & XMMINFO_READT ) {
		if( GPR_IS_CONST1( _Rt_ ) && !(g_cpuFlushedConstReg&(1<<_Rt_)) ) {
			MOV32ItoM((int)&cpuRegs.GPR.r[ _Rt_ ].UL[ 0 ], g_cpuConstRegs[_Rt_].UL[0]);
			MOV32ItoM((int)&cpuRegs.GPR.r[ _Rt_ ].UL[ 1 ], g_cpuConstRegs[_Rt_].UL[1]);
			g_cpuFlushedConstReg |= (1<<_Rt_);
		}
	}
	if( xmminfo & XMMINFO_READS) {
		if( GPR_IS_CONST1( _Rs_ ) && !(g_cpuFlushedConstReg&(1<<_Rs_)) ) {
			MOV32ItoM((int)&cpuRegs.GPR.r[ _Rs_ ].UL[ 0 ], g_cpuConstRegs[_Rs_].UL[0]);
			MOV32ItoM((int)&cpuRegs.GPR.r[ _Rs_ ].UL[ 1 ], g_cpuConstRegs[_Rs_].UL[1]);
			g_cpuFlushedConstReg |= (1<<_Rs_);
		}
	}

	if( xmminfo & XMMINFO_WRITED ) {
		GPR_DEL_CONST(_Rd_);
	}

	// add needed
	if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) {
		_addNeededGPRtoXMMreg(XMMGPR_LO);
	}
	if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) {
		_addNeededGPRtoXMMreg(XMMGPR_HI);
	}
	if( xmminfo & XMMINFO_READS) _addNeededGPRtoXMMreg(_Rs_);
	if( xmminfo & XMMINFO_READT) _addNeededGPRtoXMMreg(_Rt_);
	if( xmminfo & XMMINFO_WRITED ) _addNeededGPRtoXMMreg(_Rd_);

	// allocate
	if( xmminfo & XMMINFO_READS) {
		int reg = _allocGPRtoXMMreg(-1, _Rs_, MODE_READ);
		info |= PROCESS_EE_SET_S(reg)|PROCESS_EE_SETMODES(reg);
	}
	if( xmminfo & XMMINFO_READT) {
		int reg = _allocGPRtoXMMreg(-1, _Rt_, MODE_READ);
		info |= PROCESS_EE_SET_T(reg)|PROCESS_EE_SETMODET(reg);
	}

	if( xmminfo & XMMINFO_WRITED ) {
		int readd = MODE_WRITE|((xmminfo&XMMINFO_READD)?((xmminfo&XMMINFO_READD_LO)?(MODE_READ|MODE_READHALF):MODE_READ):0);

		int regd = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, readd);

		if( regd < 0 ) {
			if( !(xmminfo&XMMINFO_READD) && (xmminfo & XMMINFO_READT) && (_Rt_ == 0 || (g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE) || !EEINST_ISLIVEXMM(_Rt_)) ) {
				_freeXMMreg(EEREC_T);
				_deleteMMXreg(MMX_GPR+_Rd_, 2);
				xmmregs[EEREC_T].inuse = 1;
				xmmregs[EEREC_T].reg = _Rd_;
				xmmregs[EEREC_T].mode = readd;
				regd = EEREC_T;
			}
			else if( !(xmminfo&XMMINFO_READD) && (xmminfo & XMMINFO_READS) && (_Rs_ == 0 || (g_pCurInstInfo->regs[_Rs_] & EEINST_LASTUSE) || !EEINST_ISLIVEXMM(_Rs_)) ) {
				_freeXMMreg(EEREC_S);
				_deleteMMXreg(MMX_GPR+_Rd_, 2);
				xmmregs[EEREC_S].inuse = 1;
				xmmregs[EEREC_S].reg = _Rd_;
				xmmregs[EEREC_S].mode = readd;
				regd = EEREC_S;
			}
			else regd = _allocGPRtoXMMreg(-1, _Rd_, readd);
		}

		info |= PROCESS_EE_SET_D(regd);
	}
	if( xmminfo & (XMMINFO_READLO|XMMINFO_WRITELO) ) {
		info |= PROCESS_EE_SET_LO(_allocGPRtoXMMreg(-1, XMMGPR_LO, ((xmminfo&XMMINFO_READLO)?MODE_READ:0)|((xmminfo&XMMINFO_WRITELO)?MODE_WRITE:0)));
		info |= PROCESS_EE_LO;
	}
	if( xmminfo & (XMMINFO_READHI|XMMINFO_WRITEHI) ) {
		info |= PROCESS_EE_SET_HI(_allocGPRtoXMMreg(-1, XMMGPR_HI, ((xmminfo&XMMINFO_READHI)?MODE_READ:0)|((xmminfo&XMMINFO_WRITEHI)?MODE_WRITE:0)));
		info |= PROCESS_EE_HI;
	}
	return info;
}

// EE COP1(FPU) XMM allocation code
#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

#define PROCESS_EE_SETMODES_XMM(mmreg) ((xmmregs[mmreg].mode&MODE_WRITE)?PROCESS_EE_MODEWRITES:0)
#define PROCESS_EE_SETMODET_XMM(mmreg) ((xmmregs[mmreg].mode&MODE_WRITE)?PROCESS_EE_MODEWRITET:0)

// rd = rs op rt
void eeFPURecompileCode(R5900FNPTR_INFO xmmcode, R5900FNPTR fpucode, int xmminfo)
{
	int mmregs=-1, mmregt=-1, mmregd=-1, mmregacc=-1;
	int info = PROCESS_EE_XMM;

	if( xmminfo & XMMINFO_READS ) _addNeededFPtoXMMreg(_Fs_);
	if( xmminfo & XMMINFO_READT ) _addNeededFPtoXMMreg(_Ft_);
	if( xmminfo & (XMMINFO_WRITED|XMMINFO_READD) ) _addNeededFPtoXMMreg(_Fd_);
	if( xmminfo & (XMMINFO_WRITEACC|XMMINFO_READACC) ) _addNeededFPACCtoXMMreg();

	if( xmminfo & XMMINFO_READT ) {
		if( g_pCurInstInfo->fpuregs[_Ft_] & EEINST_LASTUSE ) mmregt = _checkXMMreg(XMMTYPE_FPREG, _Ft_, MODE_READ);
		else mmregt = _allocFPtoXMMreg(-1, _Ft_, MODE_READ);
	}

	if( xmminfo & XMMINFO_READS ) {
		if( ( !(xmminfo & XMMINFO_READT) || (mmregt >= 0) ) && (g_pCurInstInfo->fpuregs[_Fs_] & EEINST_LASTUSE) ) {
			mmregs = _checkXMMreg(XMMTYPE_FPREG, _Fs_, MODE_READ);
		}
		else mmregs = _allocFPtoXMMreg(-1, _Fs_, MODE_READ);
	}

	if( mmregs >= 0 ) info |= PROCESS_EE_SETMODES_XMM(mmregs);
	if( mmregt >= 0 ) info |= PROCESS_EE_SETMODET_XMM(mmregt);

	if( xmminfo & XMMINFO_READD ) {
		assert( xmminfo & XMMINFO_WRITED );
		mmregd = _allocFPtoXMMreg(-1, _Fd_, MODE_READ);
	}

	if( xmminfo & XMMINFO_READACC ) {
		if( !(xmminfo&XMMINFO_WRITEACC) && (g_pCurInstInfo->fpuregs[_Ft_] & EEINST_LASTUSE) )
			mmregacc = _checkXMMreg(XMMTYPE_FPACC, 0, MODE_READ);
		else mmregacc = _allocFPACCtoXMMreg(-1, MODE_READ);
	}

	if( xmminfo & XMMINFO_WRITEACC ) {
			
		// check for last used, if so don't alloc a new XMM reg
		int readacc = MODE_WRITE|((xmminfo&XMMINFO_READACC)?MODE_READ:0);

		mmregacc = _checkXMMreg(XMMTYPE_FPACC, 0, readacc);

		if( mmregacc < 0 ) {
			if( (xmminfo&XMMINFO_READT) && mmregt >= 0 && (FPUINST_LASTUSE(_Ft_) || !FPUINST_ISLIVE(_Ft_)) ) {
				if( FPUINST_ISLIVE(_Ft_) ) {
					_freeXMMreg(mmregt);
					info &= ~PROCESS_EE_MODEWRITET;
				}
				_deleteMMXreg(MMX_FPU+XMMFPU_ACC, 2);
				xmmregs[mmregt].inuse = 1;
				xmmregs[mmregt].reg = 0;
				xmmregs[mmregt].mode = readacc;
				xmmregs[mmregt].type = XMMTYPE_FPACC;
				mmregacc = mmregt;
			}
			else if( (xmminfo&XMMINFO_READS) && mmregs >= 0 && (FPUINST_LASTUSE(_Fs_) || !FPUINST_ISLIVE(_Fs_)) ) {
				if( FPUINST_ISLIVE(_Fs_) ) {
					_freeXMMreg(mmregs);
					info &= ~PROCESS_EE_MODEWRITES;
				}
				_deleteMMXreg(MMX_FPU+XMMFPU_ACC, 2);
				xmmregs[mmregs].inuse = 1;
				xmmregs[mmregs].reg = 0;
				xmmregs[mmregs].mode = readacc;
				xmmregs[mmregs].type = XMMTYPE_FPACC;
				mmregacc = mmregs;
			}
			else mmregacc = _allocFPACCtoXMMreg(-1, readacc);
		}

		xmmregs[mmregacc].mode |= MODE_WRITE;
	}
	else if( xmminfo & XMMINFO_WRITED ) {
		// check for last used, if so don't alloc a new XMM reg
		int readd = MODE_WRITE|((xmminfo&XMMINFO_READD)?MODE_READ:0);
		if( xmminfo&XMMINFO_READD ) mmregd = _allocFPtoXMMreg(-1, _Fd_, readd);
		else mmregd = _checkXMMreg(XMMTYPE_FPREG, _Fd_, readd);

		if( mmregd < 0 ) {
			if( (xmminfo&XMMINFO_READT) && mmregt >= 0 && (FPUINST_LASTUSE(_Ft_) || !FPUINST_ISLIVE(_Ft_)) ) {
				if( FPUINST_ISLIVE(_Ft_) ) {
					_freeXMMreg(mmregt);
					info &= ~PROCESS_EE_MODEWRITET;
				}
				_deleteMMXreg(MMX_FPU+_Fd_, 2);
				xmmregs[mmregt].inuse = 1;
				xmmregs[mmregt].reg = _Fd_;
				xmmregs[mmregt].mode = readd;
				mmregd = mmregt;
			}
			else if( (xmminfo&XMMINFO_READS) && mmregs >= 0 && (FPUINST_LASTUSE(_Fs_) || !FPUINST_ISLIVE(_Fs_)) ) {
				if( FPUINST_ISLIVE(_Fs_) ) {
					_freeXMMreg(mmregs);
					info &= ~PROCESS_EE_MODEWRITES;
				}
				_deleteMMXreg(MMX_FPU+_Fd_, 2);
				xmmregs[mmregs].inuse = 1;
				xmmregs[mmregs].reg = _Fd_;
				xmmregs[mmregs].mode = readd;
				mmregd = mmregs;
			}
			else if( (xmminfo&XMMINFO_READACC) && mmregacc >= 0 && (FPUINST_LASTUSE(XMMFPU_ACC) || !FPUINST_ISLIVE(XMMFPU_ACC)) ) {
				if( FPUINST_ISLIVE(XMMFPU_ACC) )
					_freeXMMreg(mmregacc);
				_deleteMMXreg(MMX_FPU+_Fd_, 2);
				xmmregs[mmregacc].inuse = 1;
				xmmregs[mmregacc].reg = _Fd_;
				xmmregs[mmregacc].mode = readd;
				xmmregs[mmregacc].type = XMMTYPE_FPREG;
				mmregd = mmregacc;
			}
			else mmregd = _allocFPtoXMMreg(-1, _Fd_, readd);
		}
	}

	assert( mmregs >= 0 || mmregt >= 0 || mmregd >= 0 || mmregacc >= 0 );

	if( xmminfo & XMMINFO_WRITED ) {
		assert( mmregd >= 0 );
		info |= PROCESS_EE_SET_D(mmregd);
	}
	if( xmminfo & (XMMINFO_WRITEACC|XMMINFO_READACC) ) {
		if( mmregacc >= 0 ) info |= PROCESS_EE_SET_ACC(mmregacc)|PROCESS_EE_ACC;
		else assert( !(xmminfo&XMMINFO_WRITEACC));		
	}

	if( xmminfo & XMMINFO_READS ) {
		if( mmregs >= 0 ) info |= PROCESS_EE_SET_S(mmregs)|PROCESS_EE_S;
	}
	if( xmminfo & XMMINFO_READT ) {
		if( mmregt >= 0 ) info |= PROCESS_EE_SET_T(mmregt)|PROCESS_EE_T;
	}
		
	// at least one must be in xmm
	if( (xmminfo & (XMMINFO_READS|XMMINFO_READT)) == (XMMINFO_READS|XMMINFO_READT) ) {
		assert( mmregs >= 0 || mmregt >= 0 );
	}

	xmmcode(info);
	_clearNeededXMMregs();
}

#undef _Ft_
#undef _Fs_
#undef _Fd_

////////////////////////////////////////////////////
u32 g_sseMXCSR = DEFAULT_sseMXCSR; 
u32 g_sseVUMXCSR = DEFAULT_sseVUMXCSR;

void SetCPUState(u32 sseMXCSR, u32 sseVUMXCSR)
{
	//SysMessage("SetCPUState: Config.sseMXCSR = %x; Config.sseVUMXCSR = %x \n", Config.sseMXCSR, Config.sseVUMXCSR);
	// SSE STATE //
	// WARNING: do not touch unless you know what you are doing

	sseMXCSR &= 0xffff; // clear the upper 16 bits since they shouldn't be set
	sseVUMXCSR &= 0xffff;

	if( !cpucaps.hasStreamingSIMD2Extensions )
	{
		// SSE1 cpus do not support Denormals Are Zero flag (throws an exception
		// if we don't mask them off)

		sseMXCSR &= ~0x0040;
		sseVUMXCSR &= ~0x0040;
	}

	g_sseMXCSR = sseMXCSR;
	g_sseVUMXCSR = sseVUMXCSR;
	// do NOT set Denormals-Are-Zero flag (charlie and chocfac messes up)
	// Update 11/05/08 - Doesnt seem to effect it anymore, for the speed boost, its on :p
	//g_sseMXCSR = 0x9f80; // changing the rounding mode to 0x2000 (near) kills grandia III!
						// changing the rounding mode to 0x0000 or 0x4000 totally kills gitaroo
						// so... grandia III wins (you can change individual games with the 'roundmode' patch command)

#ifdef _MSC_VER
	__asm ldmxcsr g_sseMXCSR; // set the new sse control
#else
    __asm__("ldmxcsr %0" : : "m"(g_sseMXCSR) );
#endif
	//g_sseVUMXCSR = g_sseMXCSR|0x6000;
}

#define REC_CACHEMEM 0x01000000
void __fastcall dyna_block_discard(u32 start,u32 sz);

static void recInit() 
{
	// Hardware Requirements Check...

	if ( !( cpucaps.hasMultimediaExtensions  ) )
		throw Exception::HardwareDeficiency( _( "Processor doesn't support MMX" ) );

	if ( !( cpucaps.hasStreamingSIMDExtensions ) )
		throw Exception::HardwareDeficiency( _( "Processor doesn't support SSE" ) );

	if ( !( cpucaps.hasStreamingSIMD2Extensions ) )
		throw Exception::HardwareDeficiency( _( "Processor doesn't support SSE2" ) );

	int i;
	const u8 macarr[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };

	if( recLUT == NULL )
		recLUT = (uptr*) _aligned_malloc( 0x010000 * sizeof(uptr), 16 );

    // can't have upper 4 bits nonzero!
	// ... then why don't we care to check if they are or not? (air)

	if( recMem == NULL )
		recMem = (u8*)SysMmap(0x0d000000, REC_CACHEMEM+0x1000);

	// This is ugly, but GCC is asking for a cast.
	if( recMem == NULL )
		throw Exception::OutOfMemory("R5900-32 failed to allocate recompiler memory.");

	// 32 alignment necessary
	if( recRAM == NULL )
		recRAM = (BASEBLOCK*) _aligned_malloc( sizeof(BASEBLOCK)/4*Ps2MemSize::Base , 4*sizeof(BASEBLOCK));
	if( recROM == NULL )
		recROM = (BASEBLOCK*) _aligned_malloc( sizeof(BASEBLOCK)/4*Ps2MemSize::Rom , 4*sizeof(BASEBLOCK));
	if( recROM1 == NULL )
		recROM1 = (BASEBLOCK*) _aligned_malloc( sizeof(BASEBLOCK)/4*Ps2MemSize::Rom1 , 4*sizeof(BASEBLOCK));
	if( recBlocks == NULL )
		recBlocks = (BASEBLOCKEX*) _aligned_malloc( sizeof(BASEBLOCKEX)*EE_NUMBLOCKS, 16);
	if( recStack == NULL )
		recStack = (u8*)malloc( RECSTACK_SIZE );

	if( s_pInstCache == NULL )
	{
		s_nInstCacheSize = 128;
		s_pInstCache = (EEINST*)malloc( sizeof(EEINST) * s_nInstCacheSize );
	}

	if( recBlocks == NULL || recRAM == NULL || recROM == NULL ||
		recROM1 == NULL || recMem == NULL || recLUT == NULL ||
		recStack == NULL || s_pInstCache == NULL )
	{
		throw Exception::OutOfMemory("Heap-based memory allocation failed." );
	}

	// No errors.. Proceed with initialization:

	ProfilerRegisterSource( "EERec", recMem, REC_CACHEMEM+0x1000 );
	memset( recLUT, 0, 0x010000 * sizeof(uptr) );

	for ( i = 0x0000; i < 0x0200; i++ )
	{
		recLUT[ i + 0x0000 ] = (uptr)&recRAM[ i << 14 ];
		recLUT[ i + 0x2000 ] = (uptr)&recRAM[ i << 14 ];
		recLUT[ i + 0x3000 ] = (uptr)&recRAM[ i << 14 ];
	}

	for ( i = 0x0000; i < 0x0040; i++ )
	{
		recLUT[ i + 0x1fc0 ] = (uptr)&recROM[ i << 14 ];
		recLUT[ i + 0x9fc0 ] = (uptr)&recROM[ i << 14 ];
		recLUT[ i + 0xbfc0 ] = (uptr)&recROM[ i << 14 ];
	}

	for ( i = 0x0000; i < 0x0004; i++ )
	{
		recLUT[ i + 0x1e00 ] = (uptr)&recROM1[ i << 14 ];
		recLUT[ i + 0x9e00 ] = (uptr)&recROM1[ i << 14 ];
		recLUT[ i + 0xbe00 ] = (uptr)&recROM1[ i << 14 ];
	}

	memcpy( recLUT + 0x8000, recLUT, 0x2000 * sizeof(uptr) );
	memcpy( recLUT + 0xa000, recLUT, 0x2000 * sizeof(uptr) );
	
	memset(recMem, 0xcd, REC_CACHEMEM);
	memset(recStack, 0, RECSTACK_SIZE);

	x86SetPtr(recMem+REC_CACHEMEM);
	dyna_block_discard_recmem=(u8*)x86Ptr;
	JMP32( (uptr)&dyna_block_discard - ( (u32)x86Ptr + 5 ));

	x86FpuState = FPU_STATE;

	SuperVUInit(-1);

	//SysMessage("recInit: Config.sseMXCSR = %x; Config.sseVUMXCSR = %x \n", Config.sseMXCSR, Config.sseVUMXCSR);
	SetCPUState(Config.sseMXCSR, Config.sseVUMXCSR);
}

////////////////////////////////////////////////////
static void recReset( void ) {

	DevCon::WriteLn( "EE Recompiler data reset" );

	s_nNextBlock = 0;
	maxrecmem = 0;
	memset( recRAM,  0, sizeof(BASEBLOCK)/4*Ps2MemSize::Base );
	memset( recROM,  0, sizeof(BASEBLOCK)/4*Ps2MemSize::Rom );
	memset( recROM1, 0, sizeof(BASEBLOCK)/4*Ps2MemSize::Rom1 );
	memset( recBlocks, 0, sizeof(BASEBLOCKEX)*EE_NUMBLOCKS );
	if( s_pInstCache ) memset( s_pInstCache, 0, sizeof(EEINST)*s_nInstCacheSize );
	ResetBaseBlockEx(0);
#ifndef PCSX2_VIRTUAL_MEM
	mmap_ResetBlockTracking();
#endif

#ifdef _MSC_VER
	__asm emms;
#else
    __asm__("emms");
#endif

#ifdef _DEBUG
	// don't clear since save states won't work
	//memset(recMem, 0xcd, REC_CACHEMEM);
#endif

	recPtr = recMem;
	recStackPtr = recStack;
	x86FpuState = FPU_STATE;
	iCWstate = 0;

	branch = 0;
}

void recShutdown( void )
{
	ProfilerTerminateSource( "EERec" );

	SafeSysMunmap(recMem, REC_CACHEMEM);

	safe_aligned_free( recLUT );
	safe_aligned_free( recRAM );
	safe_aligned_free( recROM );
	safe_aligned_free( recROM1 );
	safe_aligned_free( recBlocks );

	safe_free( s_pInstCache );
	s_nInstCacheSize = 0;

	SuperVUDestroy(-1);

	x86Shutdown();
}

void recEnableVU0micro(int enable) {
}

void recEnableVU1micro(int enable) {
}

#pragma warning(disable:4731) // frame pointer register 'ebp' modified by inline assembly code
static u32 s_uSaveESP = 0, s_uSaveEBP;

static void execute( void )
{
#ifdef _DEBUG
	u8* fnptr;
	u32 oldesi;
#else
	R5900FNPTR pfn;
#endif
	BASEBLOCK* pblock = PC_GETBLOCK(cpuRegs.pc);

	if ( !pblock->pFnptr || pblock->startpc != cpuRegs.pc ) {
		recRecompile(cpuRegs.pc);
	}

	assert( pblock->pFnptr != 0 );
	g_EEFreezeRegs = true;

	// skip the POPs

#ifdef _DEBUG
	fnptr = (u8*)pblock->pFnptr;

#ifdef _MSC_VER
	__asm {
		// save data
		mov oldesi, esi
		mov s_uSaveESP, esp
		sub s_uSaveESP, 8
		mov s_uSaveEBP, ebp
		push ebp

		call fnptr // jump into function
		// restore data
		pop ebp
		mov esi, oldesi
	}
#else

    __asm__("movl %%esi, %0\n"
            "movl %%esp, %1\n"
            "sub $8, %1\n"
            "push %%ebp\n"
            "call *%2\n"
            "pop %%ebp\n"
            "movl %0, %%esi\n" : "=m"(oldesi), "=m"(s_uSaveESP) : "c"(fnptr) );
#endif // _MSC_VER

#else

#ifdef _MSC_VER
	pfn = ((R5900FNPTR)pblock->pFnptr);
	// use call instead of pfn()
	__asm push ebp; // FIXME: need to preserve ebp or else the bios crashes, should find where ebp is getting corrupted instead.
	__asm call pfn;
	__asm pop ebp;	// restore ebp for the reason above
#else
    ((R5900FNPTR)pblock->pFnptr)();
#endif

#endif

	g_EEFreezeRegs = false;
}

void recStep( void ) {
}

void recExecute( void ) {
	if( Config.Options & PCSX2_EEREC ) Config.Options |= PCSX2_COP2REC;

	for (;;)
		execute();
}

void recExecuteBlock( void ) {
	execute();
}

////////////////////////////////////////////////////
extern u32 g_nextBranchCycle;

u32 g_lastpc = 0;
u32 g_EEDispatchTemp;
u32 s_pCurBlock_ltime;

#ifdef _MSC_VER

// jumped to when invalid pc address
__declspec(naked,noreturn) void Dispatcher()
{
	// EDX contains the jump addr to modify
	__asm push edx

	// calc PC_GETBLOCK
	s_pDispatchBlock = PC_GETBLOCK(cpuRegs.pc);
	
	__asm {
		mov eax, s_pDispatchBlock

		// check if startpc == cpuRegs.pc
		mov ecx, cpuRegs.pc
		//and ecx, 0x5fffffff // remove higher bits
		cmp ecx, dword ptr [eax+BLOCKTYPE_STARTPC]
		je CheckPtr

		// recompile
		push cpuRegs.pc // pc
		call recRecompile
		add esp, 4 // pop old param
		mov eax, s_pDispatchBlock
CheckPtr:
		mov eax, dword ptr [eax]
	}

#ifdef _DEBUG
	__asm mov g_EEDispatchTemp, eax
	assert( g_EEDispatchTemp );
#endif

//	__asm {
//		test eax, 0x40000000 // BLOCKTYPE_NEEDCLEAR
//		jz Done
//		// move new pc
//		and eax, 0x0fffffff
//		mov ecx, cpuRegs.pc
//		mov dword ptr [eax+1], ecx
//	}
	__asm {
		and eax, 0x0fffffff
		mov edx, eax
		pop ecx // x86Ptr to mod
		sub edx, ecx
		sub edx, 4
		mov dword ptr [ecx], edx

		jmp eax
	}
}

__declspec(naked,noreturn) void DispatcherClear()
{
	// EDX contains the current pc
	__asm mov cpuRegs.pc, edx
	__asm push edx

	// calc PC_GETBLOCK
	s_pDispatchBlock = PC_GETBLOCK(cpuRegs.pc);

	if( s_pDispatchBlock->startpc == cpuRegs.pc ) {
		assert( s_pDispatchBlock->pFnptr != 0 );

		// already modded the code, jump to the new place
		__asm {
			pop edx
			add esp, 4 // ignore stack
			mov eax, s_pDispatchBlock
			mov eax, dword ptr [eax]
			and eax, 0x0fffffff
			jmp eax
		}
	}

	__asm {
		call recRecompile
		add esp, 4 // pop old param
		mov eax, s_pDispatchBlock
		mov eax, dword ptr [eax]

		pop ecx // old fnptr

		and eax, 0x0fffffff
		mov byte ptr [ecx], 0xe9 // jmp32
		mov edx, eax
		sub edx, ecx
		sub edx, 5
		mov dword ptr [ecx+1], edx

		jmp eax
	}
}

// called when jumping to variable pc address
__declspec(naked,noreturn) void DispatcherReg()
{
	__asm {
		//s_pDispatchBlock = PC_GETBLOCK(cpuRegs.pc);
		mov edx, cpuRegs.pc
		mov ecx, edx
	}

	__asm {
		shr edx, 14
		and edx, 0xfffffffc
		add edx, recLUT
		mov edx, dword ptr [edx]

		mov eax, ecx
		and eax, 0xfffc
		// edx += 2*eax
		shl eax, 1
		add edx, eax
		
		// check if startpc == cpuRegs.pc
		mov eax, ecx
		//and eax, 0x5fffffff // remove higher bits
		cmp eax, dword ptr [edx+BLOCKTYPE_STARTPC]
		jne recomp

		mov eax, dword ptr [edx]
	}

#ifdef _DEBUG
	__asm mov g_EEDispatchTemp, eax
	assert( g_EEDispatchTemp );
#endif

	__asm {
		and eax, 0x0fffffff
		jmp eax // fnptr

recomp:
		sub esp, 8
		mov dword ptr [esp+4], edx
		mov dword ptr [esp], ecx
		call recRecompile
		mov edx, dword ptr [esp+4]
		add esp, 8
		
		mov eax, dword ptr [edx]
		and eax, 0x0fffffff
		jmp eax // fnptr
	}
}

#ifdef PCSX2_DEVBUILD
__declspec(naked) void _StartPerfCounter()
{
	__asm {
		push eax
		push ebx
		push ecx
	
		rdtsc
		mov dword ptr [offset lbase], eax
		mov dword ptr [offset lbase + 4], edx

		pop ecx
		pop ebx
		pop eax
		ret
	}
}

__declspec(naked) void _StopPerfCounter()
{
	__asm {
		push eax
		push ebx
		push ecx
	
		rdtsc
	
		sub eax, dword ptr [offset lbase]
		sbb edx, dword ptr [offset lbase + 4]
		mov ecx, s_pCurBlock_ltime
		add eax, dword ptr [ecx]
		adc edx, dword ptr [ecx + 4]
		mov dword ptr [ecx], eax
		mov dword ptr [ecx + 4], edx
		pop ecx
		pop ebx
		pop eax
		ret
	}
}

#endif // PCSX2_DEVBUILD

#else // _MSC_VER
// Linux uses an assembly version of these routines.
#ifdef __LINUX__
extern "C" {
#endif
extern void Dispatcher();
extern void DispatcherClear();
extern void DispatcherReg();
#ifdef __LINUX__
}
#endif
extern void _StartPerfCounter();
extern void _StopPerfCounter();

#endif

#ifdef PCSX2_DEVBUILD
void StartPerfCounter()
{
#ifdef PCSX2_DEVBUILD
//	if( s_startcount ) {
//		CALLFunc((uptr)_StartPerfCounter);
//	}
#endif
}

void StopPerfCounter()
{
#ifdef PCSX2_DEVBUILD
//	if( s_startcount ) {
//		MOV32ItoM((uptr)&s_pCurBlock_ltime, (u32)&s_pCurBlockEx->ltime);
//		CALLFunc((uptr)_StopPerfCounter);
//	}
#endif
}
#endif

////////////////////////////////////////////////////
void recClear( u32 Addr, u32 Size )
{
	u32 i;
	for(i = 0; i < Size; ++i, Addr+=4) {
		REC_CLEARM(Addr);
	}
}

#define EE_MIN_BLOCK_BYTES 15

void recClearMem(BASEBLOCK* p)
{
	BASEBLOCKEX* pexblock;
	BASEBLOCK* pstart;
	int lastdelay;

	// necessary since recompiler doesn't call femms/emms
#ifdef _MSC_VER
	if (cpucaps.has3DNOWInstructionExtensions) __asm femms;
	else __asm emms;
#else
    if( cpucaps.has3DNOWInstructionExtensions )__asm__("femms");
    else __asm__("emms");
#endif
		
	assert( p != NULL );

	if( p->uType & BLOCKTYPE_DELAYSLOT ) {
		recClearMem(p-1);
		if( p->pFnptr == 0 )
			return;
	}

	assert( p->pFnptr != 0 );
	assert( p->startpc );

	x86Ptr = (u8*)p->pFnptr;

	// there is a small problem: mem can be ored with 0xa<<28 or 0x8<<28, and don't know which
	MOV32ItoR(EDX, p->startpc);
	PUSH32I((u32)x86Ptr); // will be replaced by JMP32
	JMP32((u32)DispatcherClear - ( (u32)x86Ptr + 5 ));
	assert( x86Ptr == (u8*)p->pFnptr + EE_MIN_BLOCK_BYTES );

	pstart = PC_GETBLOCK(p->startpc);
	pexblock = PC_GETBLOCKEX(pstart);
	assert( pexblock->startpc == pstart->startpc );

    if( pexblock->startpc != pstart->startpc ) {
        // some bug with ffx after beating a big snake in sewers
        RemoveBaseBlockEx(pexblock, 0);
	    pexblock->size = 0;
	    pexblock->startpc = 0;
        return;
    }

//	if( pexblock->pOldFnptr ) {
//		// have to mod oldfnptr too
//		x86Ptr = pexblock->pOldFnptr;
//
//		MOV32ItoR(EDX, p->startpc);
//		JMP32((u32)DispatcherClear - ( (u32)x86Ptr + 5 ));
//	}
//	else
//		pexblock->pOldFnptr = (u8*)p->pFnptr;
	
	// don't delete if last is delay
	lastdelay = pexblock->size;
	if( pstart[pexblock->size-1].uType & BLOCKTYPE_DELAYSLOT ) {
		assert( pstart[pexblock->size-1].pFnptr != pstart->pFnptr );
		if( pstart[pexblock->size-1].pFnptr != 0 ) {
			pstart[pexblock->size-1].uType = 0;
			--lastdelay;
		}
	}

	memset(pstart, 0, lastdelay*sizeof(BASEBLOCK));

	RemoveBaseBlockEx(pexblock, 0);
	pexblock->size = 0;
	pexblock->startpc = 0;
}

// check for end of bios
void CheckForBIOSEnd()
{
	MOV32MtoR(EAX, (int)&cpuRegs.pc);

	CMP32ItoR(EAX, 0x00200008);
	j8Ptr[0] = JE8(0);

	CMP32ItoR(EAX, 0x00100008);
	j8Ptr[1] = JE8(0);

	// return
	j8Ptr[2] = JMP8(0);

	x86SetJ8( j8Ptr[0] );
	x86SetJ8( j8Ptr[1] );

	// bios end
	RET2();

	x86SetJ8( j8Ptr[2] );
}

static int *s_pCode;

void SetBranchReg( u32 reg )
{
	branch = 1;

	if( reg != 0xffffffff ) {
//		if( GPR_IS_CONST1(reg) )
//			MOV32ItoM( (uptr)&cpuRegs.pc, g_cpuConstRegs[reg].UL[0] );
//		else {
//			int mmreg;
//			
//			if( (mmreg = _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ)) >= 0 ) {
//				SSE_MOVSS_XMM_to_M32((u32)&cpuRegs.pc, mmreg);
//			}
//			else if( (mmreg = _checkMMXreg(MMX_GPR+reg, MODE_READ)) >= 0 ) {
//				MOVDMMXtoM((u32)&cpuRegs.pc, mmreg);
//				SetMMXstate();
//			}
//			else {
//				MOV32MtoR(EAX, (int)&cpuRegs.GPR.r[ reg ].UL[ 0 ] );
//				MOV32RtoM((u32)&cpuRegs.pc, EAX);
//			}
//		}
		_allocX86reg(ESI, X86TYPE_PCWRITEBACK, 0, MODE_WRITE);
		_eeMoveGPRtoR(ESI, reg);

		recompileNextInstruction(1);

		if( x86regs[ESI].inuse ) {
			assert( x86regs[ESI].type == X86TYPE_PCWRITEBACK );
			MOV32RtoM((int)&cpuRegs.pc, ESI);
			x86regs[ESI].inuse = 0;
		}
		else {
			MOV32MtoR(EAX, (u32)&g_recWriteback);
			MOV32RtoM((int)&cpuRegs.pc, EAX);
		}
	}

//	CMP32ItoM((u32)&cpuRegs.pc, 0);
//	j8Ptr[5] = JNE8(0);
//	CALLFunc((uptr)tempfn);
//	x86SetJ8( j8Ptr[5] );

	iFlushCall(FLUSH_EVERYTHING);

	iBranchTest(0xffffffff, 1);
	if( bExecBIOS ) CheckForBIOSEnd();

	JMP32((u32)DispatcherReg - ( (u32)x86Ptr + 5 ));
}

void SetBranchImm( u32 imm )
{
	u32* ptr;
	branch = 1;

	assert( imm );

	// end the current block
	MOV32ItoM( (uptr)&cpuRegs.pc, imm );
	iFlushCall(FLUSH_EVERYTHING);

	iBranchTest(imm, imm <= pc);
	if( bExecBIOS ) CheckForBIOSEnd();

	MOV32ItoR(EDX, 0);
	ptr = (u32*)(x86Ptr-4);
	*ptr = (u32)JMP32((u32)Dispatcher - ( (u32)x86Ptr + 5 ));
}

void SaveBranchState()
{
	s_savex86FpuState = x86FpuState;
	s_saveiCWstate = iCWstate;
	s_savenBlockCycles = s_nBlockCycles;
	s_saveConstGPRreg = 0xffffffff; // indicate searching
	s_saveHasConstReg = g_cpuHasConstReg;
	s_saveFlushedConstReg = g_cpuFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;
	s_saveRegHasLive1 = g_cpuRegHasLive1;
	s_saveRegHasSignExt = g_cpuRegHasSignExt;

	// save all mmx regs
	memcpy(s_saveMMXregs, mmxregs, sizeof(mmxregs));
	memcpy(s_saveXMMregs, xmmregs, sizeof(xmmregs));
}

void LoadBranchState()
{
	x86FpuState = s_savex86FpuState;
	iCWstate = s_saveiCWstate;
	s_nBlockCycles = s_savenBlockCycles;

	if( s_saveConstGPRreg != 0xffffffff ) {
		assert( s_saveConstGPRreg > 0 );

		// make sure right GPR was saved
		assert( g_cpuHasConstReg == s_saveHasConstReg || (g_cpuHasConstReg ^ s_saveHasConstReg) == (1<<s_saveConstGPRreg) );

		// restore the GPR reg
		g_cpuConstRegs[s_saveConstGPRreg] = s_ConstGPRreg;
		GPR_SET_CONST(s_saveConstGPRreg);

		s_saveConstGPRreg = 0;
	}

	g_cpuHasConstReg = s_saveHasConstReg;
	g_cpuFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;
	g_cpuRegHasLive1 = g_cpuPrevRegHasLive1 = s_saveRegHasLive1;
	g_cpuRegHasSignExt = g_cpuPrevRegHasSignExt = s_saveRegHasSignExt;

	// restore all mmx regs
	memcpy(mmxregs, s_saveMMXregs, sizeof(mmxregs));
	memcpy(xmmregs, s_saveXMMregs, sizeof(xmmregs));
}

void iFlushCall(int flushtype)
{
	_freeX86regs();

	if( flushtype & FLUSH_FREE_XMM )
		_freeXMMregs();
	else if( flushtype & FLUSH_FLUSH_XMM)
		_flushXMMregs();

	if( flushtype & FLUSH_FREE_MMX )
		_freeMMXregs();
	else if( flushtype & FLUSH_FLUSH_MMX)
		_flushMMXregs();

	if( flushtype & FLUSH_CACHED_REGS )
		_flushConstRegs();

	LoadCW();
	
	if (x86FpuState==MMX_STATE) {
		if (cpucaps.has3DNOWInstructionExtensions) FEMMS();
		else EMMS();
		x86FpuState=FPU_STATE;
	}
}

//void testfpu()
//{
//	int i;
//	for(i = 0; i < 32; ++i ) {
//		if( fpuRegs.fpr[i].UL== 0x7f800000 || fpuRegs.fpr[i].UL == 0xffc00000) {
//			SysPrintf("bad fpu: %x %x %x\n", i, cpuRegs.cycle, g_lastpc);
//		}
//
//		if( VU0.VF[i].UL[0] == 0xffc00000 || //(VU0.VF[i].UL[1]&0xffc00000) == 0xffc00000 ||
//			VU0.VF[i].UL[0] == 0x7f800000) {
//			SysPrintf("bad vu0: %x %x %x\n", i, cpuRegs.cycle, g_lastpc);
//		}
//	}
//}


static u32 eeScaleBlockCycles()
{
	// Note: s_nBlockCycles is 3 bit fixed point.  Divide by 8 when done!

	// Let's not scale blocks under 5-ish cycles.  This fixes countless "problems"
	// caused by sync hacks and such, since games seem to care a lot more about
	// these small blocks having accurate cycle counts.

	if( s_nBlockCycles <= (5<<3) || (CHECK_EE_CYCLERATE == 0) )
		return s_nBlockCycles >> 3;

	uint scalarLow, scalarMid, scalarHigh;

	// Note: larger blocks get a smaller scalar, to help keep
	// them from becoming "too fat" and delaying branch tests.

	switch( CHECK_EE_CYCLERATE )
	{
		case 0:	return s_nBlockCycles >> 3;

		case 1:		// Sync hack x1.5!
			scalarLow = 5;
			scalarMid = 7;
			scalarHigh = 5;
		break;

		case 2:		// Sync hack x2
			scalarLow = 7;
			scalarMid = 9;
			scalarHigh = 7;
		break;

		case 3:		// Sync hack x3
			scalarLow = 10;
			scalarMid = 19;
			scalarHigh = 10;
		break;

		jNO_DEFAULT
	}

	s_nBlockCycles *= 
		(s_nBlockCycles <= (10<<3)) ? scalarLow :
		((s_nBlockCycles > (21<<3)) ? scalarHigh : scalarMid );

	return s_nBlockCycles >> (3+2);
}

static void iBranchTest(u32 newpc, u32 cpuBranch)
{
#ifdef PCSX2_DEVBUILD
//	if( s_startcount ) {
//		StopPerfCounter();
//		ADD32ItoM( (u32)&s_pCurBlockEx->visited, 1 );
//	}
#endif

#ifdef _DEBUG
	//CALLFunc((uptr)testfpu);
#endif

	MOV32MtoR(ECX, (uptr)&cpuRegs.cycle);
	ADD32ItoR(ECX, eeScaleBlockCycles());
	MOV32RtoM((uptr)&cpuRegs.cycle, ECX); // update cycles
	SUB32MtoR(ECX, (uptr)&g_nextBranchCycle);

	// check if should branch
	j8Ptr[0] = JS8( 0 );

	// has to be in the middle of Save/LoadBranchState
	CALLFunc( (uptr)cpuBranchTest );

	if( newpc != 0xffffffff )
	{
		CMP32ItoM((uptr)&cpuRegs.pc, newpc);
		JNE32((uptr)DispatcherReg - ( (uptr)x86Ptr + 6 ));
	}

	x86SetJ8( j8Ptr[0] );
}

namespace EE { namespace Dynarec { namespace OpcodeImpl
{

////////////////////////////////////////////////////
#ifndef CP2_RECOMPILE

REC_SYS(COP2);

#else

void recCOP2( void )
{ 
	CPU_LOG( "Recompiling COP2:%s\n", disR5900Current.getString() );
	recCOP22( );
}

#endif

////////////////////////////////////////////////////
void recSYSCALL( void ) {
	MOV32ItoM( (uptr)&cpuRegs.code, cpuRegs.code );
	MOV32ItoM( (uptr)&cpuRegs.pc, pc );
	iFlushCall(FLUSH_NODESTROY);
	CALLFunc( (uptr)Interpreter::OpcodeImpl::SYSCALL );

	CMP32ItoM((uptr)&cpuRegs.pc, pc);
	j8Ptr[0] = JE8(0);
	ADD32ItoM((uptr)&cpuRegs.cycle, eeScaleBlockCycles());
	JMP32((uptr)DispatcherReg - ( (uptr)x86Ptr + 5 ));
	x86SetJ8(j8Ptr[0]);
	//branch = 2;
}

////////////////////////////////////////////////////
void recBREAK( void ) {
	MOV32ItoM( (uptr)&cpuRegs.code, cpuRegs.code );
	MOV32ItoM( (uptr)&cpuRegs.pc, pc );
	iFlushCall(FLUSH_EVERYTHING);
	CALLFunc( (uptr)EE::Interpreter::OpcodeImpl::BREAK );

	CMP32ItoM((uptr)&cpuRegs.pc, pc);
	j8Ptr[0] = JE8(0);
	ADD32ItoM((uptr)&cpuRegs.cycle, eeScaleBlockCycles());
	RET();
	x86SetJ8(j8Ptr[0]);
	//branch = 2;
}

////////////////////////////////////////////////////
//static void recCACHE( void ) {
//	MOV32ItoM( (uptr)&cpuRegs.code, cpuRegs.code );
//	MOV32ItoM( (uptr)&cpuRegs.pc, pc );
//	iFlushCall(FLUSH_EVERYTHING);
//	CALLFunc( (uptr)CACHE );
//	//branch = 2;
//
//	CMP32ItoM((int)&cpuRegs.pc, pc);
//	j8Ptr[0] = JE8(0);
//	RET();
//	x86SetJ8(j8Ptr[0]);
//}


void recPREF( void ) 
{
}

void recSYNC( void )
{
}

void recMFSA( void ) 
{
	int mmreg;
	if (!_Rd_) return;

	mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_WRITE);
	if( mmreg >= 0 ) {
		SSE_MOVLPS_M64_to_XMM(mmreg, (uptr)&cpuRegs.sa);
	}
	else if( (mmreg = _checkMMXreg(MMX_GPR+_Rd_, MODE_WRITE)) >= 0 ) {
		MOVDMtoMMX(mmreg, (uptr)&cpuRegs.sa);
		SetMMXstate();
	}
	else {
		MOV32MtoR(EAX, (u32)&cpuRegs.sa);
		_deleteEEreg(_Rd_, 0);
		MOV32RtoM((uptr)&cpuRegs.GPR.r[_Rd_].UL[0], EAX);
		MOV32ItoM((uptr)&cpuRegs.GPR.r[_Rd_].UL[1], 0);
	}
}

void recMTSA( void )
{
	if( GPR_IS_CONST1(_Rs_) ) {
		MOV32ItoM((uptr)&cpuRegs.sa, g_cpuConstRegs[_Rs_].UL[0] );
	}
	else {
		int mmreg;
		
		if( (mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0 ) {
			SSE_MOVSS_XMM_to_M32((uptr)&cpuRegs.sa, mmreg);
		}
		else if( (mmreg = _checkMMXreg(MMX_GPR+_Rs_, MODE_READ)) >= 0 ) {
			MOVDMMXtoM((uptr)&cpuRegs.sa, mmreg);
			SetMMXstate();
		}
		else {
			MOV32MtoR(EAX, (uptr)&cpuRegs.GPR.r[_Rs_].UL[0]);
			MOV32RtoM((uptr)&cpuRegs.sa, EAX);
		}
	}
}

void recMTSAB( void ) 
{
	if( GPR_IS_CONST1(_Rs_) ) {
		MOV32ItoM((uptr)&cpuRegs.sa, ((g_cpuConstRegs[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF)) << 3);
	}
	else {
		_eeMoveGPRtoR(EAX, _Rs_);
		AND32ItoR(EAX, 0xF);
		XOR32ItoR(EAX, _Imm_&0xf);
		SHL32ItoR(EAX, 3);
		MOV32RtoM((uptr)&cpuRegs.sa, EAX);
	}
}

void recMTSAH( void ) 
{
	if( GPR_IS_CONST1(_Rs_) ) {
		MOV32ItoM((uptr)&cpuRegs.sa, ((g_cpuConstRegs[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 4);
	}
	else {
		_eeMoveGPRtoR(EAX, _Rs_);
		AND32ItoR(EAX, 0x7);
		XOR32ItoR(EAX, _Imm_&0x7);
		SHL32ItoR(EAX, 4);
		MOV32RtoM((uptr)&cpuRegs.sa, EAX);
	}
}

}}}		// end Namespace EE::Dynarec::OpcodeImpl

static void checkcodefn()
{
	int pctemp;

#ifdef _MSC_VER
	__asm mov pctemp, eax;
#else
    __asm__("movl %%eax, %0" : "=m"(pctemp) );
#endif

	SysPrintf("code changed! %x\n", pctemp);
	assert(0);
}

void checkpchanged(u32 startpc)
{
	assert(0);
}

//#ifdef _DEBUG
//#define CHECK_XMMCHANGED() CALLFunc((uptr)checkxmmchanged);
//#else
//#define CHECK_XMMCHANGED()
//#endif
//
//static void checkxmmchanged()
//{
//	assert( !g_globalMMXSaved );
//	assert( !g_globalXMMSaved );
//}

u32 recompileCodeSafe(u32 temppc)
{
	BASEBLOCK* pblock = PC_GETBLOCK(temppc);

	if( pblock->pFnptr != 0 && pblock->startpc != s_pCurBlock->startpc ) {
		if( pc == pblock->startpc )
			return 0;
	}

	return 1;
}

void recompileNextInstruction(int delayslot)
{
	static u8 s_bFlushReg = 1;
	int i, count;

	BASEBLOCK* pblock = PC_GETBLOCK(pc);

	// need *ppblock != s_pCurBlock because of branches
	if( pblock->pFnptr != 0 && pblock->startpc != s_pCurBlock->startpc ) {

		if( !delayslot && pc == pblock->startpc ) {
			// code already in place, so jump to it and exit recomp
			assert( PC_GETBLOCKEX(pblock)->startpc == pblock->startpc );
			
			iFlushCall(FLUSH_EVERYTHING);
			MOV32ItoM((uptr)&cpuRegs.pc, pc);
				
//			if( pexblock->pOldFnptr ) {
//				// code already in place, so jump to it and exit recomp
//				JMP32((u32)pexblock->pOldFnptr - ((u32)x86Ptr + 5));
//				branch = 3;
//				return;
//			}
			
			JMP32((uptr)pblock->pFnptr - ((uptr)x86Ptr + 5));
			branch = 3;
			return;
		}
		else {

			if( !(delayslot && pblock->startpc == pc) ) {
				u8* oldX86 = x86Ptr;
				//__Log("clear block %x\n", pblock->startpc);
				recClearMem(pblock);
				x86Ptr = oldX86;
				if( delayslot )
					SysPrintf("delay slot %x\n", pc);
			}
		}
	}

	if( delayslot )
		pblock->uType = BLOCKTYPE_DELAYSLOT;
		
	s_pCode = (int *)PSM( pc );
	assert(s_pCode);

#ifdef _DEBUG
	MOV32ItoR(EAX, pc);
#endif

	cpuRegs.code = *(int *)s_pCode;
	pc += 4;
	
//#ifdef _DEBUG
//	CMP32ItoM((u32)s_pCode, cpuRegs.code);
//	j8Ptr[0] = JE8(0);
//	MOV32ItoR(EAX, pc);
//	CALLFunc((uptr)checkcodefn);
//	x86SetJ8( j8Ptr[ 0 ] );
//
//	if( !delayslot ) {
//		CMP32ItoM((u32)&cpuRegs.pc, s_pCurBlockEx->startpc);
//		j8Ptr[0] = JB8(0);
//		CMP32ItoM((u32)&cpuRegs.pc, pc);
//		j8Ptr[1] = JA8(0);
//		j8Ptr[2] = JMP8(0);
//		x86SetJ8( j8Ptr[ 0 ] );
//		x86SetJ8( j8Ptr[ 1 ] );
//		PUSH32I(s_pCurBlockEx->startpc);
//		CALLFunc((uptr)checkpchanged);
//		ADD32ItoR(ESP, 4);
//		x86SetJ8( j8Ptr[ 2 ] );	
//	}
//#endif

	g_pCurInstInfo++;

	// reorder register priorities
//	for(i = 0; i < X86REGS; ++i) {
//		if( x86regs[i].inuse ) {
//			if( count > 0 ) mmxregs[i].counter = 1000-count;
//			else mmxregs[i].counter = 0;
//		}
//	}

	for(i = 0; i < MMXREGS; ++i) {
		if( mmxregs[i].inuse ) {
			assert( MMX_ISGPR(mmxregs[i].reg) );
			count = _recIsRegWritten(g_pCurInstInfo, (s_nEndBlock-pc)/4 + 1, XMMTYPE_GPRREG, mmxregs[i].reg-MMX_GPR);
			if( count > 0 ) mmxregs[i].counter = 1000-count;
			else mmxregs[i].counter = 0;
		}
	}

	for(i = 0; i < XMMREGS; ++i) {
		if( xmmregs[i].inuse ) {
			count = _recIsRegWritten(g_pCurInstInfo, (s_nEndBlock-pc)/4 + 1, xmmregs[i].type, xmmregs[i].reg);
			if( count > 0 ) xmmregs[i].counter = 1000-count;
			else xmmregs[i].counter = 0;
		}
	}

	const EE::OPCODE& opcode = EE::GetCurrentInstruction();

	// peephole optimizations
	if( g_pCurInstInfo->info & EEINSTINFO_COREC ) {

#ifdef PCSX2_VIRTUAL_MEM
		if( g_pCurInstInfo->numpeeps > 1 ) {
			switch(_Opcode_) {
				case 30: EE::Dynarec::OpcodeImpl::recLQ_coX(g_pCurInstInfo->numpeeps); break;
				case 31: EE::Dynarec::OpcodeImpl::recSQ_coX(g_pCurInstInfo->numpeeps); break;
				case 49: EE::Dynarec::OpcodeImpl::recLWC1_coX(g_pCurInstInfo->numpeeps); break;
				case 57: EE::Dynarec::OpcodeImpl::recSWC1_coX(g_pCurInstInfo->numpeeps); break;
				case 55: EE::Dynarec::OpcodeImpl::recLD_coX(g_pCurInstInfo->numpeeps); break;
				case 63: EE::Dynarec::OpcodeImpl::recSD_coX(g_pCurInstInfo->numpeeps, 1); break; //not sure if should be set to 1 or 0; looks like "1" handles alignment, so i'm going with that for now
				default:
					assert(0);
			}
			pc += g_pCurInstInfo->numpeeps*4;
			s_nBlockCycles += (g_pCurInstInfo->numpeeps+1) * opcode.cycles;
			g_pCurInstInfo += g_pCurInstInfo->numpeeps;
		}
		else {
			EE::Dynarec::recBSC_co[_Opcode_]();
			pc += 4;
			g_pCurInstInfo++;
			s_nBlockCycles += opcode.cycles*2;
		}
#else
		assert(0);
#endif
	}
	else {
	 	assert( !(g_pCurInstInfo->info & EEINSTINFO_NOREC) );

		// if this instruction is a jump or a branch, exit right away
		if( delayslot ) {
			switch(_Opcode_) {
				case 1:
					switch(_Rt_) {
						case 0: case 1: case 2: case 3: case 0x10: case 0x11: case 0x12: case 0x13:
							SysPrintf("branch %x in delay slot!\n", cpuRegs.code);
							_clearNeededX86regs();
							_clearNeededMMXregs();
							_clearNeededXMMregs();
							return;
					}
					break;

				case 2: case 3: case 4: case 5: case 6: case 7: case 0x14: case 0x15: case 0x16: case 0x17:
					SysPrintf("branch %x in delay slot!\n", cpuRegs.code);
					_clearNeededX86regs();
					_clearNeededMMXregs();
					_clearNeededXMMregs();
					return;
			}
		}
		opcode.recompile();
		s_nBlockCycles += (opcode.cycles==0) ? 6 : opcode.cycles;
	}

	if( !delayslot ) {
		if( s_bFlushReg ) {
			//if( !_flushUnusedConstReg() ) {
				int flushed = 0;
				if( _getNumMMXwrite() > 3 ) flushed = _flushMMXunused();
				if( !flushed && _getNumXMMwrite() > 2 ) _flushXMMunused();
				s_bFlushReg = !flushed;
//			}
//			else s_bFlushReg = 0;
		}
		else s_bFlushReg = 1;
	}
	else s_bFlushReg = 1;

	//CHECK_XMMCHANGED();
	_clearNeededX86regs();
	_clearNeededMMXregs();
	_clearNeededXMMregs();

//	_freeXMMregs();
//	_freeMMXregs();
//	_flushCachedRegs();
//	g_cpuHasConstReg = 1;
}

//__declspec(naked) void iDummyBlock()
//{
////	g_lastpc = cpuRegs.pc;
////
////	do {
////		cpuRegs.cycle = g_nextBranchCycle;
////		cpuBranchTest();
////	} while(g_lastpc == cpuRegs.pc);
////
////	__asm jmp DispatcherReg
//	__asm {
//RepDummy:
//		add cpuRegs.cycle, 9
//		call cpuBranchTest
//		cmp cpuRegs.pc, 0x81fc0
//		je RepDummy
//		jmp DispatcherReg
//	}
//}

////////////////////////////////////////////////////
#include "R3000A.h"
#include "PsxCounters.h"
#include "PsxMem.h"
extern tIPU_BP g_BP;

extern u32 psxdump;
extern void iDumpPsxRegisters(u32 startpc, u32 temp); 
extern Counter counters[6];
extern int rdram_devices;	// put 8 for TOOL and 2 for PS2 and PSX
extern int rdram_sdevid;

void iDumpRegisters(u32 startpc, u32 temp)
{
// [TODO] fixme : thie code is broken and has no labels.  Needs a rewrite to be useful.

#if 0

	int i;
	const char* pstr;// = temp ? "t" : "";
	const u32 dmacs[] = {0x8000, 0x9000, 0xa000, 0xb000, 0xb400, 0xc000, 0xc400, 0xc800, 0xd000, 0xd400 };
	extern const char *disRNameGPR[];
    const char* psymb;
	
	if (temp)
		pstr = "t";
	else
		pstr = "";
	
    psymb = disR5900GetSym(startpc);

    if( psymb != NULL )
        __Log("%sreg(%s): %x %x c:%x\n", pstr, psymb, startpc, cpuRegs.interrupt, cpuRegs.cycle);
    else
        __Log("%sreg: %x %x c:%x\n", pstr, startpc, cpuRegs.interrupt, cpuRegs.cycle);
	for(i = 1; i < 32; ++i) __Log("%s: %x_%x_%x_%x\n", disRNameGPR[i], cpuRegs.GPR.r[i].UL[3], cpuRegs.GPR.r[i].UL[2], cpuRegs.GPR.r[i].UL[1], cpuRegs.GPR.r[i].UL[0]);
    //for(i = 0; i < 32; i+=4) __Log("cp%d: %x_%x_%x_%x\n", i, cpuRegs.CP0.r[i], cpuRegs.CP0.r[i+1], cpuRegs.CP0.r[i+2], cpuRegs.CP0.r[i+3]);
	//for(i = 0; i < 32; ++i) __Log("%sf%d: %f %x\n", pstr, i, fpuRegs.fpr[i].f, fpuRegs.fprc[i]);
	//for(i = 1; i < 32; ++i) __Log("%svf%d: %f %f %f %f, vi: %x\n", pstr, i, VU0.VF[i].F[3], VU0.VF[i].F[2], VU0.VF[i].F[1], VU0.VF[i].F[0], VU0.VI[i].UL);
	for(i = 0; i < 32; ++i) __Log("%sf%d: %x %x\n", pstr, i, fpuRegs.fpr[i].UL, fpuRegs.fprc[i]);
	for(i = 1; i < 32; ++i) __Log("%svf%d: %x %x %x %x, vi: %x\n", pstr, i, VU0.VF[i].UL[3], VU0.VF[i].UL[2], VU0.VF[i].UL[1], VU0.VF[i].UL[0], VU0.VI[i].UL);
	__Log("%svfACC: %x %x %x %x\n", pstr, VU0.ACC.UL[3], VU0.ACC.UL[2], VU0.ACC.UL[1], VU0.ACC.UL[0]);
	__Log("%sLO: %x_%x_%x_%x, HI: %x_%x_%x_%x\n", pstr, cpuRegs.LO.UL[3], cpuRegs.LO.UL[2], cpuRegs.LO.UL[1], cpuRegs.LO.UL[0],
	cpuRegs.HI.UL[3], cpuRegs.HI.UL[2], cpuRegs.HI.UL[1], cpuRegs.HI.UL[0]);
	__Log("%sCycle: %x %x, Count: %x\n", pstr, cpuRegs.cycle, g_nextBranchCycle, cpuRegs.CP0.n.Count);
	iDumpPsxRegisters(psxRegs.pc, temp);

    __Log("f410,30,40: %x %x %x, %d %d\n", psHu32(0xf410), psHu32(0xf430), psHu32(0xf440), rdram_sdevid, rdram_devices);
	__Log("cyc11: %x %x; vu0: %x, vu1: %x\n", cpuRegs.sCycle[1], cpuRegs.eCycle[1], VU0.cycle, VU1.cycle);

	__Log("%scounters: %x %x; psx: %x %x\n", pstr, nextsCounter, nextCounter, psxNextsCounter, psxNextCounter);
	for(i = 0; i < 4; ++i) {
		__Log("eetimer%d: count: %x mode: %x target: %x %x; %x %x; %x %x %x %x\n", i,
			counters[i].count, counters[i].mode, counters[i].target, counters[i].hold, counters[i].rate,
			counters[i].interrupt, counters[i].Cycle, counters[i].sCycle, counters[i].CycleT, counters[i].sCycleT);
	}
	__Log("VIF0_STAT = %x, VIF1_STAT = %x\n", psHu32(0x3800), psHu32(0x3C00));
	__Log("ipu %x %x %x %x; bp: %x %x %x %x\n", psHu32(0x2000), psHu32(0x2010), psHu32(0x2020), psHu32(0x2030), g_BP.BP, g_BP.bufferhasnew, g_BP.FP, g_BP.IFC);
	__Log("gif: %x %x %x\n", psHu32(0x3000), psHu32(0x3010), psHu32(0x3020));
	for(i = 0; i < ARRAYSIZE(dmacs); ++i) {
		DMACh* p = (DMACh*)(PS2MEM_HW+dmacs[i]);
		__Log("dma%d c%x m%x q%x t%x s%x\n", i, p->chcr, p->madr, p->qwc, p->tadr, p->sadr);
	}
	__Log("dmac %x %x %x %x\n", psHu32(DMAC_CTRL), psHu32(DMAC_STAT), psHu32(DMAC_RBSR), psHu32(DMAC_RBOR));
	__Log("intc %x %x\n", psHu32(INTC_STAT), psHu32(INTC_MASK));
	__Log("sif: %x %x %x %x %x\n", psHu32(0xf200), psHu32(0xf220), psHu32(0xf230), psHu32(0xf240), psHu32(0xf260));
#endif
}

extern u32 psxdump;

static void printfn()
{
	static int lastrec = 0;
	static int curcount = 0;
	const int skip = 0;

	assert( !g_globalMMXSaved );
	assert( !g_globalXMMSaved );

    if( (dumplog&2) && g_lastpc != 0x81fc0 ) {//&& lastrec != g_lastpc ) {
		curcount++;

		if( curcount > skip ) {
			iDumpRegisters(g_lastpc, 1);
			curcount = 0;
		}

		lastrec = g_lastpc;
	}
}

u32 s_recblocks[] = {0};

void badespfn() {
	assert(0);
	SysPrintf("Bad esp!\n");
}

#define OPTIMIZE_COP2 0//CHECK_VU0REC

void __fastcall dyna_block_discard(u32 start,u32 sz)
{
#ifdef _MSC_VER
	__asm push ebp;
#else
	__asm__("push %ebp\n");
#endif
	SysPrintf("dyna_block_discard %08X , count %d\n",start,sz);
	Cpu->Clear(start,sz);
#ifdef _MSC_VER
	__asm pop ebp;
#else
	__asm__("pop %ebp\n");
#endif
	return;
}
void recRecompile( u32 startpc )
{
	u32 i = 0;
	u32 branchTo;
	u32 willbranch3 = 0;
	u32* ptr;
	u32 usecop2;

#ifdef _DEBUG
    //dumplog |= 4;
    if( dumplog & 4 )
		iDumpRegisters(startpc, 0);	
#endif

	assert( startpc );

	// if recPtr reached the mem limit reset whole mem
	if ( ( (uptr)recPtr - (uptr)recMem ) >= REC_CACHEMEM-0x40000 || dumplog == 0xffffffff) {
		recReset();
	}
	if ( ( (uptr)recStackPtr - (uptr)recStack ) >= RECSTACK_SIZE-0x100 ) {
#ifdef _DEBUG
		SysPrintf("stack reset\n");
#endif
		recReset();
	}

	s_pCurBlock = PC_GETBLOCK(startpc);
	
	if( s_pCurBlock->pFnptr ) {
		// clear if already taken
		assert( s_pCurBlock->startpc < startpc );
		recClearMem(s_pCurBlock);	
	}

	if( s_pCurBlock->startpc == startpc ) {
		s_pCurBlockEx = PC_GETBLOCKEX(s_pCurBlock);
		assert( s_pCurBlockEx->startpc == startpc );
	}
	else {
		s_pCurBlockEx = NULL;
		for(i = 0; i < EE_NUMBLOCKS; ++i) {
			if( recBlocks[(i+s_nNextBlock)%EE_NUMBLOCKS].size == 0 ) {
				s_pCurBlockEx = recBlocks+(i+s_nNextBlock)%EE_NUMBLOCKS;
				s_nNextBlock = (i+s_nNextBlock+1)%EE_NUMBLOCKS;
				break;
			}
		}

		if( s_pCurBlockEx == NULL ) {
			//SysPrintf("ee reset (blocks)\n");
			recReset();
			s_nNextBlock = 0;
			s_pCurBlockEx = recBlocks;
		}

		s_pCurBlockEx->startpc = startpc;
	}

	x86SetPtr( recPtr );
	x86Align(16);
	recPtr = x86Ptr;
	s_pCurBlock->pFnptr = (u32)x86Ptr;
	s_pCurBlock->startpc = startpc;

	// slower
//	if( startpc == 0x81fc0 ) {
//		
//		MOV32MtoR(ECX, (u32)&g_nextBranchCycle);
//		MOV32RtoM((u32)&cpuRegs.cycle, ECX);
//		//ADD32ItoR(ECX, 9);
//		//ADD32ItoM((u32)&cpuRegs.cycle, 512);
//		CALLFunc((uptr)cpuBranchTest);
//		CMP32ItoM((u32)&cpuRegs.pc, 0x81fc0);
//		JE8(s_pCurBlock->pFnptr - (u32)(x86Ptr+2) );
//		JMP32((u32)DispatcherReg - (u32)(x86Ptr+5));
//
//		pc = startpc + 9*4;
//		assert( (pc-startpc)>>2 <= 0xffff );
//		s_pCurBlockEx->size = (pc-startpc)>>2;
//
//		for(i = 1; i < (u32)s_pCurBlockEx->size-1; ++i) {
//			s_pCurBlock[i].pFnptr = s_pCurBlock->pFnptr;
//			s_pCurBlock[i].startpc = s_pCurBlock->startpc;
//		}
//
//		// don't overwrite if delay slot
//		if( i < (u32)s_pCurBlockEx->size && !(s_pCurBlock[i].uType & BLOCKTYPE_DELAYSLOT) ) {
//			s_pCurBlock[i].pFnptr = s_pCurBlock->pFnptr;
//			s_pCurBlock[i].startpc = s_pCurBlock->startpc;
//		}
//
//		// set the block ptr
//		AddBaseBlockEx(s_pCurBlockEx, 0);
//
//		if( !(pc&0x10000000) )
//			maxrecmem = max( (pc&~0xa0000000), maxrecmem );
//
//		recPtr = x86Ptr;
//		return;
//	}

	branch = 0;

	// reset recomp state variables
	s_nBlockCycles = 0;
	pc = startpc;
	x86FpuState = FPU_STATE;
	iCWstate = 0;
	s_saveConstGPRreg = 0;
	g_cpuHasConstReg = g_cpuFlushedConstReg = 1;
	g_cpuPrevRegHasLive1 = g_cpuRegHasLive1 = 0xffffffff;
	g_cpuPrevRegHasSignExt = g_cpuRegHasSignExt = 0;
	_recClearWritebacks();
	assert( g_cpuConstRegs[0].UD[0] == 0 );

	_initX86regs();
	_initXMMregs();
	_initMMXregs();

#ifdef _DEBUG
	// for debugging purposes
	MOV32ItoM((uptr)&g_lastpc, pc);
	CALLFunc((uptr)printfn);

//	CMP32MtoR(EBP, (uptr)&s_uSaveEBP);
//	j8Ptr[0] = JE8(0);
//	CALLFunc((uptr)badespfn);
//	x86SetJ8(j8Ptr[0]);
#endif

	// go until the next branch
	i = startpc;
	s_nEndBlock = 0xffffffff;
	s_nHasDelay = 0;
	
	while(1) {
		BASEBLOCK* pblock = PC_GETBLOCK(i);
		if( pblock->pFnptr != 0 && pblock->startpc != s_pCurBlock->startpc ) {

			if( i == pblock->startpc ) {
				// branch = 3
				willbranch3 = 1;
				s_nEndBlock = i;
				break;
			}
		}
		//HUH ? PSM ? whut ? THIS IS VIRTUAL ACCESS GOD DAMMIT
		cpuRegs.code = *(int *)PSM(i);

		switch(cpuRegs.code >> 26) {
			case 0: // special

				if( _Funct_ == 8 || _Funct_ == 9 ) { // JR, JALR
					s_nEndBlock = i + 8;
					s_nHasDelay = 1;
					goto StartRecomp;
				}

				break;
			case 1: // regimm
				
				if( _Rt_ < 4 || (_Rt_ >= 16 && _Rt_ < 20) ) {
					// branches
					if( _Rt_ == 2 || _Rt_ == 3 || _Rt_ == 18 || _Rt_ == 19 ) s_nHasDelay = 1;
					else s_nHasDelay = 2;

					branchTo = _Imm_ * 4 + i + 4;
					if( branchTo > startpc && branchTo < i ) s_nEndBlock = branchTo;
					else  s_nEndBlock = i+8;

					goto StartRecomp;
				}

				break;

			case 2: // J
			case 3: // JAL
				s_nHasDelay = 1;
				s_nEndBlock = i + 8;
				goto StartRecomp;

			// branches
			case 4: case 5: case 6: case 7: 
			case 20: case 21: case 22: case 23:

				if( (cpuRegs.code >> 26) >= 20 ) s_nHasDelay = 1;
				else s_nHasDelay = 2;

				branchTo = _Imm_ * 4 + i + 4;
				if( branchTo > startpc && branchTo < i ) s_nEndBlock = branchTo;
				else  s_nEndBlock = i+8;
				
				goto StartRecomp;

			case 16: // cp0
				if( _Rs_ == 16 ) {
					if( _Funct_ == 24 ) { // eret
						s_nEndBlock = i+4;
						goto StartRecomp;
					}
				}

				break;
			case 17: // cp1
			case 18: // cp2
				if( _Rs_ == 8 ) {
					// BC1F, BC1T, BC1FL, BC1TL
					// BC2F, BC2T, BC2FL, BC2TL
					if( _Rt_ >= 2 ) s_nHasDelay = 1;
					else s_nHasDelay = 2;

					branchTo = _Imm_ * 4 + i + 4;
					if( branchTo > startpc && branchTo < i ) s_nEndBlock = branchTo;
					else  s_nEndBlock = i+8;
					
					goto StartRecomp;
				}
				break;
		}

		i += 4;
	}

StartRecomp:

	// rec info //
	{
		EEINST* pcur;

		if( s_nInstCacheSize < (s_nEndBlock-startpc)/4+1 ) {
			free(s_pInstCache);
			s_nInstCacheSize = (s_nEndBlock-startpc)/4+10;
			s_pInstCache = (EEINST*)malloc(sizeof(EEINST)*s_nInstCacheSize);
			assert( s_pInstCache != NULL );
		}

		pcur = s_pInstCache + (s_nEndBlock-startpc)/4;
		_recClearInst(pcur);
		pcur->info = 0;

		for(i = s_nEndBlock; i > startpc; i -= 4 ) {
			cpuRegs.code = *(int *)PSM(i-4);
			pcur[-1] = pcur[0];

			BSCPropagate bsc( pcur[-1], pcur[0] );
			bsc.rprop();
			pcur--;
		}
	}

	// analyze instructions //
	{
		usecop2 = 0;
		g_pCurInstInfo = s_pInstCache;

		for(i = startpc; i < s_nEndBlock; i += 4) {
			g_pCurInstInfo++;
			cpuRegs.code = *(u32*)PSM(i);

			// cop2 //
			if( g_pCurInstInfo->info & EEINSTINFO_COP2 ) {

				if( !usecop2 ) {
					// init
					vucycle = 0;
					usecop2 = 1;
				}
				
				VU0.code = cpuRegs.code;
				_vuRegsCOP22( &VU0, &g_pCurInstInfo->vuregs );
				continue;
			}

			// fixme - This should be based on the cycle count of the current EE
			// instruction being analyzed.
			if( usecop2 ) vucycle++;

			// peephole optimizations //
#ifdef PCSX2_VIRTUAL_MEM
			if( i < s_nEndBlock-4 && recompileCodeSafe(i) ) {
				u32 curcode = cpuRegs.code;
				u32 nextcode = *(u32*)PSM(i+4);
				if( _eeIsLoadStoreCoIssue(curcode, nextcode) && EE::Dynarec::recBSC_co[curcode>>26] != NULL ) {

					// rs has to be the same, and cannot be just written
					if( ((curcode >> 21) & 0x1F) == ((nextcode >> 21) & 0x1F) && !_eeLoadWritesRs(curcode) ) {

						if( _eeIsLoadStoreCoX(curcode) && ((nextcode>>16)&0x1f) != ((curcode>>21)&0x1f) ) {
							// see how many stores there are
							u32 j;
							// use xmmregs since only supporting lwc1,lq,swc1,sq
							for(j = i+8; j < s_nEndBlock && j < i+4*XMMREGS; j += 4 ) {
								u32 nncode = *(u32*)PSM(j);
								if( (nncode>>26) != (curcode>>26) || ((curcode>>21)&0x1f) != ((nncode>>21)&0x1f) ||
									_eeLoadWritesRs(nncode))
									break;
							}

							if( j > i+8 ) {
								u32 num = (j-i)>>2; // number of stores that can coissue
								assert( num <= XMMREGS );

								g_pCurInstInfo[0].numpeeps = num-1;
								g_pCurInstInfo[0].info |= EEINSTINFO_COREC;

								while(i < j-4) {
									g_pCurInstInfo++;
									g_pCurInstInfo[0].info |= EEINSTINFO_NOREC;
									i += 4;	
								}

								continue;
							}

							// fall through
						}

						// unaligned loadstores

						// if LWL, check if LWR and that offsets are +3 away
						switch(curcode >> 26) {
							case 0x22: // LWL
								if( (nextcode>>26) != 0x26 || ((s16)nextcode)+3 != (s16)curcode )
									continue;
								break;
							case 0x26: // LWR
								if( (nextcode>>26) != 0x22 || ((s16)nextcode) != (s16)curcode+3 )
									continue;
								break;

							case 0x2a: // SWL
								if( (nextcode>>26) != 0x2e || ((s16)nextcode)+3 != (s16)curcode )
									continue;
								break;
							case 0x2e: // SWR
								if( (nextcode>>26) != 0x2a || ((s16)nextcode) != (s16)curcode+3 )
									continue;
								break;

							case 0x1a: // LDL
								if( (nextcode>>26) != 0x1b || ((s16)nextcode)+7 != (s16)curcode )
									continue;
								break;
							case 0x1b: // LWR
								if( (nextcode>>26) != 0x1aa || ((s16)nextcode) != (s16)curcode+7 )
									continue;
								break;

							case 0x2c: // SWL
								if( (nextcode>>26) != 0x2d || ((s16)nextcode)+7 != (s16)curcode )
									continue;
								break;
							case 0x2d: // SWR
								if( (nextcode>>26) != 0x2c || ((s16)nextcode) != (s16)curcode+7 )
									continue;
								break;
						}
						
						// good enough
						g_pCurInstInfo[0].info |= EEINSTINFO_COREC;
						g_pCurInstInfo[0].numpeeps = 1;
						g_pCurInstInfo[1].info |= EEINSTINFO_NOREC;
						g_pCurInstInfo++;
						i += 4;
						continue;
					}
				}
			}
#endif // end peephole
		}
		// This *is* important because g_pCurInstInfo is checked a bit later on and
		// if it's not equal to s_pInstCache it handles recompilation differently.
		// ... but the empty if() conditional inside the for loop is still amusing. >_<
		if( usecop2 ) {
			// add necessary mac writebacks
			g_pCurInstInfo = s_pInstCache;

			for(i = startpc; i < s_nEndBlock-4; i += 4) {
				g_pCurInstInfo++;

				if( g_pCurInstInfo->info & EEINSTINFO_COP2 ) {
				}
			}
		}
	}

	// perf counters //
#ifdef PCSX2_DEVBUILD
//	s_startcount = 0;
//	if( pc+32 < s_nEndBlock ) {
//		// only blocks with more than 8 insts
//		//PUSH32I((uptr)&lbase);
//		//CALLFunc((uptr)QueryPerformanceCounter);
//		lbase.QuadPart = GetCPUTick();
//		s_startcount = 1;
//	}
#endif

#ifdef _DEBUG
	// dump code
	for(i = 0; i < ARRAYSIZE(s_recblocks); ++i) {
		if( startpc == s_recblocks[i] ) {
			iDumpBlock(startpc, recPtr);
		}
	}

	if( (dumplog & 1) ) //|| usecop2 )
		iDumpBlock(startpc, recPtr);
#endif

	u32 sz=(s_nEndBlock-startpc)>>2;
#ifdef lulz
	/*
		Block checking (ADDED BY RAZ-TEMP)
	*/
	
	MOV32ItoR(ECX,startpc);
	MOV32ItoR(EDX,sz);

#endif

	u32 inpage_offs=startpc&0xFFF;
	u32 inpage_ptr=startpc;
	u32 inpage_sz=sz*4;

	MOV32ItoR(ECX,startpc);
	MOV32ItoR(EDX,sz);

#ifndef PCSX2_VIRTUAL_MEM
	while(inpage_sz)
	{
		int PageType=mmap_GetRamPageInfo((u32*)PSM(inpage_ptr));
		u32 pgsz=std::min(0x1000-inpage_offs,inpage_sz);

		if(PageType!=-1)
		{
			if (PageType==0)
			{
				//MOV32ItoR(EAX,*pageVer);
				//CMP32MtoR(EAX,(uptr)pageVer);
				//JNE32(((u32)dyna_block_discard_recmem)- ( (u32)x86Ptr + 6 ));

				mmap_MarkCountedRamPage(PSM(inpage_ptr),inpage_ptr&~0xFFF);
			}
			else
			{
				u32 lpc=inpage_ptr;
				u32 stg=pgsz;
				while(stg>0)
				{
					CMP32ItoM((uptr)PSM(lpc),*(u32*)PSM(lpc));
					// fixme: was dyna_block_discard_recmem .. but umm.. why?
					JNE32(((u32)&dyna_block_discard)- ( (u32)x86Ptr + 6 ));

					stg-=4;
					lpc+=4;
				}
				DbgCon::MsgLn("Manual block @ %08X : %08X %d %d %d %d",
					startpc,inpage_ptr,pgsz,0x1000-inpage_offs,inpage_sz,sz*4);
			}
		}
		inpage_ptr+=pgsz;
		inpage_sz-=pgsz;
		inpage_offs=inpage_ptr&0xFFF;
	}
#endif

	// finally recompile //
	g_pCurInstInfo = s_pInstCache;
	while (!branch && pc < s_nEndBlock) {
		recompileNextInstruction(0);
	}

#ifdef _DEBUG
	if( (dumplog & 1) )
		iDumpBlock(startpc, recPtr);
#endif

	assert( (pc-startpc)>>2 <= 0xffff );
	s_pCurBlockEx->size = (pc-startpc)>>2;

	for(i = 1; i < (u32)s_pCurBlockEx->size-1; ++i) {
		s_pCurBlock[i].pFnptr = s_pCurBlock->pFnptr;
		s_pCurBlock[i].startpc = s_pCurBlock->startpc;
	}

	// don't overwrite if delay slot
	if( i < (u32)s_pCurBlockEx->size && !(s_pCurBlock[i].uType & BLOCKTYPE_DELAYSLOT) ) {
		s_pCurBlock[i].pFnptr = s_pCurBlock->pFnptr;
		s_pCurBlock[i].startpc = s_pCurBlock->startpc;
	}

	// set the block ptr
	AddBaseBlockEx(s_pCurBlockEx, 0);
//	if( p[1].startpc == p[0].startpc + 4 ) {
//		assert( p[1].pFnptr != 0 );
//		// already fn in place, so add to list
//		AddBaseBlockEx(s_pCurBlockEx, 0);
//	}
//	else
//		*(BASEBLOCKEX**)(p+1) = pex;
//	}

	//PC_SETBLOCKEX(s_pCurBlock, s_pCurBlockEx);

	if( !(pc&0x10000000) )
		maxrecmem = std::max( (pc&~0xa0000000), maxrecmem );

	if( branch == 2 ) {
		iFlushCall(FLUSH_EVERYTHING);

		iBranchTest(0xffffffff, 1);	
		if( bExecBIOS ) CheckForBIOSEnd();

		JMP32((uptr)DispatcherReg - ( (uptr)x86Ptr + 5 ));
	}
	else {
		assert( branch != 3 );
		if( branch ) assert( !willbranch3 );
		else ADD32ItoM((int)&cpuRegs.cycle, eeScaleBlockCycles() );

		if( willbranch3 ) {
			BASEBLOCK* pblock = PC_GETBLOCK(s_nEndBlock);
			assert( pc == s_nEndBlock );
			iFlushCall(FLUSH_EVERYTHING);
			MOV32ItoM((uptr)&cpuRegs.pc, pc);
			JMP32((uptr)pblock->pFnptr - ((uptr)x86Ptr + 5));
			branch = 3;
		}
		else if( !branch ) {
			// didn't branch, but had to stop
			MOV32ItoM( (uptr)&cpuRegs.pc, pc );

			iFlushCall(FLUSH_EVERYTHING);

			ptr = JMP32(0);
		}
	}

	assert( x86Ptr >= (u8*)s_pCurBlock->pFnptr + EE_MIN_BLOCK_BYTES );
	assert( x86Ptr < recMem+REC_CACHEMEM );
	assert( recStackPtr < recStack+RECSTACK_SIZE );
	assert( x86FpuState == 0 );

	recPtr = x86Ptr;

	assert( (g_cpuHasConstReg&g_cpuFlushedConstReg) == g_cpuHasConstReg );

	if( !branch ) {
		BASEBLOCK* pcurblock = s_pCurBlock;
		u32 nEndBlock = s_nEndBlock;
		s_pCurBlock = PC_GETBLOCK(pc);
		assert( ptr != NULL );
		
		if( s_pCurBlock->startpc != pc ) 
 			recRecompile(pc);

		if( pcurblock->startpc == startpc ) {
			assert( pcurblock->pFnptr );
			assert( s_pCurBlock->startpc == nEndBlock );
			*ptr = s_pCurBlock->pFnptr - ( (u32)ptr + 4 );
		}
		else {
			recRecompile(startpc);
			assert( pcurblock->pFnptr != 0 );
		}
	}
}

R5900cpu recCpu = {
	recInit,
	recReset,
	recStep,
	recExecute,
	recExecuteBlock,
	recExecuteVU0Block,
	recExecuteVU1Block,
	recEnableVU0micro,
	recEnableVU1micro,
	recClear,
	recClearVU0,
	recClearVU1,
	recShutdown
};