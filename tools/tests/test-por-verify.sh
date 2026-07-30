#!/bin/bash
# Host tests for xiao-por.sh's POR verification. No Pi, no relay, no INA219:
# `ssh` is stubbed and returns a canned ina219-sample.py summary line, so the
# classifier and the retry loop run for real against known-shape data.
set -u

POR="${1:?usage: run-por-tests.sh /path/to/xiao-por.sh}"
WORK="$(mktemp -d -t por-tests)"; trap 'rm -rf "$WORK"' EXIT
REPO="$WORK/repo"; FAKEBIN="$WORK/bin"
mkdir -p "$REPO/tools" "$FAKEBIN"
cp "$POR" "$REPO/tools/xiao-por.sh"; chmod +x "$REPO/tools/xiao-por.sh"
# must exist; contents irrelevant, the stub never runs it
echo "# stub" > "$REPO/tools/ina219-sample.py"
printf '#!/bin/bash\nexit 0\n' > "$FAKEBIN/sleep"; chmod +x "$FAKEBIN/sleep"

# ssh stub: pinctrl calls succeed silently; the sampler emits SUMMARY_LINE
# (or the Nth line of SUMMARY_SEQ, one per invocation) on stderr.
cat > "$FAKEBIN/ssh" <<'STUB'
#!/bin/bash
CMD="$2"
case "$CMD" in
    *pinctrl*) exit 0 ;;
    *python3*)
        N=$(cat "$WORK_DIR/.n" 2>/dev/null || echo 0); N=$((N+1)); echo "$N" > "$WORK_DIR/.n"
        LINE=$(sed -n "${N}p" "$WORK_DIR/seq" 2>/dev/null)
        [ -z "$LINE" ] && LINE=$(tail -1 "$WORK_DIR/seq" 2>/dev/null)
        [ "$LINE" = "NONE" ] && exit 1
        echo "$LINE" >&2
        exit 0 ;;
    *) exit 0 ;;
esac
STUB
chmod +x "$FAKEBIN/ssh"

PASS=0; FAIL=0
summary() { printf '# summary n=100 mean=%smA sd=%smA min=-0.300mA p50=%smA p95=0.200mA max=5.800mA\n' "$1" "$2" "$3"; }
seq_set() { : > "$WORK/seq"; rm -f "$WORK/.n"; for l in "$@"; do echo "$l" >> "$WORK/seq"; done; }

run() { local name="$1" want="$2"; shift 2
    OUT=$(env PATH="$FAKEBIN:$PATH" WORK_DIR="$WORK" "$REPO/tools/xiao-por.sh" "$@" 2>&1); RC=$?
    if [ "$RC" -eq "$want" ]; then echo "  PASS  $name (rc=$RC)"; PASS=$((PASS+1))
    else echo "  FAIL  $name — wanted rc=$want got rc=$RC"; echo "$OUT" | sed 's/^/          /'; FAIL=$((FAIL+1)); fi; }
has() { if echo "$OUT" | grep -q "$1"; then echo "  PASS  ...says '$1'"; PASS=$((PASS+1));
        else echo "  FAIL  ...should say '$1'"; echo "$OUT" | sed 's/^/          /'; FAIL=$((FAIL+1)); fi; }

echo "== clean POR (the real -0.097 mA measurement) =="
seq_set "$(summary -0.097 0.234 -0.100)"
run "verified clean" 0
has "POR verified"

echo "== contaminated POR (the real 1.512 mA failed-latch measurement) =="
seq_set "$(summary 1.512 0.081 1.510)" "$(summary 1.509 0.077 1.512)" "$(summary 1.515 0.080 1.511)"
run "3 contaminated attempts => exit 2" 2
has "did NOT take"
has "must NOT be used for battery-life numbers"

echo "== real capture 144349-postpor-idle: contaminated despite sd inflated by TX spikes =="
# p50=1.500 sd=0.588 max=29.8 — an sd-based test misreads this as 'unknown'
seq_set "$(summary 1.512 0.588 1.500)" "$(summary 1.526 0.217 1.500)" "$(summary 1.509 0.601 1.500)"
run "spiky-but-contaminated => exit 2" 2
has "did NOT take"

echo "== POR fails once then succeeds — the exact 2026-07-30 sequence =="
seq_set "$(summary 1.512 0.081 1.510)" "$(summary -0.097 0.234 -0.100)"
run "retry recovers" 0
has "POR verified"

echo "== busy device (rejoin scan) must not be called contaminated =="
seq_set "$(summary 8.600 3.900 9.100)" "$(summary -0.097 0.234 -0.100)"
run "busy then clean => 0" 0
has "can't judge the latch yet"

echo "== sampler failure is not a silent pass =="
seq_set "NONE"
run "no summary => non-zero" 3
has "could not verify"

echo "== --no-verify still works, but warns =="
seq_set "$(summary -0.097 0.234 -0.100)"
run "--no-verify exits 0" 0 --no-verify
has "NOT verified"
has "fails silently"

echo
echo "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]
