/*
 * WoW64 CPU support
 *
 * Copyright 2021 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/asm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wow);

#include "pshpack1.h"
struct thunk_32to64
{
    BYTE  lcall;   /* call far, absolute indirect */
    BYTE  modrm;  /* address=disp32, opcode=3 */
    DWORD op;
    DWORD lcall_addr;
    WORD  cs;

    BYTE add;
    BYTE add_modrm;
    BYTE add_op;

    BYTE jmp;
    BYTE jmp_modrm;
    DWORD jmp_op;
    DWORD jmp_addr;
};
#include "poppack.h"

static BYTE DECLSPEC_ALIGN(4096) code_buffer[0x1000];

static USHORT cs64_sel;
static USHORT ds64_sel;
static USHORT fs32_sel;

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH) LdrDisableThreadCalloutsForDll( inst );
    return TRUE;
}

/***********************************************************************
 *           fpux_to_fpu
 *
 * Build a standard i386 FPU context from an extended one.
 */
static void fpux_to_fpu( I386_FLOATING_SAVE_AREA *fpu, const XMM_SAVE_AREA32 *fpux )
{
    unsigned int i, tag, stack_top;

    fpu->ControlWord   = fpux->ControlWord;
    fpu->StatusWord    = fpux->StatusWord;
    fpu->ErrorOffset   = fpux->ErrorOffset;
    fpu->ErrorSelector = fpux->ErrorSelector | (fpux->ErrorOpcode << 16);
    fpu->DataOffset    = fpux->DataOffset;
    fpu->DataSelector  = fpux->DataSelector;
    fpu->Cr0NpxState   = fpux->StatusWord | 0xffff0000;

    stack_top = (fpux->StatusWord >> 11) & 7;
    fpu->TagWord = 0xffff0000;
    for (i = 0; i < 8; i++)
    {
        memcpy( &fpu->RegisterArea[10 * i], &fpux->FloatRegisters[i], 10 );
        if (!(fpux->TagWord & (1 << i))) tag = 3;  /* empty */
        else
        {
            const M128A *reg = &fpux->FloatRegisters[(i - stack_top) & 7];
            if ((reg->High & 0x7fff) == 0x7fff)  /* exponent all ones */
            {
                tag = 2;  /* special */
            }
            else if (!(reg->High & 0x7fff))  /* exponent all zeroes */
            {
                if (reg->Low) tag = 2;  /* special */
                else tag = 1;  /* zero */
            }
            else
            {
                if (reg->Low >> 63) tag = 0;  /* valid */
                else tag = 2;  /* special */
            }
        }
        fpu->TagWord |= tag << (2 * i);
    }
}

/**********************************************************************
 *           copy_context_64to32
 *
 * Copy a 64-bit context corresponding to an exception happening in 32-bit mode
 * into the corresponding 32-bit context.
 */
static void copy_context_64to32( I386_CONTEXT *ctx32, DWORD flags, AMD64_CONTEXT *ctx64 )
{
    ctx32->ContextFlags = flags;
    flags &= ~CONTEXT_i386;
    if (flags & CONTEXT_I386_INTEGER)
    {
        ctx32->Eax = ctx64->Rax;
        ctx32->Ebx = ctx64->Rbx;
        ctx32->Ecx = ctx64->Rcx;
        ctx32->Edx = ctx64->Rdx;
        ctx32->Esi = ctx64->Rsi;
        ctx32->Edi = ctx64->Rdi;
    }
    if (flags & CONTEXT_I386_CONTROL)
    {
        ctx32->Esp    = ctx64->Rsp;
        ctx32->Ebp    = ctx64->Rbp;
        ctx32->Eip    = ctx64->Rip;
        ctx32->EFlags = ctx64->EFlags;
        ctx32->SegCs  = ctx64->SegCs;
        ctx32->SegSs  = ds64_sel;
    }
    if (flags & CONTEXT_I386_SEGMENTS)
    {
        ctx32->SegDs = ds64_sel;
        ctx32->SegEs = ds64_sel;
        ctx32->SegFs = fs32_sel;
        ctx32->SegGs = ds64_sel;
    }
    if (flags & CONTEXT_I386_DEBUG_REGISTERS)
    {
        ctx32->Dr0 = ctx64->Dr0;
        ctx32->Dr1 = ctx64->Dr1;
        ctx32->Dr2 = ctx64->Dr2;
        ctx32->Dr3 = ctx64->Dr3;
        ctx32->Dr6 = ctx64->Dr6;
        ctx32->Dr7 = ctx64->Dr7;
    }
    if (flags & CONTEXT_I386_FLOATING_POINT)
    {
        fpux_to_fpu( &ctx32->FloatSave, &ctx64->FltSave );
    }
    if (flags & CONTEXT_I386_EXTENDED_REGISTERS)
    {
        *(XSAVE_FORMAT *)ctx32->ExtendedRegisters = ctx64->FltSave;
    }
    /* FIXME: xstate */
}


/**********************************************************************
 *           syscall_32to64
 *
 * Execute a 64-bit syscall from 32-bit code, then return to 32-bit.
 */
extern void WINAPI syscall_32to64(void) DECLSPEC_HIDDEN;
__ASM_GLOBAL_FUNC( syscall_32to64,
                   /* cf. BTCpuSimulate prolog */
                   __ASM_SEH(".seh_stackalloc 0x28\n\t")
                   __ASM_SEH(".seh_endprologue\n\t")
                   __ASM_CFI(".cfi_adjust_cfa_offset 0x28\n\t")
                   "pushq %r12\n\t"
                   "movq %gs:0x30,%r12\n\t"
                   "movq %r13,0x14a0(%r12)\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R13_32BIT] */
                   "movq %r14,0x14b0(%r12)\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R14_32BIT] */
                   "movq 0x1488(%r12),%r13\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED] */
                   "leaq 4(%r13),%r13\n"        /* cpu->Context */
                   "movq 0x1490(%r12),%r14\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R14] */
                   "popq %r12\n\t"

                   "xchgq %r14,%rsp\n\t"
                   "movl %edi,0x9c(%r13)\n\t"   /* context->Edi */
                   "movl %esi,0xa0(%r13)\n\t"   /* context->Esi */
                   "movl %ebx,0xa4(%r13)\n\t"   /* context->Ebx */
                   "movl %ebp,0xb4(%r13)\n\t"   /* context->Ebp */
                   "movl (%r14),%edx\n\t"
                   "movl %edx,0xb8(%r13)\n\t"   /* context->Eip */
                   "pushfq\n\t"
                   "popq %rdx\n\t"
                   "movl %edx,0xc0(%r13)\n\t"   /* context->EFlags */
                   "leaq 4(%r14),%rdx\n\t"
                   "movl %edx,0xc4(%r13)\n\t"   /* context->Esp */
                   "movq %rax,%rcx\n\t"         /* syscall number */
                   "leaq 8(%r14),%rdx\n\t"      /* parameters */
                   "call " __ASM_NAME("Wow64SystemServiceEx") "\n\t"

                   "syscall_32to64_return:\n\t"
                   "movl 0x9c(%r13),%edi\n\t"   /* context->Edi */
                   "movl 0xa0(%r13),%esi\n\t"   /* context->Esi */
                   "movl 0xa4(%r13),%ebx\n\t"   /* context->Ebx */
                   "movl 0xb4(%r13),%ebp\n\t"   /* context->Ebp */
                   "btrl $0,-4(%r13)\n\t"       /* cpu->Flags & WOW64_CPURESERVED_FLAG_RESET_STATE */
                   "jc 1f\n\t"
                   "movl 0xb8(%r13),%edx\n\t"   /* context->Eip */
                   "movl %edx,(%rsp)\n\t"
                   "movl 0xbc(%r13),%edx\n\t"   /* context->SegCs */
                   "movl %edx,4(%rsp)\n\t"
                   "movl 0xc4(%r13),%r14d\n\t"  /* context->Esp */
                   "xchgq %r14,%rsp\n\t"

                   "pushq 0(%r14)\n\t"
                   "pushq %r12\n\t"
                   "movq %gs:0x30,%r12\n\t"
                   "movq 0x14a0(%r12),%r13\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R13_32BIT] */
                   "movq %r14,0x1490(%r12)\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R14] */
                   "movq 0x14b0(%r12),%r14\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R14_32BIT] */
                   "popq %r12\n\t"

                   "lret\n"
                   "1:\tmovq %rsp,%r14\n\t"
                   "movl 0xa8(%r13),%edx\n\t"   /* context->Edx */
                   "movl 0xac(%r13),%ecx\n\t"   /* context->Ecx */
                   "movl 0xc8(%r13),%eax\n\t"   /* context->SegSs */
                   "movq %rax,0x20(%rsp)\n\t"
                   "mov %ax,%ds\n\t"
                   "mov %ax,%es\n\t"
                   "mov 0x90(%r13),%fs\n\t"     /* context->SegFs */
                   "movl 0xc4(%r13),%eax\n\t"   /* context->Esp */
                   "movq %rax,0x18(%rsp)\n\t"
                   "movl 0xc0(%r13),%eax\n\t"   /* context->EFlags */
                   "movq %rax,0x10(%rsp)\n\t"
                   "movl 0xbc(%r13),%eax\n\t"   /* context->SegCs */
                   "movq %rax,0x8(%rsp)\n\t"
                   "movl 0xb8(%r13),%eax\n\t"   /* context->Eip */
                   "movq %rax,(%rsp)\n\t"
                   "movl 0xb0(%r13),%eax\n\t"   /* context->Eax */

                   "pushq %r12\n\t"
                   "movq %gs:0x30,%r12\n\t"
                   "movq 0x14a0(%r12),%r13\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R13_32BIT] */
                   "movq %r14,0x1490(%r12)\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R14] */
                   "movq 0x14b0(%r12),%r14\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_WINEHYBRID_RESERVED_R14_32BIT] */
                   "popq %r12\n\t"

                   "iretq" )


/**********************************************************************
 *           BTCpuSimulate  (wow64cpu.@)
 */
__ASM_STDCALL_FUNC( BTCpuSimulate, 0,
                    "subq $0x28,%rsp\n"
                   __ASM_SEH(".seh_stackalloc 0x28\n\t")
                   __ASM_SEH(".seh_endprologue\n\t")
                   __ASM_CFI(".cfi_adjust_cfa_offset 0x28\n\t")
                    "movq %gs:0x30,%r12\n\t"
                    "movq 0x1488(%r12),%rcx\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED] */
                    "leaq 4(%rcx),%r13\n"        /* cpu->Context */
                    "jmp syscall_32to64_return\n" )


/**********************************************************************
 *           BTCpuProcessInit  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuProcessInit(void)
{
    struct thunk_32to64 *thunk = (struct thunk_32to64 *)code_buffer;
    SIZE_T size = sizeof(*thunk);
    ULONG old_prot;
    CONTEXT context;

    if ((ULONG_PTR)syscall_32to64 >> 32)
    {
        ERR( "wow64cpu loaded above 4G, disabling\n" );
        return STATUS_INVALID_ADDRESS;
    }

    RtlCaptureContext( &context );
    cs64_sel = context.SegCs;
    ds64_sel = context.SegDs;
    fs32_sel = context.SegFs;

    /* CW HACK 20760:
     * Use lcall rather than ljmp to work around a Rosetta SIGUSR1 race condition.
     */
    thunk->lcall = 0xff;
    thunk->modrm = 0x1d;
    thunk->op   = PtrToUlong( &thunk->lcall_addr );
    thunk->lcall_addr = PtrToUlong( &thunk->add );
    thunk->cs   = cs64_sel;

    /* We are now in 64-bit. */
    /* add $0x08,%esp to remove the addr/segment pushed on the stack by the lcall */
    thunk->add = 0x83;
    thunk->add_modrm = 0xc4;
    thunk->add_op = 0x08;

    /* jmp to syscall_32to64 */
    thunk->jmp = 0xff;
    thunk->jmp_modrm = 0x25;
    thunk->jmp_op = 0x00;
    thunk->jmp_addr = PtrToUlong( syscall_32to64 );

    NtProtectVirtualMemory( GetCurrentProcess(), (void **)&thunk, &size, PAGE_EXECUTE_READ, &old_prot );
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           BTCpuGetBopCode  (wow64cpu.@)
 */
void * WINAPI BTCpuGetBopCode(void)
{
    return code_buffer;
}


/**********************************************************************
 *           BTCpuGetContext  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuGetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return NtQueryInformationThread( thread, ThreadWow64Context, ctx, sizeof(*ctx), NULL );
}


/**********************************************************************
 *           BTCpuSetContext  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuSetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return NtSetInformationThread( thread, ThreadWow64Context, ctx, sizeof(*ctx) );
}


/**********************************************************************
 *           BTCpuResetToConsistentState  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuResetToConsistentState( EXCEPTION_POINTERS *ptrs )
{
    CONTEXT *context = ptrs->ContextRecord;
    I386_CONTEXT wow_context;

    copy_context_64to32( &wow_context, CONTEXT_I386_ALL, context );
    wow_context.EFlags &= ~(0x100|0x40000);
    BTCpuSetContext( GetCurrentThread(), GetCurrentProcess(), NULL, &wow_context );

    /* fixup context to pretend that we jumped to 64-bit mode */
    context->Rip = (ULONG64)syscall_32to64;
    context->SegCs = cs64_sel;
    context->Rsp = NtCurrentTeb()->TlsSlots[2]; /* WOW64_TLS_WINEHYBRID_RESERVED_R14 */
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           BTCpuTurboThunkControl  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuTurboThunkControl( ULONG enable )
{
    if (enable) return STATUS_NOT_SUPPORTED;
    /* we don't have turbo thunks yet */
    return STATUS_SUCCESS;
}