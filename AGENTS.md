# Working on the USB–UART bridge

- Keep this project independent from the projector firmware repository.
- Build against the ESP-IDF revision in `idf.lock.json`. Preserve the default receive-only state, external TX disconnection and raw byte stream when changing firmware.
- Host checks must not open physical serial ports. Hardware tests require the owner's identified, spare ESP32-C3 and explicit scope; never infer permission to connect a target device from a loopback test.
- GPIO logic is 3.3 V. A software option cannot make these pins compatible with a 1.8 V target.
- Distinguish native tests, actual firmware builds, internal UART loopback and external pin tests. Never report the last two as interchangeable.
- Keep raw captures, device identifiers, flash dumps and generated firmware out of Git. Use ignored `local/` for private evidence.
- Use one task branch and PR, English Conventional Commits and PR titles, and the owner's `git-workflow`, `pr-mr-writing` and `readme-writing` skills. Do not merge unless authorized.
