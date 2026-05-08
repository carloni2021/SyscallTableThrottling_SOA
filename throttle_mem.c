// SPDX-License-Identifier: GPL-2.0
/*
 * throttle_mem.c — Accesso hardware a basso livello
 *
 * sys_vtpmo: page-table walk via CR3, basato su lib/vtpmo.c di
 *            Francesco Quaglia https://github.com/FrancescoQuaglia/Linux-sys_call_table-discoverer Percorre PML4→PDP→PDE→PTE.
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

/* Legge CR3 (base della page table del processo corrente) in una variabile C.
 *   "mov %%cr3, %0"  — istruzione x86; %%cr3 è il registro, %0 è l'operando 0
 *   "=r"(v)          — output: scrivi il risultato in un GPR qualsiasi → v
 *   volatile         — impedisce al compilatore di eliminare/spostare l'istruzione */
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

/* Scrive val in CR0 bypassando il CR pinning del kernel.
 *
 * Il kernel espone write_cr0() ma al suo interno controlla che certi bit
 * non vengano modificati (CR pinning): verrebbe bloccato prima di arrivare
 * all'istruzione hardware. Usando asm inline si va direttamente sull'hardware.
 *
 *   "mov %0, %%cr0"   — scrive il valore di %0 nel registro CR0
 *   "+r"(val)         — input+output: val viene letto dal compilatore in un GPR
 *   "+m"(__force_order) — operando memoria fittizio: crea una dipendenza che
 *                         impedisce al compilatore di riordinare questa istruzione
 *                         rispetto agli accessi alla SCT (stesso trucco usato da
 *                         native_write_cr0 nei kernel senza CR pinning)
 *
 * CR0  — gestisce protezioni fondamentali: paging (PG), write-protect (WP), ecc.
 * CR4  — gestisce estensioni: CET (shadow stack / IBT), SMEP, SMAP, ecc. */
inline void write_cr0_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}

#ifdef X86_CR4_CET
/* Identico a write_cr0_forced ma per CR4 — necessario per disabilitare CET
 * prima di scrivere su memoria eseguibile (x64_sys_call / stub). */
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
