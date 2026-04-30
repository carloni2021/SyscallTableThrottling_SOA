// SPDX-License-Identifier: GPL-2.0
/*
 * throttle_discovery.c — Ricerca di sys_call_table e x64_sys_call
 *
 * sys_call_table: scansione del virtual address space [0xffffffff00000000, MAX)
 *   con heuristica su 7 indici noti di sys_ni_syscall.
 *   Basato su usctm.c di Francesco Quaglia (GPL-2.0).
 *
 * x64_sys_call (kernel >= 5.15), percorso unico:
 *   LSTAR MSR → entry_SYSCALL_64 → scan do_syscall_64
 *   (il percorso via kprobe è rimosso: causa freeze in kprobes_module_going()
 *    durante delete_module anche dopo unregister_kprobe)
 *
 * patch_x64_sys_call: alloca uno stub eseguibile FUORI dal modulo via
 *   module_alloc(), scrive 14 byte di machine code auto-contenuto, poi
 *   installa un JMP rel32 da x64_sys_call → stub.
 *
 * Stub (14 byte, auto-contenuto, nessun riferimento a dati del modulo):
 *   49 BA [addr 8B]    movabs r10, <sys_call_table_addr>   ; SCT hardcoded
 *   41 FF 24 F2        jmpq   *(%r10, %rsi, 8)            ; sys_call_table[nr](regs)
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

#define SCT_START  0xffffffff00000000ULL
#define SCT_MAX    0xfffffffffff00000ULL
#define NI_1  134
#define NI_2  174
#define NI_3  182
#define NI_4  183
#define NI_5  214
#define NI_6  215
#define NI_7  236

syscall_fn_t     *sys_call_table_ptr = NULL;
static unsigned long *hacked_ni_syscall  = NULL;

static int sct_good_area(unsigned long *addr)
{
    int i;
    for (i = 1; i < NI_1; i++)
        if (addr[i] == addr[NI_1]) return 0;
    return 1;
}

static int sct_validate_page(unsigned long *addr)
{
    int i;
    unsigned long page = (unsigned long)addr;

    for (i = 0; i < (int)PAGE_SIZE; i += (int)sizeof(void *)) {
        unsigned long new_page = page + i + NI_7 * sizeof(void *);

        if (((page + PAGE_SIZE) == (new_page & PT_PHYS_MASK)) &&
            sys_vtpmo(new_page) == NO_MAP)
            break;

        addr = (unsigned long *)(page + i);
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
            hacked_ni_syscall  = (unsigned long *)(addr[NI_1]);
            sys_call_table_ptr = (syscall_fn_t *)addr;
            return 1;
        }
    }
    return 0;
}

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
static char x64_orig_bytes[5];
static char x64_jump_inst[5];
static void *x64_stub = NULL;   /* allocato con module_alloc, fuori dal modulo */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
/*
 * execmem_alloc, execmem_free, set_memory_x non sono EXPORT_SYMBOL su >= 6.4:
 * li risolviamo a runtime via kprobe (register+unregister immediato, solo per
 * ricavare l'indirizzo; nessun kprobe resta attivo dopo la risoluzione).
 */
#include <linux/kprobes.h>

typedef void *(*fn_execmem_alloc_t)(unsigned int type, size_t size);
typedef void  (*fn_execmem_free_t)(void *ptr);
typedef int   (*fn_set_memory_x_t)(unsigned long addr, int numpages);

static fn_execmem_alloc_t fn_execmem_alloc;
static fn_execmem_free_t  fn_execmem_free;
static fn_set_memory_x_t  fn_set_memory_x;

static unsigned long lookup_unexported(const char *name)
{
    struct kprobe kp = { .symbol_name = name };
    unsigned long addr;
    if (register_kprobe(&kp) < 0) return 0;
    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    return addr;
}

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

#else  /* kernel < 6.4 */
#  define stub_alloc()          module_alloc(PAGE_SIZE)
#  define stub_free(p)          vfree(p)
#  define stub_set_memory_x(a)  set_memory_x((unsigned long)(a), 1)
#endif

/* Scansiona func cercando cmp imm ∈ [256,600] poi CALL; ritorna la CALL target. */
static unsigned long scan_for_x64_sys_call(unsigned long func)
{
    unsigned char *p = (unsigned char *)func;
    int i, saw_cmp = 0;

    for (i = 0; i < 2048; i++) {
        if (!((unsigned long)(p + i) & (PAGE_SIZE - 1)) &&
            sys_vtpmo((unsigned long)(p + i)) == NO_MAP)
            break;

        /* cmp r/m32, imm32: 81 /7 */
        if (p[i] == 0x81 &&
            (p[i+1] & 0xC0) == 0xC0 && (p[i+1] & 0x38) == 0x38) {
            u32 imm = *(u32 *)(p + i + 2);
            if (imm >= 256 && imm <= 600) saw_cmp = 1;
        }
        /* REX.W + cmp: 48/41 81 /7 */
        if ((p[i] == 0x48 || p[i] == 0x41) && p[i+1] == 0x81 &&
            (p[i+2] & 0xC0) == 0xC0 && (p[i+2] & 0x38) == 0x38) {
            u32 imm = *(u32 *)(p + i + 3);
            if (imm >= 256 && imm <= 600) saw_cmp = 1;
        }

        if (saw_cmp && p[i] == 0xE8) {
            s32 rel = *(s32 *)(p + i + 1);
            unsigned long tgt = (unsigned long)(p + i + 5) + (long)rel;
            if (tgt > 0xffffffff00000000ULL)
                return tgt;
        }

        if (p[i] == 0xC3) break;
    }
    return 0;
}

/*
 * Percorso unico: LSTAR MSR → entry_SYSCALL_64 → scan do_syscall_64
 *
 * Il percorso via kprobe (find_x64_sys_call_kprobe) è stato rimosso.
 * Motivo: kprobes_module_going() scansiona la memoria del modulo durante
 * delete_module e si può bloccare sulla struct kprobe statica anche dopo
 * unregister_kprobe, causando un freeze durante rmmod.
 */
static int find_x64_sys_call_lstar(void)
{
    unsigned long lstar;
    unsigned char *p;
    int i;
    unsigned long cands[8];
    int ncand = 0;

    rdmsrl(MSR_LSTAR, lstar);
    if (lstar < 0xffffffff00000000ULL) {
        printk(KERN_ERR "<throttle>: LSTAR non valido: %lx\n", lstar);
        return -ENOENT;
    }
    printk(KERN_INFO "<throttle>: entry_SYSCALL_64 (LSTAR) @ %px\n", (void *)lstar);

    p = (unsigned char *)lstar;
    for (i = 0; i < 512 && ncand < 8; i++) {
        if (!((unsigned long)(p+i) & (PAGE_SIZE-1)) &&
            sys_vtpmo((unsigned long)(p+i)) == NO_MAP) break;
        if (p[i] == 0xE8) {
            s32 rel = *(s32 *)(p + i + 1);
            unsigned long tgt = (unsigned long)(p + i + 5) + (long)rel;
            if (tgt > 0xffffffff00000000ULL && tgt != lstar)
                cands[ncand++] = tgt;
        }
        if ((p[i] == 0x0F && p[i+1] == 0x07) || p[i] == 0xCF) break;
    }

    if (ncand == 0) {
        printk(KERN_ERR "<throttle>: nessuna CALL trovata in entry_SYSCALL_64\n");
        return -ENOENT;
    }

    for (i = 0; i < ncand; i++) {
        unsigned long x64;
        printk(KERN_INFO "<throttle>: provo do_syscall_64 @ %px\n", (void *)cands[i]);
        x64 = scan_for_x64_sys_call(cands[i]);
        if (x64) {
            x64_sys_call_addr = x64;
            printk(KERN_INFO "<throttle>: x64_sys_call @ %px (via LSTAR scan)\n",
                   (void *)x64);
            return 0;
        }
    }

    printk(KERN_ERR "<throttle>: x64_sys_call non trovata\n");
    return -ENOENT;
}

int find_x64_sys_call(void)
{
    return find_x64_sys_call_lstar();
}

/* ================================================================
 *  PATCH SMP-SAFE via stop_machine
 * ================================================================ */

struct text_patch_data {
    void       *dst;
    const void *src;
    size_t      len;
};

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
    unsigned char stub_code[14];
    struct text_patch_data pd;
    int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    ret = resolve_stub_fns();
    if (ret) return ret;
#endif

    /* Costruisce il machine code del stub */
    stub_code[0]  = 0x49; stub_code[1]  = 0xBA;  /* movabs r10, imm64 */
    memcpy(stub_code + 2, &sct_addr, 8);           /* SCT addr hardcoded */
    stub_code[10] = 0x41; stub_code[11] = 0xFF;   /* jmpq *(r10+rsi*8) */
    stub_code[12] = 0x24; stub_code[13] = 0xF2;

    x64_stub = stub_alloc();
    if (!x64_stub) {
        printk(KERN_ERR "<throttle>: stub_alloc fallita\n");
        return -ENOMEM;
    }

    /* Scrive il stub via stop_machine (gestisce CR0.WP e atomicità SMP) */
    pd.dst = x64_stub;
    pd.src = stub_code;
    pd.len = sizeof(stub_code);
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
