# throttleDriver — Syscall Throttling Monitor

**throttleDriver** è un modulo del kernel Linux per architettura *x86-64*
che implementa un meccanismo di *System Call Throttling*.

Il modulo permette di limitare il numero di invocazioni di specifiche system call
da parte di programmi o utenti monitorati. La politica di throttling è basata su una
**finestra temporale di 1 secondo**: i thread che superano il limite MAX configurato
vengono bloccati trasparentemente fino all'apertura della finestra successiva. Il modulo
raccoglie inoltre statistiche sul delay introdotto e sul numero di thread bloccati.

Il throttling si attiva quando valgono le condizioni:
```
syscall_is_registered(nr)  AND  (prog_is_registered(exe) OR uid_is_registered(euid))
```

Basato sul discoverer della sys_call_table di
[Prof. Francesco Quaglia](https://github.com/FrancescoQuaglia/Linux-sys_call_table-discoverer),
sviluppato nell'ambito del corso di Sistemi Operativi Avanzati.

---

## Quick Start

```bash
# Compila modulo e tool userspace
make

# Carica il modulo e crea il device node
make load

# Registra un programma, una syscall e un UID, imposta il limite, abilita
sudo ./throttleClient add_prog /bin/bash
sudo ./throttleClient add_sys 1          # sys_write (nr=1)
sudo ./throttleClient add_uid 1000
sudo ./throttleClient set_max 5          # 5 chiamate/secondo
sudo ./throttleClient monitor 1

# Osserva il throttling in azione (in un altro terminale)
while true; do echo hello; done

# Consulta le statistiche
./throttleClient stats

# Scarica il modulo
make unload
```

---

## Utilizzo

L'interazione con il modulo avviene tramite `ioctl` sul device file `/dev/throttleDriver`.
È fornito il tool userspace `throttleClient` per inviare comandi al modulo.

> **Nota:** Tutti i comandi di configurazione richiedono privilegi di **root**.
> I comandi di lettura (`status`, `stats`, `list`) sono invece accessibili a tutti gli utenti.

### Esempi di comandi

**1. Registrazione filtri:**

```bash
sudo ./throttleClient add_prog /path/to/binary  # registra programma (path assoluto)
sudo ./throttleClient add_uid  1000             # registra UID
sudo ./throttleClient add_sys  39               # registra syscall (es. getpid nr=39)
```

**2. Configurazione throttling:**

```bash
sudo ./throttleClient set_max 10   # imposta MAX=10 invocazioni/secondo
sudo ./throttleClient monitor 1    # abilita il monitor (ON)
sudo ./throttleClient monitor 0    # disabilita il monitor (OFF)
```

**3. Rimozione filtri:**

```bash
sudo ./throttleClient del_prog /path/to/binary
sudo ./throttleClient del_uid  1000
sudo ./throttleClient del_sys  39
```

**4. Monitoraggio:**

```bash
./throttleClient status      # stato: monitor on/off, MAX, chiamate finestra corrente
./throttleClient stats       # statistiche: peak/avg delay, peak/avg thread bloccati
./throttleClient list        # elenca programmi, UID e syscall registrati
sudo ./throttleClient reset_stats  # azzera le statistiche
```

**5. Listing:**

La lista dei filtri registrati non avviene tramite ioctl ma tramite `read()` sul device,
in modo di evitare limiti di dimensione fissi:

```bash
./throttleClient list   # legge /dev/throttleDriver in loop fino a EOF
```

---

## Compilazione e Configurazione

### Piattaforma

Il modulo è sviluppato e testato su **Linux ≥ 5.15 / x86-64**. Le scelte realizzative
non portabili riguardano:

- Ricerca della `sys_call_table` tramite page-table walk hardware da `CR3` (PML4→PDP→PDE→PTE).
- Patching di `x64_sys_call` su kernel ≥ 5.15 in particolare (6.17), dove il dispatch non usa più la SCT direttamente.
- Stub esterno allocato con `module_alloc()` fuori dalla memoria del modulo.
- Bypass di `CR0.WP` e `CR4.CET` tramite assembly inline per scrivere in memoria read-only.
- `hrtimer` con `CLOCK_MONOTONIC` per il reset della finestra di throttling.

### Requisiti

- Linux Kernel 5.x o superiore con headers installati
- GCC, Make
- Architettura x86-64
- Privilegi root per caricare il modulo e configurarlo

### Gestione Modulo

| Target | Descrizione |
|--------|-------------|
| `make` | Compila il modulo (`throttleDriver.ko`) e i tool userspace |
| `make load` | Compila, carica il modulo (`insmod`) e crea `/dev/throttleDriver` |
| `make unload` | Scarica il modulo (`rmmod`) e rimuove il device node |
| `make clean` | Rimuove tutti i file oggetto, il modulo compilato e i binari |

---

## Test e Validazione

Sono forniti due programmi di test, ciascuno auto-configurante: aprono il device,
registrano sé stessi, eseguono il test e puliscono la configurazione al termine.

### throttleTest — Syscall non bloccante, multi-thread

```bash
sudo ./throttleTest <num_thread> <durata_sec> <MAX> [rate_per_thread] [monitor_on]

# Esempi:
sudo ./throttleTest 8 6 200          # 8 thread a piena velocità, limite 200/s
sudo ./throttleTest 8 6 200 50       # ogni thread tenta 50 chiamate/s
sudo ./throttleTest 8 6 200 0 0      # baseline: hook attivi, monitor OFF
```

Lancia N thread che invocano `getpid` (nr=39) in un loop continuo. Al termine legge le
statistiche del driver e verifica automaticamente che il rate osservato non abbia superato
MAX. Stampa `PASS` o `FAIL`.

### throttleTest2 — Syscall bloccante + throttling per UID

```bash
sudo ./throttleTest2 <num_worker> <durata_sec> <MAX>

# Esempio:
sudo ./throttleTest2 8 6 200
```

Esegue due test indipendenti:

**Test 1 — Syscall bloccante (`read` su `/dev/zero`):**
Verifica che il throttling funzioni correttamente per una syscall che si blocca in kernel
space. `/dev/zero` elimina la latenza I/O reale: l'unico fattore limitante è il rate limit
del modulo. Dimostra che il flusso `thread → wrapper → throttle_wq → orig_fn() → dato`
funziona identicamente al caso non-bloccante.

**Test 2 — Throttling per UID:**
Verifica che il limite venga applicato in base all'UID effettivo del chiamante,
indipendentemente dal nome del programma. Due gruppi di processi figli girano in parallelo
via `fork()` + `setresuid()`:
- **gruppo throttlato**: N figli con `euid = TARGET_UID` (60001) — soggetti al limite MAX
- **gruppo di "controllo"**: M figli con `euid = CONTROL_UID` (60002) — nessun limite

I contatori condivisi tra processi usano `mmap(MAP_SHARED|MAP_ANONYMOUS)` con variabili
`_Atomic` per la coordinazione lock-free.

### testCSV.sh — campagna sistematica con output CSV

```bash
sudo ./testCSV.sh [durata_sec] [output.csv]

# Esempio:
sudo ./testCSV.sh 6 campagna.csv
```

Esegue `throttleTest` su una matrice completa **thread in {1,2,4,8} × MAX in {10,50,100}**
(12 run totali) e salva i risultati in un file CSV:

```
threads,MAX,avg_calls_finestra,peak_calls_finestra,avg_delay_ns,peak_delay_ns,avg_bloccati,peak_bloccati
1,10,...
...
8,100,...
```

Permette di analizzare come throughput, delay e thread bloccati variano al variare
del carico e del limite configurato.

### testON_OFF.sh — confronto baseline vs throttling attivo

```bash
sudo ./testON_OFF.sh <num_thread> <durata_sec> <MAX>

# Esempio:
sudo ./testON_OFF.sh 4 6 50
```

Esegue `throttleTest` due volte con lo stesso MAX:

- **Run 1 — Baseline**: monitor OFF — hook installati ma nessun blocco. Misura l'overhead
  puro degli hook senza throttling attivo.
- **Run 2 — Throttling attivo**: monitor ON — misura il rate limiting in azione.

Stampa i risultati affiancati per mostrare il costo aggiuntivo del blocking rispetto
all'overhead di base degli hook.

---

## Dettagli Tecnici

### 1. Ricerca della sys_call_table

Il kernel non esporta l'indirizzo di `sys_call_table`. Anziché usare
`kallsyms_lookup_name` (non disponibile ai moduli dal kernel 5.7), il modulo usa un
**page-table walk hardware** partendo da `CR3`.

`sys_vtpmo(vaddr)` percorre la gerarchia a quattro livelli (PML4→PDP→PDE→PTE) leggendo
l'indirizzo fisico direttamente da `CR3`. `find_sys_call_table()` scansiona il range
`[0xffffffff00000000, 0xfffffffffff00000)` pagina per pagina: per ogni pagina mappata,
`sct_validate_page()` verifica che 7 indici noti di `sys_ni_syscall`
(134, 174, 182, 183, 214, 215, 236) puntino tutti allo stesso valore — un pattern stabile
tra versioni del kernel che identifica univocamente la syscall table.

Questo approccio non richiede simboli esportati, funziona con `CONFIG_KALLSYMS_ALL=n`
e sopravvive a KASLR perché la scansione copre l'intero range possibile.

### 2. Patching di x64_sys_call (kernel ≥ 5.15)

Dal kernel 5.15 il percorso di dispatch è cambiato:

```
< 5.15:   entry_SYSCALL_64 → sys_call_table[nr](regs)
≥ 5.15:   entry_SYSCALL_64 → do_syscall_64 → x64_sys_call(regs, nr)
```

`x64_sys_call` è un `switch` generato dal compilatore che **non usa** la SCT. Sostituire
le entry della tabella non è più sufficiente.

**Soluzione:** reindirizzare `x64_sys_call` verso la nostra SCT (già hookata) tramite
uno **stub esterno**. L'indirizzo di `x64_sys_call` viene risolto con un kprobe
(registrato e rimosso immediatamente a init, zero overhead a runtime). I primi 5 byte
di `x64_sys_call` vengono sostituiti con un `JMP rel32` verso lo stub.

Lo stub (allocato con `module_alloc()` **fuori dalla memoria del modulo**) contiene
machine code auto-contenuto. Se disponibile, usa la variante retpoline (19 byte,
mitigazione Spectre v2), altrimenti il direct jmp (14 byte):

```asm
; Retpoline (19 byte):
movabs r10, <sys_call_table_addr>   ; indirizzo SCT hardcoded
mov    rax, [r10 + rsi*8]          ; sys_call_table[nr], rsi=nr per ABI do_syscall_64
jmp    __x86_indirect_thunk_rax    ; salto indiretto sicuro

; Direct jmp (14 byte, fallback):
movabs r10, <sys_call_table_addr>
jmpq   *(%r10, %rsi, 8)
```

Lo stub è esterno al modulo perché la memoria del modulo viene liberata dopo
`module_exit()`. Lo stub sopravvive indipendentemente e viene liberato esplicitamente
dopo.

Su kernel ≥ 6.4, `execmem_alloc`, `execmem_free` e `set_memory_x` non sono più
esportate e vengono risolte a runtime con lo stesso helper kprobe.

### 3. Identificazione del processo tramite inode

Per determinare se una syscall proviene da un programma monitorato, il modulo legge
`current->mm->exe_file` ed estrae **numero di inode + device ID** dell'eseguibile.

`current->comm` non viene usato perché può essere modificato dal processo stesso
(`prctl(PR_SET_NAME)`), è troncato a 16 byte e non è univoco tra eseguibili diversi
con lo stesso nome. L'identità inode+device:
- sopravvive ai rename del binario
- distingue hard link su filesystem diversi
- non può essere falsificata dal processo monitorato

`lettura dell'eseguibile protetta tramite mmap_read_lock(mm)`.

### 4. Strutture dati

| Cosa | Struttura    | Chiave | Complessità |
|------|--------------|--------|-------------|
| Programmi monitorati | Hashtable kernel | numero di inode | O(1) medio |
| UID monitorati | Hashtable kernel | valore uid | O(1) medio |
| Syscall monitorate | Bitmap kernel| numero syscall | O(1) |

La bitmap è la struttura più importante : `test_bit(nr, syscall_bitmap)` è
una singola istruzione bitwise senza allocazione, eseguita ad ogni invocazione prima
di qualsiasi lock.

### 5. Algoritmo di throttling

Il modulo usa un approccio a **contatore su finestra fissa**:

- Un `atomic_t call_count` conta le chiamate nella finestra corrente.
- Un `hrtimer` scatta ogni secondo (`CLOCK_MONOTONIC`) e azzera atomicamente `call_count`.
- Ad ogni invocazione monitorata, `throttle_check()` fa `atomic_inc_return(&call_count)`.
  Se il valore supera `max_calls`, il thread si blocca su `throttle_wq` fino al prossimo tick.

### 6. Syscall bloccanti vs non bloccanti

Per le syscall **non bloccanti** (es. `getpid`, `write` su pipe), il delay di throttling
è l'unica fonte di latenza e le statistiche riflettono direttamente la pressione del throttle.

Per le syscall **bloccanti** (es. `read` da socket, `accept`, `futex`), il thread attraversa
due fasi di blocco indipendenti: il throttling (misurato) e l'attesa I/O dentro `orig_fn`
(non misurata). Un thread non throttlato può comunque impiegare secondi dentro `orig_fn`
— quel tempo non viene mai attribuito al throttle.

### 7. Prevenzione del thundering herd

Una semplice `wake_up_all()` chiamata ogni volta sveglierebbe tutti i thread in attesa creando il seguente problema:
con 1000 thread e `max_calls=100`, 900 si risveglierebbero inutilmente tornando subito a dormire.

**Soluzione:** tramite due meccanismi cooperanti:
- I thread si bloccano con `wait_event_interruptible_exclusive()` — marcati come *esclusivi*
  nella wait queue.
- Il timer chiama `wake_up_nr(&throttle_wq, max_calls)` — sveglia esattamente tanti thread
  quanti sono gli slot disponibili nella nuova finestra.

I thread svegliati controllano comunque il contatore: nel caso un altro thread avesse già
occupato lo slot, torneranno a dormire fino al tick successivo.

### 8. Drain protocol per lo scaricamento sicuro

`try_module_get()` non è utilizzabile (nonostate un iniziale tentativo) poichè i thread possono dormire dentro `generic_sct_wrapper`
(bloccati in `throttle_check()`), impedendo per sempre lo scaricamento del modulo.
La soluzione è un **drain manuale**: `throttle_exit()` attende che tutti i thread
in-flight escano naturalmente.

```
throttle_exit():
  1. module_unloading=1 + wake_up_all(throttle_wq)  → sblocca i thread in attesa
  2. remove_all_hooks()                              → nessun nuovo thread entra nel wrapper
  3. synchronize_rcu()                               → attende che tutti i CPU vedano la rimozione
  4. wait_event(unload_wq, active_threads==0)        → drain: attende i thread in-flight nei cpu
  5. synchronize_rcu()                               → barriera finale
  6. restore_x64_sys_call()                          → ripristina e libera lo stub
```

Due `synchronize_rcu()` per chiudere la race: il primo garantisce che un CPU che aveva
già letto il vecchio puntatore SCT abbia avuto il tempo di entrare nel wrapper e
incrementare il contatore prima che venga controllato.

### 9. Bypass della write-protection

La syscall table risiede in memoria kernel read-only. Per sostituire le entry è necessario
disabilitare temporaneamente due protezioni hardware:

- **CR0.WP** (Write Protect bit): impedisce la scrittura su pagine read-only anche in ring 0.
- **CR4.CET** (Control-flow Enforcement Technology): presente su CPU moderne, richiede di
  essere disabilitato per scrivere su memoria eseguibile (come `x64_sys_call`).

Entrambi i registri vengono scritti con assembly inline (`mov %0, %%cr0`) con un operando
memoria fittizio `"+m"(__force_order)` che bypassa il CR pinning del kernel.
`preempt_disable()` / `preempt_enable()` impedisce la migrazione del thread tra CPU
(i registri CR sono per-CPU) durante la modifica.

### Compatibilità versioni kernel

| Range kernel | Percorso di dispatch | Comportamento del modulo |
|---|---|---|
| < 5.15 | `entry_SYSCALL_64 → sys_call_table[nr]` | Solo hook SCT |
| ≥ 5.15, < 6.4 | `entry_SYSCALL_64 → x64_sys_call` | Hook SCT + patch `x64_sys_call` via `module_alloc` |
| ≥ 6.4, < 6.15 | come sopra | Come sopra, ma `execmem_alloc/free/set_memory_x` risolti via kprobe |
| ≥ 6.15 | come sopra | Come sopra + API `hrtimer_setup` |

---

## Struttura dei file

```
throttle.h             — tipi condivisi, definizioni IOCTL, dichiarazioni extern
throttle_main.c        — init/exit del modulo, stato globale, orchestrazione drain protocol
throttle_mem.c         — page-table walk (sys_vtpmo), bypass CR0/CR4 write-protect
throttle_discovery.c   — ricerca sys_call_table (walk CR3), lookup x64_sys_call via kprobe, stub
throttle_hook.c        — generic_sct_wrapper, install_hook / remove_hook, contabilità drain
throttle_core.c        — throttle_check, callback hrtimer, identificazione processo
throttle_dev.c         — driver device a caratteri, handler ioctl, dev_read
throttleClient.c       — tool di configurazione userspace
throttleTest.c         — test multi-thread: syscall non bloccante (getpid), PASS/FAIL automatico
throttleTest2.c        — test avanzato: syscall bloccante (read) + throttling per UID
testCSV.sh             — campagna sistematica su matrice threads×MAX, output CSV
testON_OFF.sh          — confronto baseline (hook senza limite) vs throttling attivo
```

---

## Autore

**Luca Carloni**

**Laurea Magistrale in Ingegneria Informatica**
*Sistemi Operativi Avanzati (A.A. 2025/2026)*
Università degli Studi di Roma Tor Vergata
