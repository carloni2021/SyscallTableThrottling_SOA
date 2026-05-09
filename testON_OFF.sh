#!/bin/bash
# testON_OFF.sh — confronto monitor OFF (baseline) vs monitor ON (throttling attivo)
#
# Esegue throttleTest due volte con gli stessi parametri e lo stesso MAX:
#   Run 1 — monitor OFF: hook installati ma nessun blocco — baseline pulito.
#   Run 2 — monitor ON:  throttling attivo al limite MAX configurato.
#
# Il confronto mostra:
#   - overhead degli hook senza throttling (run 1)
#   - costo aggiuntivo del blocking (differenza run 2 - run 1)
#
# Uso:
#   sudo ./testON_OFF.sh <num_thread> <durata_sec> <MAX>
#
# Esempio:
#   sudo ./testON_OFF.sh 4 6 50

set -e

THROTTLE_TEST="./throttleTest"

if [ "$#" -lt 3 ]; then
    echo "Uso: sudo $0 <num_thread> <durata_sec> <MAX>" >&2
    echo "Esempio: sudo $0 4 6 50" >&2
    exit 1
fi

THREADS="$1"
DURATA="$2"
MAX="$3"

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

# ----------------------------------------------------------------
#  Funzioni di parsing (stessa logica di testCSV.sh)
# ----------------------------------------------------------------
estrai_avg_calls()  { echo "$1" | awk '/Avg  calls\/finestra/{print $NF}'; }
estrai_peak_calls() { echo "$1" | awk '/Peak calls\/finestra/{print $NF}'; }
estrai_peak_delay() { echo "$1" | awk '/Peak delay/ && !/prog/ && !/uid/ {print $4}'; }
estrai_avg_delay()  { echo "$1" | awk '/Avg  delay/  && !/prog/ && !/uid/ {print $4}'; }
estrai_peak_blocc() { echo "$1" | awk '/Peak bloccati/{print $4}'; }
estrai_avg_blocc()  { echo "$1" | awk '/Avg  bloccati/{print $4}'; }
estrai_esito()      { echo "$1" | grep -E "^\s+(PASS|FAIL)$" | tr -d ' '; }

echo "========================================================"
echo "  testON_OFF — monitor OFF (baseline) vs monitor ON"
echo "========================================================"
echo "  Thread   : $THREADS"
echo "  Durata   : ${DURATA}s per run"
echo "  MAX      : $MAX inv/s"
echo "========================================================"
echo ""

# ----------------------------------------------------------------
#  Run 1: monitor OFF — hook attivi, nessun throttling
# ----------------------------------------------------------------
echo "[1/2] Baseline (monitor OFF, MAX=$MAX) ..."
out_base=$("$THROTTLE_TEST" "$THREADS" "$DURATA" "$MAX" 0 2>&1) || true

b_avg_calls=$(estrai_avg_calls  "$out_base")
b_peak_calls=$(estrai_peak_calls "$out_base")
b_peak_delay=$(estrai_peak_delay "$out_base")
b_avg_delay=$(estrai_avg_delay  "$out_base")
b_peak_blocc=$(estrai_peak_blocc "$out_base")
b_avg_blocc=$(estrai_avg_blocc  "$out_base")

echo "  avg  calls/finestra : ${b_avg_calls:-N/A}"
echo "  peak calls/finestra : ${b_peak_calls:-N/A}"
echo "  peak delay          : ${b_peak_delay:-N/A} ns"
echo "  avg  delay          : ${b_avg_delay:-N/A} ns"
echo "  peak bloccati       : ${b_peak_blocc:-N/A} thread"
echo ""

sleep 1

# ----------------------------------------------------------------
#  Run 2: monitor ON — throttling attivo
# ----------------------------------------------------------------
echo "[2/2] Throttling attivo (monitor ON, MAX=$MAX) ..."
out_thr=$("$THROTTLE_TEST" "$THREADS" "$DURATA" "$MAX" 2>&1) || true

t_avg_calls=$(estrai_avg_calls  "$out_thr")
t_peak_calls=$(estrai_peak_calls "$out_thr")
t_peak_delay=$(estrai_peak_delay "$out_thr")
t_avg_delay=$(estrai_avg_delay  "$out_thr")
t_peak_blocc=$(estrai_peak_blocc "$out_thr")
t_avg_blocc=$(estrai_avg_blocc  "$out_thr")
esito=$(estrai_esito "$out_thr")

echo "  avg  calls/finestra : ${t_avg_calls:-N/A}"
echo "  peak calls/finestra : ${t_peak_calls:-N/A}"
echo "  peak delay          : ${t_peak_delay:-N/A} ns"
echo "  avg  delay          : ${t_avg_delay:-N/A} ns"
echo "  peak bloccati       : ${t_peak_blocc:-N/A} thread"
echo ""

# ----------------------------------------------------------------
#  Confronto affiancato
# ----------------------------------------------------------------
echo "========================================================"
echo "  Confronto"
echo "========================================================"
printf "  %-26s  %-16s  %-16s\n" "Metrica"                "Baseline"         "Throttling ON"
printf "  %-26s  %-16s  %-16s\n" "--------------------------" "----------------" "----------------"
printf "  %-26s  %-16s  %-16s\n" "avg  calls/finestra"    "${b_avg_calls:-N/A}"   "${t_avg_calls:-N/A}"
printf "  %-26s  %-16s  %-16s\n" "peak calls/finestra"    "${b_peak_calls:-N/A}"  "${t_peak_calls:-N/A}"
printf "  %-26s  %-16s  %-16s\n" "peak delay (ns)"        "${b_peak_delay:-N/A}"  "${t_peak_delay:-N/A}"
printf "  %-26s  %-16s  %-16s\n" "avg  delay (ns)"        "${b_avg_delay:-N/A}"   "${t_avg_delay:-N/A}"
printf "  %-26s  %-16s  %-16s\n" "peak bloccati (thread)" "${b_peak_blocc:-N/A}"  "${t_peak_blocc:-N/A}"
echo ""
echo "  Esito throttling: ${esito:-N/A}"
echo "========================================================"
