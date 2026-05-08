// SPDX-License-Identifier: GPL-2.0
/*
 * throttle_discovery.c — Ricerca di sys_call_table e x64_sys_call
 *
 * sys_call_table: scansione del virtual address space [0xffffffff00000000, MAX)
 *   con euristica su 7 indici noti di sys_ni_syscall.
 *   Basato su usctm.c https://github.com/FrancescoQuaglia/Linux-sys_call_table-discoverer del Professor Francesco Quaglia.
 *
 * x64_sys_call (kernel >= 5.15): risolto tramite kprobe su "x64_sys_call",
 *   stesso approccio di Quaglia.
 *
 * patch_x64_sys_call: alloca uno stub eseguibile FUORI dal modulo via
 *   module_alloc(), scrive machine code auto-contenuto, poi installa un
 *   JMP rel32 da x64_sys_call → stub.
 *
 * Stub (auto-contenuto, nessun riferimento a dati del modulo):
 *   Se CONFIG_RETPOLINE: 19 byte con __x86_indirect_thunk_rax (Spectre v2)
 *     49 BA [addr 8B]    movabs r10, <sys_call_table_addr>
 *     49 8B 04 F2        mov    rax, [r10 + rsi*8]
 *     E9 [4B]            jmp    __x86_indirect_thunk_rax
 *   Altrimenti: 14 byte direct jmp
 *     49 BA [addr 8B]    movabs r10, <sys_call_table_addr>
 *     41 FF 24 F2        jmpq   *(%r10, %rsi, 8)
 *
 * Perché lo stub è esterno al modulo:
 *   Durante module unload la memoria del modulo viene rilasciata dopo module_exit().
 *   Lo stub allocato con module_alloc() sopravvive indipendentemente e viene
 *   liberato esplicitamente da restore_x64_sys_call(), chiamata in throttle_exit()
 *   DOPO synchronize_rcu() + wait_event(drain) — quando nessun CPU è più nello stub.
 */

#include "throttle.h"

/* ================================================================
 *  SYS_CALL_TABLE FINDER — scansione [0xffffffff00000000, MAX)
 * ================================================================ */

//indici noti di sys_ni_syscall in sys_call_table, usati per identificare la posizione della tabella
//durante la scansione del virtual address space.
#define SCT_START  0xffffffff00000000ULL
#define SCT_MAX    0xfffffffffff00000ULL
//Questi indici sono stati identificati empiricamente su diverse versioni del kernel
//sono generalmente stabili, ma potrebbero variare in future versioni del kernel.
#define NI_1  134
#define NI_2  174
#define NI_3  182
#define NI_4  183
#define NI_5  214
#define NI_6  215
#define NI_7  236

syscall_fn_t     *sys_call_table_ptr = NULL;
static unsigned long *hacked_ni_syscall  = NULL;

// Verifica se i 7 indici noti di sys_ni_syscall in addr puntano allo stesso valore (e non sono nulli o piccoli), per identificare una potenziale sys_call_table.
static int sct_good_area(unsigned long *addr)
{
    int i;
    for (i = 1; i < NI_1; i++)
        if (addr[i] == addr[NI_1]) return 0;
    return 1;
}

//Controlla se la pagina puntata da addr contiene una potenziale sys_call_table
//verificando i 7 indici noti di sys_ni_syscall e la coerenza dei valori.
//Se trova una potenziale tabella, salva l'indirizzo di sys_ni_syscall e sys_call_table_ptr e restituisce 1; altrimenti restituisce 0.
static int sct_validate_page(unsigned long *addr)
{
    int i;
    unsigned long page = (unsigned long)addr;

    //Scansiona la pagina, con step di sizeof(void *), verificando se la posizione dei 7 indici noti di sys_ni_syscall è coerente
    //(puntano allo stesso valore, che deve essere > 0xffffffff00000000 e non deve essere 0 o piccolo).
    //Se trova una potenziale sys_call_table, salva gli indirizzi e restituisce 1.
    //Se attraversa una pagina non mappata, si ferma per evitare kernel panic.
    for (i = 0; i < (int)PAGE_SIZE; i += (int)sizeof(void *)) {
        unsigned long new_page = page + i + NI_7 * sizeof(void *);

        /* Ferma la scansione se l'entry NI_7 cadrebbe nella pagina successiva
         * e quella pagina non è mappata: evita un page fault durante l'accesso. */
        if (((page + PAGE_SIZE) == (new_page & PT_PHYS_MASK)) &&
            sys_vtpmo(new_page) == NO_MAP)
            break;

        addr = (unsigned long *)(page + i);
        /*
        * Verifica i 7 indici noti di sys_ni_syscall:
        * devono puntare allo stesso valore, che deve essere > 0xffffffff00000000 (alto)
        * e non deve essere 0 o piccolo (non valido).
        */
        if ((addr[NI_1] & 0x3) == 0 &&
             addr[NI_1] != 0 &&
             addr[NI_1] > 0xffffffff00000000ULL &&
             addr[NI_1] == addr[NI_2] &&
             addr[NI_1] == addr[NI_3] &&
             addr[NI_1] == addr[NI_4] &&
             addr[NI_1] == addr[NI_5] &&
             addr[NI_1] == addr[NI_6] &&
             addr[NI_1] == addr[NI_7] &&
             sct_good_area(addr)) {
            //Controllo aggiuntivo in sct_good_area per verificare che i primi n indici non siano uguali a sys_ni_syscall, per evitare falsi positivi.
            //potenziale sys_call_table trovata: salva gli indirizzi di sys_ni_syscall e sys_call_table_ptr, poi restituisce 1.
            hacked_ni_syscall  = (unsigned long *)(addr[NI_1]);
            sys_call_table_ptr = (syscall_fn_t *)addr;
            return 1;
        }
    }
    return 0;
}

// Scansiona il virtual address space da SCT_START a SCT_MAX, pagina per pagina, per trovare sys_call_table tramite sct_validate_page che viene lanciata se la pagina è mappata.
//  Restituisce 0 se trovata, -ENOENT altrimenti.
int find_sys_call_table(void)
{
    unsigned long k;
    for (k = SCT_START; k < SCT_MAX; k += PAGE_SIZE) {
        if (sys_vtpmo(k) != NO_MAP) {
            if (sct_validate_page((unsigned long *)k)) {
                printk(KERN_INFO "<throttle>: sys_call_table @ %px\n",
                       (void *)sys_call_table_ptr);
                printk(KERN_INFO "<throttle>: sys_ni_syscall  @ %px\n",
                       (void *)hacked_ni_syscall);
                return 0;
            }
        }
    }
    printk(KERN_ERR "<throttle>: sys_call_table non trovata\n");
    return -ENOENT;
}

/* ================================================================
 *  X64_SYS_CALL FINDER + PATCH TRAMITE STUB ESTERNO (kernel >= 5.15)
 * ================================================================ */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)

static unsigned long x64_sys_call_addr = 0;
//buffer che contiene i byte originali di x64_sys_call sovrascritti dallo stub, usato per ripristinare la funzione durante unload.
static char x64_orig_bytes[5];
static char x64_jump_inst[5];

static void *x64_stub = NULL;   /* allocato con module_alloc, fuori dal modulo */


#include <linux/kprobes.h>

/* Usa kprobe per risolvere l'indirizzo di un simbolo non esportato.
 * Usata sia per execmem_alloc/free/set_memory_x (>= 6.4) sia per
 * __x86_indirect_thunk_rax (mitigazione Spectre v2, tutti i kernel). */
static unsigned long lookup_unexported(const char *name)
{
    struct kprobe kp = { .symbol_name = name };
    unsigned long addr;
    if (register_kprobe(&kp) < 0) return 0;
    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    return addr;
}

//blocco kernel v 6.4+ : execmem_alloc, execmem_free e set_memory_x non sono più esportate, vengono risolte a runtime via kprobe in resolve_stub_fns().
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
/*
 * execmem_alloc, execmem_free, set_memory_x non sono EXPORT_SYMBOL su >= 6.4:
 * vengono risolti a runtime via kprobe (register+unregister immediato,
 * nessun kprobe resta attivo dopo la risoluzione).
 */
typedef void *(*fn_execmem_alloc_t)(unsigned int type, size_t size);
typedef void  (*fn_execmem_free_t)(void *ptr);
typedef int   (*fn_set_memory_x_t)(unsigned long addr, int numpages);

static fn_execmem_alloc_t fn_execmem_alloc;
static fn_execmem_free_t  fn_execmem_free;
static fn_set_memory_x_t  fn_set_memory_x;

// Risolve gli indirizzi di execmem_alloc, execmem_free e set_memory_x a runtime usando lookup_unexported. Ritorna 0 se fallisce.
static int resolve_stub_fns(void)
{
    fn_execmem_alloc = (fn_execmem_alloc_t)lookup_unexported("execmem_alloc");
    fn_execmem_free  = (fn_execmem_free_t) lookup_unexported("execmem_free");
    fn_set_memory_x  = (fn_set_memory_x_t) lookup_unexported("set_memory_x");
    if (!fn_execmem_alloc || !fn_execmem_free || !fn_set_memory_x) {
        printk(KERN_ERR "<throttle>: risoluzione execmem/set_memory_x fallita\n");
        return -ENOENT;
    }
    printk(KERN_INFO "<throttle>: execmem_alloc=%px execmem_free=%px set_memory_x=%px\n",
           (void *)fn_execmem_alloc, (void *)fn_execmem_free, (void *)fn_set_memory_x);
    return 0;
}

/* EXECMEM_MODULE_TEXT == 0 (primo valore dell'enum execmem_type) */
#  define stub_alloc()          fn_execmem_alloc(0, PAGE_SIZE)
#  define stub_free(p)          fn_execmem_free(p)
#  define stub_set_memory_x(a)  fn_set_memory_x((unsigned long)(a), 1)

// Per kernel < 6.4, usiamo module_alloc, vfree e set_memory_x direttamente.
#else  /* kernel < 6.4 */
#  define stub_alloc()          module_alloc(PAGE_SIZE)
#  define stub_free(p)          vfree(p)
#  define stub_set_memory_x(a)  set_memory_x((unsigned long)(a), 1)
#endif

/* Risolve x64_sys_call tramite kprobe — stesso approccio di Quaglia. */
int find_x64_sys_call(void)
{
    x64_sys_call_addr = lookup_unexported("x64_sys_call");
    if (!x64_sys_call_addr) {
        printk(KERN_ERR "<throttle>: x64_sys_call non trovata\n");
        return -ENOENT;
    }
    printk(KERN_INFO "<throttle>: x64_sys_call @ %px (via kprobe)\n",
           (void *)x64_sys_call_addr);
    return 0;
}

/* ================================================================
 *  PATCH SMP-SAFE via stop_machine
 * ================================================================ */

struct text_patch_data {
    void       *dst;
    const void *src;
    size_t      len;
};

// Funzione eseguita da stop_machine per scrivere il codice del stub in modo SMP-safe ovvero sicuro per multiprocessore,
// Gestendo CR0.WP e garantendo l'atomicità su tutti i CPU.
// Riceve una struct text_patch_data con dst, src e len, disabilita WP, copia i byte, poi ripristina WP e fa una memory barrier.
static int do_text_patch(void *arg)
{
    struct text_patch_data *p = arg;
    unsigned long cr0;

    cr0 = read_cr0();
    write_cr0_forced(cr0 & ~X86_CR0_WP);
    memcpy(p->dst, p->src, p->len);
    write_cr0_forced(cr0);
    mb();
    return 0;
}

/* ================================================================
 *  STUB ESTERNO: dispatcher x64_sys_call → sys_call_table[nr]
 *
 *  Machine code (14 byte, auto-contenuto):
 *    49 BA [addr 8B]    movabs r10, <sys_call_table_addr>
 *    41 FF 24 F2        jmpq   *(%r10, %rsi, 8)
 *
 *  ABI x86-64: rdi=regs, rsi=nr (zero-extended da esi).
 *  Il dispatch via SCT funziona automaticamente con l'hooking SCT:
 *    - syscall hookate  → sys_call_table[nr] = generic_sct_wrapper → throttling
 *    - syscall normali  → sys_call_table[nr] = handler originale diretto
 *  Nessun try_module_get, nessun riferimento a simboli del modulo.
 * ================================================================ */

static int alloc_x64_stub(void)
{
    unsigned long sct_addr = (unsigned long)sys_call_table_ptr;
    unsigned long thunk_addr;
    unsigned char stub_code[19];
    size_t stub_len;
    struct text_patch_data pd;
    int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    ret = resolve_stub_fns();
    if (ret) return ret;
#endif

    x64_stub = stub_alloc();
    if (!x64_stub) {
        printk(KERN_ERR "<throttle>: stub_alloc fallita\n");
        return -ENOMEM;
    }

    /*
     * Costruisce il machine code del stub.
     * Se __x86_indirect_thunk_rax è disponibile (CONFIG_RETPOLINE), usa la
     * variante retpoline per mitigare Spectre v2 (19 byte):
     *   49 BA [8B]   movabs r10, <sct_addr>
     *   49 8B 04 F2  mov    rax, [r10 + rsi*8]   ← sys_call_table[nr]
     *   E9 [4B]      jmp    __x86_indirect_thunk_rax
     *
     * Altrimenti fallback al direct jmp (14 byte):
     *   49 BA [8B]   movabs r10, <sct_addr>
     *   41 FF 24 F2  jmpq   *(%r10, %rsi, 8)
     */

    //lookup per thunk __x86_indirect_thunk_rax, se presente costruisce stub con retpoline per mitigare Spectre v2;
    //altrimenti fallback a direct jmp.
    thunk_addr = lookup_unexported("__x86_indirect_thunk_rax");
    if (thunk_addr) {
        /* rel32 = indirizzo_destinazione - (indirizzo_dopo_jmp)
         * "indirizzo dopo jmp" = base_stub + 19 (dimensione stub retpoline) */
        long diff = (long)thunk_addr - ((long)x64_stub + 19);
        if (diff <= (long)INT_MAX && diff >= (long)INT_MIN) {
            int rel32 = (int)diff;
            /* movabs r10, <sct_addr>  — 49 BA [8 byte little-endian]
             * REX.WB (49) estende r10 a 64 bit; BA = opcode MOV r/imm64 */
            stub_code[0]  = 0x49; stub_code[1]  = 0xBA;
            memcpy(stub_code + 2, &sct_addr, 8);       /* imm64 = indirizzo SCT */
            /* mov rax, [r10 + rsi*8]  — 49 8B 04 F2
             * REX.WB (49), 8B = MOV r64←m64, ModRM 04 = SIB presente,
             * SIB F2 = base r10 (rex.B) + index rsi * scale 8 */
            stub_code[10] = 0x49; stub_code[11] = 0x8B;
            stub_code[12] = 0x04; stub_code[13] = 0xF2;
            /* jmp rel32  — E9 [4 byte little-endian]
             * salta al retpoline thunk; rax contiene il puntatore da chiamare */
            stub_code[14] = 0xE9;
            memcpy(stub_code + 15, &rel32, 4);
            stub_len = 19;
            printk(KERN_INFO "<throttle>: stub con retpoline (thunk @ %px)\n",
                   (void *)thunk_addr);
        } else {
            thunk_addr = 0; /* fuori range ±2GB, usa fallback */
        }
    }
    if (!thunk_addr) {
        /* movabs r10, <sct_addr>  — 49 BA [8 byte little-endian] */
        stub_code[0]  = 0x49; stub_code[1]  = 0xBA;
        memcpy(stub_code + 2, &sct_addr, 8);
        /* jmpq *(%r10 + rsi*8)  — 41 FF 24 F2
         * REX.B (41) per r10, FF /4 = JMP m64, ModRM 24 = SIB,
         * SIB F2 = base r10 + index rsi * 8  →  salta a sys_call_table[nr] */
        stub_code[10] = 0x41; stub_code[11] = 0xFF;
        stub_code[12] = 0x24; stub_code[13] = 0xF2;
        stub_len = 14;
    }

    /* Scrive il stub via stop_machine (gestisce CR0.WP e atomicità SMP) */
    pd.dst = x64_stub;
    pd.src = stub_code;
    pd.len = stub_len;
    ret = stop_machine(do_text_patch, &pd, NULL);
    if (ret) {
        printk(KERN_ERR "<throttle>: scrittura stub fallita (%d)\n", ret);
        stub_free(x64_stub);
        x64_stub = NULL;
        return ret;
    }

    /* Rende la pagina eseguibile */
    ret = stub_set_memory_x(x64_stub);
    if (ret) {
        printk(KERN_ERR "<throttle>: set_memory_x stub fallita (%d)\n", ret);
        stub_free(x64_stub);
        x64_stub = NULL;
        return ret;
    }

    printk(KERN_INFO "<throttle>: stub allocato @ %px (SCT @ %px)\n",
           x64_stub, (void *)sct_addr);
    return 0;
}

int patch_x64_sys_call(void)
{
    struct text_patch_data pd;
    long diff;
    int  off32;
    int  ret;

    if (!x64_sys_call_addr) return -ENODEV;

    ret = alloc_x64_stub();
    if (ret) return ret;

    /* Salva i byte originali */
    memcpy(x64_orig_bytes, (void *)x64_sys_call_addr, 5);

    /* Verifica raggiungibilità con JMP rel32 (±2 GB):
     * module_alloc alloca nel range MODULES_VADDR–MODULES_END,
     * che è tipicamente entro 2 GB dal testo kernel. */
    diff = (long)x64_stub - (long)x64_sys_call_addr - 5;
    if (diff > (long)INT_MAX || diff < (long)INT_MIN) {
        printk(KERN_ERR "<throttle>: stub fuori range JMP rel32 (diff=%ld)\n", diff);
        stub_free(x64_stub);
        x64_stub = NULL;
        return -ERANGE;
    }
    off32 = (int)diff;

    x64_jump_inst[0] = 0xE9;
    memcpy(x64_jump_inst + 1, &off32, sizeof(int));

    pd.dst = (void *)x64_sys_call_addr;
    pd.src = x64_jump_inst;
    pd.len = 5;
    ret = stop_machine(do_text_patch, &pd, NULL);
    if (ret) {
        printk(KERN_ERR "<throttle>: stop_machine patch fallita (%d)\n", ret);
        stub_free(x64_stub);
        x64_stub = NULL;
        return ret;
    }

    printk(KERN_INFO "<throttle>: x64_sys_call → stub @ %px (off32=%d)\n",
           x64_stub, off32);
    return 0;
}

/*
 * restore_x64_sys_call — ripristina x64_sys_call e libera lo stub esterno.
 *
 * DEVE essere chiamata in throttle_exit() DOPO synchronize_rcu() + drain,
 * in modo che nessun CPU possa essere ancora in esecuzione nello stub.
 */
void restore_x64_sys_call(void)
{
    struct text_patch_data pd;

    if (!x64_sys_call_addr || !x64_stub) return;

    /* Ripristina i 5 byte originali di x64_sys_call */
    pd.dst = (void *)x64_sys_call_addr;
    pd.src = x64_orig_bytes;
    pd.len = 5;
    stop_machine(do_text_patch, &pd, NULL);
    printk(KERN_INFO "<throttle>: x64_sys_call ripristinata\n");

    /* Libera lo stub: throttle_exit() ha già eseguito synchronize_rcu()
     * + drain prima di chiamare questa funzione, quindi nessun CPU è nello stub. */
    stub_free(x64_stub);
    x64_stub = NULL;
    printk(KERN_INFO "<throttle>: stub esterno liberato\n");
}

#endif /* LINUX_VERSION_CODE >= 5.15 */
