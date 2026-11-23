# Mac validation

The following checks were performed on **2026-11-15** with a spare ESP32-C3 SuperMini V1601 and macOS 27.2.1 / arm64. Only USB connected the board to the Mac. The projector and all external UART wires remained disconnected.

The Mac identified a native Espressif USB Serial/JTAG device (VID `303a`, PID `1001`), without installing a separate serial driver. esptool identified ESP32-C3 revision 0.4, embedded 4 MB flash and a 40 MHz crystal.

## Result

- The normal and internal-loopback images built with the pinned ESP-IDF 5.5.2 source and native Mac toolchain. Application sizes were 162592 and 162736 bytes respectively.
- The diagnostic image was flashed and the writer verified all written regions. The host then sent **65536 bytes** at a requested maximum of 9216 bytes/s and received exactly the same bytes. The comparison includes all 256 byte values, zero bytes, CR/LF and flow-control characters. The payload SHA-256 was `7070bf56ba0957b90059c9e7be917ca4a6c859c05939e62d6b79f6f9b28d0aeb` in both directions; the strict phase lasted 8.946446 seconds, including the trailing quiet interval.
- On this Mac, opening the port reset the ESP32 even with DTR/RTS configured low before opening. A naive first probe correctly failed because it mixed startup output with the binary test. The successful procedure preserved **238 bytes of ROM startup text** in a separate file on the same connection before starting the strict binary phase. Diagnostic UART initialization also settles and clears its own input after internal loopback is enabled; normal UART reception is unchanged.
- The first USB-only BOOT check showed the fault LED and did not arm. Adding a weak 3.3 V pull-up on RX kept its disconnected input idle-high. With the corrected normal image and the Mac port held open, the owner confirmed slow blink → BOOT held approximately 2 seconds → steady LED → release and short BOOT press → slow blink. TX pull-ups and output routing remain disabled in receive-only mode.
- The **normal image was flashed last**, with written-region verification and internal loopback disabled. It starts with external TX disconnected as an output.
- All **19 host tests** passed under Python 3.9.6. Native control tests passed with AddressSanitizer and UndefinedBehaviorSanitizer. A deliberately conflicting generated configuration was rejected by the build-mode guard before compilation.

Exact application hashes, test output and scope are in [the sanitized report](../reports/mac-validation-2026-11-15.json). Private logs and raw captures stay under ignored `local/`.

## Reproduce the internal test

Activate the locked ESP-IDF environment, build both modes and identify the spare board's current port. The example port below is illustrative; check yours with `list`.

```sh
python -m pip install -r requirements-host.txt
sh scripts/build.sh normal
sh scripts/build.sh loopback
python tools/serial_probe.py list
idf.py -B build/loopback -p /dev/cu.usbmodem101 flash
mkdir -p local
python tools/serial_probe.py loopback /dev/cu.usbmodem101 \
    --bytes 65536 --timeout 20 \
    --startup-log local/startup.bin --startup-duration 1
idf.py -B build/normal -p /dev/cu.usbmodem101 flash
```

Choose an unused startup filename for another run. The optional startup phase is explicit and preserved; there is no hidden purge in the strict comparison.

## Limits

This verifies **Mac USB ↔ application ↔ internal UART loopback**. It does not verify external GPIO continuity, measured TX voltage, electrical high impedance, operation with a 1.8 V level shifter or communication with the projector. The owner confirmed the physical BOOT/LED sequence; this does not replace electrical measurement of TX.

The native USB port's baud-rate setting does not throttle host writes. This result used paced writes and is not evidence of lossless unrestricted USB-to-UART traffic or hardware flow control. No target voltage or external wiring has been qualified.

## LuatOS CORE prototype

On **2026-11-22**, the Mac enumerated the LuatOS CORE's onboard CH343 as `1a86:55d3`. An earlier firmware prototype passed an **8,192-byte** external loopback with a jumper between the onboard CH343's TX (`RX0` on the board) and `IO03`. This exercises the external receive pin and the return path through the CH343.

On **2026-11-23**, open-drain prototypes on `IO01` received a cold boot trace and separately sent one shell identification command and one ordinary reboot-to-recovery request to a powered target. The shell returned its user and the owner observed the recovery menu. The target supplied the idle high level. Those prototypes accepted fixed commands. The reusable frame format and host command in this repository have their own build and host checks and require a separate physical loopback with the resulting firmware.
