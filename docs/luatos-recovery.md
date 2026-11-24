# LuatOS D9W recovery profile

Use the `luatos-recovery` build mode with the LuatOS ESP32-C3 CORE CH343
adapter and D9W TWRP R1. It forwards the target's UART stream byte for byte
at 115200 baud, 8N1. Target transmit uses the same 3.3 V pins and idle-high
check as the LuatOS mode. IO01 starts as an input and returns to an input
after each transmission.

Activate the ESP-IDF checkout pinned in `idf.lock.json` and build the profile.

```sh
sh scripts/build.sh luatos-recovery
```

The application image is `build/luatos-recovery/esp32c3_usb_uart.bin`.
Connect only the identified adapter when checking its build or wiring on a
bench. A target must provide compatible electrical levels on the UART.

## Session and request format

R1 sends `D9W1 <nonce> READY`. The bridge accepts commands with that
32-character lowercase hexadecimal nonce for up to 600 seconds and
128 transmitted requests. A duplicate `READY` for the current nonce
leaves both limits in place. A matching `EXIT` closes the session.

Host requests are ASCII with a single space between tokens and a final LF.

| Request body | Accepted arguments |
| --- | --- |
| `STATUS` | None |
| `GET <name>` | `cache.bin`, `data.bin`, `apex.bin`, `adb.bin`, `boot.bin`, `guard-status.bin`, `guard-exit.bin`, `result.txt` |
| `GET <name> <offset>` | `logcat`, `logcat.1`, `logcat.2`, `logcat.3` and an aligned offset |
| `PLAN <operation>`, `RUN <operation>` | `install`, `restore`, `collect`, `reboot` |

An offset is canonical unsigned decimal from 0 to 983040 inclusive, in
65536-byte steps. The host verifies the full-file SHA-256 supplied by R1
after assembling the ranges. A 1 MiB log needs 16 windows and approximately
220 seconds at line rate because the chunks are hex encoded. Target data
streams through unchanged, including replies larger than the bridge's
request parser.

The bridge also accepts `D9W:RECOVERY` once per adapter boot while no
session is active. It transmits the fixed normal recovery reboot command.
The host checks the expected projector console before sending this request.
R1 validates operation plans and their signed inputs before starting writes.

Native parser fixtures exercise the opt-in profile's framing, requests and
limits. Separate hardware tests establish transport behavior on the adapter.
