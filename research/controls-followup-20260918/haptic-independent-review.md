# Independent haptic review

## Finding

The V45 `qcom,brake-pattern` claim is supported by the supplied audit and
driver source. The property is 16 bytes, representing four big-endian u32
cells `[3, 3, 0, 0]`. The original driver calls
`of_property_read_u8_array(..., 4)` at `original.c:870`, so it consumes the
first four bytes of those cells: `[0, 0, 0, 3]`. The candidate's u32 path at
`candidate.c:871-882` reconstructs `[3, 3, 0, 0]`. This is a real data-format
bug in the original parsing, but the evidence does not establish that it
caused the unperceived 100 ms pulse; voltage, PM660 resonance, and startup
behavior remain unresolved hypotheses.

The build and diskless QEMU report support source/module integrity and ABI
load/unload behavior only. They do not exercise a physical haptic register or
provide evidence of perceptible vibration. The report also records that the
phone was not tested and that amplitude and duration were unchanged.

## Candidate review

The intended u32 fix is narrowly scoped and preserves the existing defaults
and the four-byte legacy form. It validates u32 cell values `0..3`, which is
appropriate for the two-bit brake pattern fields. One malformed-property gap
remains at `candidate.c:884-890`: every non-u32-count-of-four property falls
through to `of_property_read_u8_array(..., 4)`. That read can accept a byte
property longer than four bytes and silently consume only its first four
bytes. Thus malformed 8-, 12-, or 20-byte properties can still be accepted;
the candidate does not enforce that the legacy form is exactly four bytes.

A concrete hardening fix is to inspect the property's byte length and accept
only exactly 16 bytes for the u32 form or exactly 4 bytes for the legacy byte
form; reject every other present length. Keep the absent-property case as the
existing default if that compatibility behavior is intended. This is a
review finding, not a reason to claim the current V45 property is malformed.

## Test disposition

The existing `device/hisense/a6l/diagnostic/haptic_probe.c` helper can be
reused unchanged for the candidate runtime check. It sends the same bounded
strong-magnitude `8192` effect for `100 ms`, and the candidate changes only
device-tree brake-pattern parsing. Its self-test documents the resulting
approximately `1276 mV` request and the 100 ms bound. A successful command
run would still require physical confirmation and would not prove that the
brake-pattern correction, rather than another haptic limitation, restored
perceptible vibration.

