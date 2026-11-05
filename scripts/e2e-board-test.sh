#!/bin/bash
# DFC Board E2E Test Script
# Reads all relevant registers via SWD and compares against known-good values.
# Run after flashing any firmware to verify correct initialization.
#
# Usage: ./scripts/e2e-board-test.sh [label]
#   label: optional name for this test run (e.g., "beta4-boot", "custom-connected")

LABEL="${1:-test}"
PASS=0
FAIL=0
WARN=0

rd() {
    nrfjprog -f nrf52 --memrd "$1" --n 4 2>&1 | grep -v "SeggerBackend" | awk '{print $2}'
}

check() {
    local name="$1"
    local actual="$2"
    local expected="$3"
    local severity="${4:-FAIL}"  # FAIL or WARN

    if [ "$actual" = "$expected" ]; then
        echo "  ✓ $name = $actual"
        ((PASS++))
    else
        if [ "$severity" = "WARN" ]; then
            echo "  ⚠ $name = $actual (expected $expected)"
            ((WARN++))
        else
            echo "  ✗ $name = $actual (expected $expected)"
            ((FAIL++))
        fi
    fi
}

check_not() {
    local name="$1"
    local actual="$2"
    local bad_value="$3"

    if [ "$actual" != "$bad_value" ]; then
        echo "  ✓ $name = $actual (not $bad_value)"
        ((PASS++))
    else
        echo "  ✗ $name = $actual (should NOT be $bad_value)"
        ((FAIL++))
    fi
}

check_any() {
    local name="$1"
    local actual="$2"
    shift 2
    for expected in "$@"; do
        if [ "$actual" = "$expected" ]; then
            echo "  ✓ $name = $actual"
            ((PASS++))
            return
        fi
    done
    echo "  ✗ $name = $actual (expected one of: $*)"
    ((FAIL++))
}

echo "════════════════════════════════════════════════════"
echo "DFC Board E2E Test — $LABEL"
echo "Date: $(date)"
echo "════════════════════════════════════════════════════"
echo ""

# ---- 1. CPU State ----
echo "▸ CPU State"
PC=$(nrfjprog -f nrf52 --readregs 2>&1 | grep "PC:" | awk '{print $2}')
XPSR=$(nrfjprog -f nrf52 --readregs 2>&1 | grep "xPSR:" | awk '{print $2}')

# PC should be in SoftDevice idle loop (0x2xxx) or app area (0x27xxx+)
PC_NUM=$((16#${PC:2}))
if [ $PC_NUM -ge $((16#1000)) ] && [ $PC_NUM -le $((16#100000)) ]; then
    echo "  ✓ PC = $PC (in valid code range)"
    ((PASS++))
else
    echo "  ✗ PC = $PC (outside valid code range!)"
    ((FAIL++))
fi

# Check no fault (IPSR = 0 means thread mode, not exception)
IPSR_BITS=$((16#${XPSR:2} & 0x1FF))
if [ $IPSR_BITS -eq 0 ]; then
    echo "  ✓ IPSR = 0 (thread mode, no fault)"
    ((PASS++))
else
    echo "  ✗ IPSR = $IPSR_BITS (in exception handler!)"
    ((FAIL++))
fi
echo ""

# ---- 2. Fault Registers ----
echo "▸ Fault Status"
check "CFSR (no config fault)" "$(rd 0xE000ED28)" "00000000"
check "HFSR (no hard fault)" "$(rd 0xE000ED2C)" "00000000"
echo ""

# ---- 3. UICR ----
echo "▸ UICR Configuration"
check "PSELRESET[0] (P0.18)" "$(rd 0x10001200)" "00000012" "WARN"
check "PSELRESET[1] (P0.18)" "$(rd 0x10001204)" "00000012" "WARN"
REGOUT0=$(rd 0x10001304)
check_any "REGOUT0 (voltage)" "$REGOUT0" "FFFFFFFF" "00000005"
echo ""

# ---- 4. Clocks ----
echo "▸ Clocks"
HFSTAT=$(rd 0x4000040C)
check_not "HFCLK running" "$HFSTAT" "00000000"
LFSTAT=$(rd 0x40000418)
check_not "LFCLK running" "$LFSTAT" "00000000"
echo ""

# ---- 5. Peripherals that SHOULD be off ----
echo "▸ Peripherals (should be disabled)"
check "USB disabled" "$(rd 0x40027500)" "00000000"
check "QSPI disabled" "$(rd 0x40029500)" "00000000"
check "SPI0 disabled" "$(rd 0x40003500)" "00000000"
check "SPI1/TWI1 disabled" "$(rd 0x40004500)" "00000000"
echo ""

# ---- 6. GPIO Pin Configuration ----
echo "▸ GPIO Configuration"
echo "  (Reference: original FW when UART active)"
echo "  P0.7  PIN_CNF = $(rd 0x5000071C) (original FW: 0x00 as UART CTS; ours: 0x02, pin is not wired on the DFC)"
check "P0.12 (unconnected) = default input" "$(rd 0x50000730)" "00000002"
check "P0.13 (unconnected) = default input" "$(rd 0x50000734)" "00000002"
check "P0.28 (LED) = output" "$(rd 0x50000770)" "00000003"
check "P0.30 (SP3222 /SHDN) = output" "$(rd 0x50000778)" "00000003"
P0_OUT=$(rd 0x50000504)
P0_OUT_NUM=$((16#$P0_OUT))
if [ $((P0_OUT_NUM & 0x40000000)) -eq 0 ]; then
    echo "  ✓ P0.30 (SP3222 /SHDN) driven LOW — RS-232 driver shut down"
    ((PASS++))
else
    echo "  ✗ P0.30 (SP3222 /SHDN) driven HIGH — RS-232 driver ENABLED, will corrupt the bus"
    ((FAIL++))
fi
check "P1.12 (SP3222 /EN) = output" "$(rd 0x50000A30)" "00000003"
P1_OUT=$(rd 0x50000804)
echo "  P1.OUT = $P1_OUT (bit 12 = /EN: 1 = receivers off/idle, 0 = receivers on/connected)"

# These depend on whether UART is active:
P5_CNF=$(rd 0x50000714)
P26_CNF=$(rd 0x50000768)
P27_CNF=$(rd 0x5000076C)
echo "  P0.5  (RTS) PIN_CNF = $P5_CNF (0x03 when UART active, 0x02 at boot)"
echo "  P0.26 (RXD) PIN_CNF = $P26_CNF (0x00 when UART active, 0x02 at boot)"
echo "  P0.27 (TXD) PIN_CNF = $P27_CNF (0x03 when UART active, 0x02 at boot)"
echo ""

# ---- 7. UART State ----
echo "▸ UART0 Configuration"
UART_EN=$(rd 0x40002500)
echo "  UART ENABLE = $UART_EN (0x04=legacy UART, 0x08=UARTE DMA, 0x00=off)"

if [ "$UART_EN" = "00000004" ]; then
    echo "  ✓ Legacy UART mode (matching original firmware)"
    ((PASS++))
    check "PSEL.RXD = P0.26" "$(rd 0x40002514)" "0000001A"
    check "PSEL.TXD = P0.27" "$(rd 0x4000250C)" "0000001B"
    check "PSEL.RTS = P0.05" "$(rd 0x40002508)" "00000005"
    check "PSEL.CTS = P0.07" "$(rd 0x40002510)" "00000007"
    check "BAUDRATE = 19200" "$(rd 0x40002524)" "004EA000"
    check "CONFIG = HWFC enabled" "$(rd 0x4000256C)" "00000001"

    INTENSET=$(rd 0x40002304)
    echo "  INTENSET = $INTENSET"
    # Should have at least RXDRDY(2) and ERROR(9) = 0x204
    INTEN_NUM=$((16#$INTENSET))
    if [ $((INTEN_NUM & 0x204)) -eq $((0x204)) ]; then
        echo "  ✓ RXDRDY + ERROR interrupts enabled"
        ((PASS++))
    else
        echo "  ✗ Missing RXDRDY or ERROR interrupt"
        ((FAIL++))
    fi
elif [ "$UART_EN" = "00000008" ]; then
    echo "  ⚠ UARTE DMA mode (original firmware uses legacy UART!)"
    ((WARN++))
elif [ "$UART_EN" = "00000000" ]; then
    echo "  ⚠ UART disabled (original beta4 disables at boot, enables on BLE connect)"
    ((WARN++))
else
    echo "  ✗ Unknown UART ENABLE value: $UART_EN"
    ((FAIL++))
fi
echo ""

# ---- 8. BLE Advertising Check ----
echo "▸ BLE State (indirect)"
# SoftDevice manages BLE — we can't directly read BLE state via registers.
# But we can check if the SoftDevice is running by looking at SWI2 handler
SWI2_VEC=$(rd 0x270A0)
if [ "$SWI2_VEC" != "00000000" ]; then
    echo "  ✓ SWI2 vector set ($SWI2_VEC) — SoftDevice event handler configured"
    ((PASS++))
else
    echo "  ⚠ SWI2 vector = 0 — SoftDevice events may not be handled"
    ((WARN++))
fi
echo ""

# ---- Summary ----
TOTAL=$((PASS + FAIL + WARN))
echo "════════════════════════════════════════════════════"
echo "RESULTS: $PASS passed, $FAIL failed, $WARN warnings (of $TOTAL checks)"
if [ $FAIL -eq 0 ]; then
    echo "STATUS: ✓ ALL CHECKS PASSED"
else
    echo "STATUS: ✗ $FAIL CHECKS FAILED — investigate above"
fi
echo "════════════════════════════════════════════════════"
