# V31: compare the stock command-store barrier

V30 returned safely to Android; final capture and cleanup are archived. V31 tests one concrete stock-versus-modern difference: wmb (ARM64 dsb st) immediately before the first CMD0 halfword store, after the unchanged Qualcomm accessor side effects. It also records use_cdr during transfer setup before the delayed stages. No hardware voltage, clock, DT or init change is introduced. The V30 automatic completion/error/pending branches remain intact.

Build succeeded. The actual state-machine C passed 15 scenarios; the accessor harness verified first-command/board opt-in, unrelated opcodes, transfer state, and preserved CDR/power side effects. Model tests do not emulate hardware. Module/RAM QEMU checks and packaging are in progress; no V31 phone write yet.

See research/storage-ordered-v31-20260916/notes.md for the primary-source search and limits.

The matching module-load and enabled-board RAM QEMU checks passed. Module SHA256 `905225a1bb3883a5a77606c530b4a8315dd234d53e5188eae1b4145a3877d0ee`; adapted kernel `1cac2b81609f4134d74c079381dce76b1cedfbcae2563dcfaf4e935c367c5ce7`; RAM `bb061e54aafa28c986e3c90fb0a2cb85741928b3e7648716f726950b9b6664a2`. Disassembly confirms dsb st before the halfword store. Candidate `0cb8efe9d42e3c1cd8114a6b6d17dde03a12ebb307a78e46a70419d34258d047`.

The first ABL check caught a stale V30 kernel pin in the copied test; corrected to the independently verified V31 hash, with failure archived. The next run hit the unchanged 120-second emulator wall-time limit, with no emulated return. The unchanged check passed on retry; both failure reports retained. No validation assertion was removed or threshold enlarged. Two offline WSL approval checks initially timed out and passed on their permitted retry. Six protocol, four transition and eight collector checks passed.

All nine staged tools and candidate hashes verified on laptop; offline payload/import/XML inspection passed. Installation launched 2026-09-17T06:45:28.944315 UTC, PID199349. Date suffix preserves this preparation series from September 16. Do not retry installation. Await full readbacks and desktop verification. Capture/restore unused.

Installed 2026-09-17T06:46:00.513181 UTC. All twelve copied readbacks passed independent desktop verification. Exact V30 predecessor and known BCB preserved; poweroff acknowledged, EDL absent and host services restored. User asked Power to normal Android; awaiting reply. Install USED; capture/restore UNUSED.

Stock Android baseline, physical port and all nine pinned tools reverified. Capture launched 2026-09-17T06:50:18.924050 UTC PID200466 under GNOME A6L-v31-capture. Exact fastboot 18d1:d00d / 1e529013 and active logger confirmed. User asked Recovery, ~70 seconds, no filming. Capture USED; coordinator owns host-service pause until Android return.

Physical result: 200509 bytes, SHA256 `9208778c6c561ce2e7cbdb68e1b615cef148175e4cd2f2bdf38c69864d75d49b`. USB disconnected 2026-09-17T06:51:55.280645 UTC; capture finished 2026-09-17T06:52:27.177156+00:00. Early accessor trace confirms use_cdr=0 and transfer=0000. Preissue snapshot/status/mask/active PM checks again passed; last heartbeat seconds=32. No step2 or MSM write-boundary markers were delivered. Added barrier did not resolve the failure; absence of buffered messages cannot prove whether the barrier or store executed. User asked Power Android; cleanup pending. V31 remains installed. Next investigation should focus on controller/card setup and fault evidence, not arbitrary added waits.

Normal Android and host-service cleanup verified 2026-09-17T06:53:08.483984+00:00. Final session recopied. Stock reset properties only say bootloader; pstore absent, last_kmsg denied. These do not identify the failure. Saved direct stock/V31 eMMC-node and referenced-supply/pinctrl comparison in research/storage-ordered-v31-20260916/stock-v31-storage-dt.json. No V32 prepared.
