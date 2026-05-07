// SPDX-License-Identifier: GPL-2.0
/*
 * throttle_mem.c — Accesso hardware a basso livello
 *
 * sys_vtpmo: page-table walk via CR3, basato su lib/vtpmo.c di
 *            Francesco Quaglia (GPL-2.0). Percorre PML4→PDP→PDE→PTE.
 *
 * begin/end_syscall_table_hack: disabilita CR0.WP (e CR4.CET se presente)
 *   per consentire la scrittura sulla sys_call_table in memoria read-only.
 *   Usa istruzioni inline dirette per aggirare il CR pinning del kernel.
 */

#include "throttle.h"

/* 
 *  VTPMO — Page-table walk via CR3 scanning della memoria virtuale
 *  per identificare sys_call_table senza simboli esportati.
 */

// Maschera per estrarre l'indirizzo fisico da un entry di pagina
#define PT_ADDR_MASK 0x7ffffffffffff000ULL
#define PT_VALID     0x1UL
#define PT_LARGE     0x80UL

#define PML4_IDX(a) (((u64)(a) >> 39) & 0x1ff)
#define PDP_IDX(a)  (((u64)(a) >> 30) & 0x1ff)
#define PDE_IDX(a)  (((u64)(a) >> 21) & 0x1ff)
#define PTE_IDX(a)  (((u64)(a) >> 12) & 0x1ff)

// Restituisce il numero di pagina fisica mappata a vaddr, o NO_MAP se non mappata
static inline unsigned long vtpmo_read_cr3(void)
{
    unsigned long v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

int sys_vtpmo(unsigned long vaddr)
{
    unsigned long *pml4, *pdp, *pde, *pte;
    unsigned long e;

    pml4 = (unsigned long *)phys_to_virt(vtpmo_read_cr3() & PT_PHYS_MASK);

    e = pml4[PML4_IDX(vaddr)];
    if (!(e & PT_VALID)) return NO_MAP;

    pdp = (unsigned long *)__va(e & PT_ADDR_MASK);
    e   = pdp[PDP_IDX(vaddr)];
    if (!(e & PT_VALID)) return NO_MAP;
    if (e & PT_LARGE)    return (int)((e & PT_ADDR_MASK) >> 12);

    pde = (unsigned long *)__va(e & PT_ADDR_MASK);
    e   = pde[PDE_IDX(vaddr)];
    if (!(e & PT_VALID)) return NO_MAP;
    if (e & PT_LARGE)    return (int)((e & PT_ADDR_MASK) >> 12);

    pte = (unsigned long *)__va(e & PT_ADDR_MASK);
    e   = pte[PTE_IDX(vaddr)];
    if (!(e & PT_VALID)) return NO_MAP;

    return (int)((e & PT_ADDR_MASK) >> 12);
}

/* 
 *  CR0.WP — disabilita write-protect bypassando il CR pinning
 */

static unsigned long saved_cr0;
static unsigned long saved_cr4;

//Scrittura diretta su CR0 e CR4 usando istruzioni inline per aggirare il CR pinning del kernel
/*definizione: cr pinning è una tecnica usata dal kernel per impedire modifiche non autorizzate ai registri di controllo CR0
 e CR4, che potrebbe essere usata da rootkit o moduli malintenzionati per bypassare le protezioni del kernel. 
 Il kernel impone restrizioni sulla modifica di questi registri, rendendo difficile disabilitare la protezione in 
 scrittura (CR0.WP) necessaria per patchare la sys_call_table. Usando istruzioni inline dirette, il modulo può modificare 
 temporaneamente questi registri senza essere bloccato dal CR pinning, consentendo l'hook delle syscall anche su kernel 
 moderni con protezioni avanzate.*/
inline void write_cr0_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}
/*differenza tra cr4 e cr0 : 
* cr0 è un registro di controllo che gestisce le operazioni di base del processore, come la protezione della memoria, il paging e le interruzioni.
* cr4 è un registro di controllo che gestisce funzionalità avanzate del processore (gestisce anche la protezione per ROP)
*/
#ifdef X86_CR4_CET
static inline void write_cr4_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr4" : "+r"(val), "+m"(__force_order));
}
#endif
//Funzioni per disabilitare e ripristinare la protezione in scrittura (CR0.WP) e CET (CR4.CET se presente) per patchare la sys_call_table
void begin_syscall_table_hack(void)
{
    preempt_disable();
    saved_cr0 = read_cr0();
    saved_cr4 = native_read_cr4();
#ifdef X86_CR4_CET
    if (saved_cr4 & X86_CR4_CET)
        write_cr4_forced(saved_cr4 & ~X86_CR4_CET);
#endif
    write_cr0_forced(saved_cr0 & ~X86_CR0_WP);
}
//Ripristina i valori originali di CR0 e CR4 dopo aver patchato la sys_call_table
void end_syscall_table_hack(void)
{
    //ripristina i valori di cr0 e cr4 ai valori pre-patching 
    //preempt_enable viene chiamato alla fine di end_syscall_table_hack per consentire nuovamente il preemption del kernel dopo aver completato le modifiche alla sys_call_table, garantendo così la stabilità del sistema.
    write_cr0_forced(saved_cr0);
#ifdef X86_CR4_CET
    if (saved_cr4 & X86_CR4_CET)
        write_cr4_forced(saved_cr4);
#endif
    preempt_enable();
}
