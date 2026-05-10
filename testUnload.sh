#!/bin/bash
# testUnload.sh — verifica il drain protocol durante rmmod
#
# Scenario:
#   1. Avvia N thread che martellano getpid() con MAX=1 → quasi tutti
#      bloccati in throttle_check (wait queue del driver).
#   2. Dopo 2s, esegue rmmod mentre i thread sono ancora attivi.
#      Il drain protocol in throttle_exit deve:
#        - segnalare module_unloading → i thread escono dalla wait queue
#        - aspettare active_threads_in_wrapper == 0 (tutti fuori dal wrapper)
#   3. Misura il tempo di unload e verifica che completi entro TIMEOUT.
#   4. Ricarica il modulo per lasciare il sistema in stato pulito.
#
# Uso:
#   sudo ./testUnload.sh [num_thread] [timeout_sec]
#
# Esempio:
#   sudo ./testUnload.sh 8 5

set -e

THROTTLE_TEST="./throttleTest"
THREADS="${1:-8}"
TIMEOUT="${2:-5}"

# ----------------------------------------------------------------
#  Prerequisiti
# ----------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    echo "Errore: richiede root." >&2
    exit 1
fi
if [ ! -x "$THROTTLE_TEST" ]; then
    echo "Errore: $THROTTLE_TEST non trovato. Esegui 'make' prima." >&2
    exit 1
fi
if [ ! -c "/dev/throttleDriver" ]; then
    echo "Errore: /dev/throttleDriver non trovato. Esegui 'make load' prima." >&2
    exit 1
fi

echo "========================================================"
echo "  testUnload — drain protocol durante rmmod"
echo "========================================================"
echo "  Thread  : $THREADS (MAX=1 → quasi tutti bloccati)"
echo "  Timeout : ${TIMEOUT}s per l'unload"
echo "========================================================"
echo ""

# ----------------------------------------------------------------
#  Avvia throttleTest in background con MAX=1 e durata lunga
#  I thread si bloccheranno quasi subito in throttle_check
# ----------------------------------------------------------------
echo "[1/4] Avvio $THREADS thread con MAX=1 (durata 30s, interrotti dal test)..."
"$THROTTLE_TEST" "$THREADS" 30 1 0 1 > /tmp/throttleTest_unload.log 2>&1 &
TEST_PID=$!

# Attende che i thread siano operativi e bloccati
sleep 2

if ! kill -0 "$TEST_PID" 2>/dev/null; then
    echo "ERRORE: throttleTest terminato prematuramente."
    cat /tmp/throttleTest_unload.log
    exit 1
fi
echo "  throttleTest attivo (PID $TEST_PID), thread bloccati in wait queue."
echo ""

# ----------------------------------------------------------------
#  Esegue rmmod e misura il tempo
# ----------------------------------------------------------------
echo "[2/4] Esecuzione rmmod con thread attivi..."
T_START=$(date +%s%N)

# Rimuove il modulo: il drain protocol deve sbloccare i thread e completare
sudo rmmod throttleDriver
sudo rm -f /dev/throttleDriver

T_END=$(date +%s%N)
ELAPSED_MS=$(( (T_END - T_START) / 1000000 ))

echo "  rmmod completato in ${ELAPSED_MS} ms"
echo ""

# ----------------------------------------------------------------
#  Verifica che l'unload sia avvenuto entro il timeout
# ----------------------------------------------------------------
echo "[3/4] Verifica..."

TIMEOUT_MS=$(( TIMEOUT * 1000 ))
if [ "$ELAPSED_MS" -le "$TIMEOUT_MS" ]; then
    ESITO_TEMPO="PASS (${ELAPSED_MS}ms ≤ ${TIMEOUT_MS}ms)"
else
    ESITO_TEMPO="FAIL (${ELAPSED_MS}ms > ${TIMEOUT_MS}ms — possibile hang)"
fi

# Verifica che il modulo non sia più caricato
if lsmod | grep -q "^throttleDriver "; then
    ESITO_MOD="FAIL (modulo ancora presente)"
else
    ESITO_MOD="PASS (modulo rimosso)"
fi

echo "  Tempo unload : $ESITO_TEMPO"
echo "  Stato modulo : $ESITO_MOD"
echo ""

# Attende che throttleTest si accorga che il monitor è spento e termini
wait "$TEST_PID" 2>/dev/null || true

# ----------------------------------------------------------------
#  Ricarica il modulo per lasciare il sistema in stato pulito
# ----------------------------------------------------------------
echo "[4/4] Ricarica del modulo..."
make load --no-print-directory > /dev/null 2>&1
echo "  Modulo ricaricato."
echo ""

# ----------------------------------------------------------------
#  Esito finale
# ----------------------------------------------------------------
echo "========================================================"
if [[ "$ESITO_TEMPO" == PASS* ]] && [[ "$ESITO_MOD" == PASS* ]]; then
    echo "  PASS — drain protocol funziona correttamente"
else
    echo "  FAIL"
    echo "  $ESITO_TEMPO"
    echo "  $ESITO_MOD"
fi
echo "========================================================"
