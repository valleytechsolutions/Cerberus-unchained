# Passive MAC-header parser

The portable `shared/mesh_mac.c` module parses clear MAC headers for passive
observation. It does not start a radio or implement a Mesh Detection workflow;
capture, transport, inventory, and UI integration are separate work. The reviewed
draft passed 97,536 synthetic assertions with an 80-byte observation on the tested
host. The production checker records the exact interpreter, compiler, source
hashes, and output. No copied CYM implementation or captured network data is
included.

## Contract

`mesh_mac_parse(bytes, length, out)` receives a normalized IEEE 802.15.4 MAC span
without the leading ESP-IDF length byte and without the two-byte FCS replacement.
For the supported 127-byte PHY format the normalized maximum is 125 bytes. The
future callback adapter must validate/normalize the SDK envelope separately and
return each borrowed SDK buffer exactly once. The SDK receive footer is RSSI/LQI,
not an FCS that this parser can recompute. No adapter is implemented here.

`MESH_MAC_HEADER_OK` means only that the supported clear header and MIC extent fit
the supplied span. It does not validate application payloads, payload IEs, frame
authentication, device identity, protocol membership, or complete frame-type
semantics. `MALFORMED` means a required field is truncated or has an inconsistent
declared length. `UNSUPPORTED` means a format or reserved encoding is outside the
declared subset. `INVALID_ARGUMENT` includes NULL and overlapping input/output.
On every failure, output remains byte-for-byte unchanged. Input is never modified.

The output contains only copied scalar/value fields. A typed short address uses
its full 16-bit value; an extended address uses its full 64-bit value. Wire fields
are little-endian, while these numeric values have the natural numeric display
order. No low-byte identity, hash, or guessed address conversion is performed.
PAN origins are `EXPLICIT`, `COMPRESSED_SAME`, or `UNKNOWN`; unknown PAN has a
canonical zero value, which remains distinct from explicit PAN ID zero. The
destination PAN can be explicit even when the destination address is absent.

## Supported subset

- General-format Beacon, Data, ACK, and MAC Command, versions 0/1/2.
- Address modes none, short16, and extended64, including every 2015 PAN-presence
  combination. Legacy compression requires both addresses.
- 2015 sequence suppression. Legacy sequence-suppression and IE bits are rejected.
- Unsecured versions 0/1/2; secured versions 1/2 with nonzero security levels,
  key identifier modes 0/1/2/3, and bounded MIC extents. Version 2 counter suppression
  is supported structurally. ASN-in-nonce is retained as a flag only; no nonce,
  key, security-policy, or counter-authenticity validation is claimed.
- Bounded 2015 Header IEs: unknown IDs can be skipped by their declared length;
  HT2 ends the header list; a nonempty list may end exactly at the MIC boundary.
  HT1 ends the header and requires at least two following non-MIC bytes, but that
  payload-IE region stays opaque. This permits mapping clear addresses without
  parsing encrypted payload IEs or falsely claiming their structural validity.
- Pre-2015 MAC Command ID is a required clear-header byte. For 2015 it remains
  opaque payload. Beacon payload contents are not decoded or validated.

Unsupported cases are version3; reserved address mode1; frame types4/5/6/7;
reserved FCF bit7; legacy sequence suppression/IE bits; legacy PAN compression
with an absent endpoint; legacy ACKs using addressing/security/compression;
secured version0; security level0 with Security Enabled; reserved auxiliary
security bit7; auxiliary bits5/6 before version2; and an unannounced payload-IE
descriptor inside a Header IE list. These are counted as unsupported by a future
consumer, not silently coerced into a different header layout.

Truncation at every required field, a Header IE extending through the MIC or past
the end, partial descriptors, nonzero HT1/HT2 termination lengths, an IE-present
frame with no header IE, or HT1 without a minimum payload descriptor extent are
malformed. Opaque payload bytes can still be malformed at higher layers; a
successful result does not assert otherwise.

## PAN table and mapping limits

| 2015 destination/source | Compression0: D PAN/S PAN | Compression1: D PAN/S PAN |
|---|---|---|
| none/none | absent/absent | explicit/absent |
| present/none | explicit/absent | absent/absent |
| none/present | absent/explicit | absent/absent |
| extended/extended | explicit/absent | absent/absent |
| both present, at least one short | explicit/explicit | explicit/absent |

Only the final compressed 2015 row, or a legacy compressed pair, derives the
source PAN as equal to the destination PAN. The extended/extended special case
does not acquire an invented source PAN. Missing PANs never inherit a prior
packet's PAN, the receiver's configured PAN, or a nearby beacon's PAN.

A later map must distinguish an observed transmitter from an addressed
destination. A destination has not independently been heard. Source-frame RSSI
does not measure the destination. Unknown-PAN short identities must not merge
with explicit-PAN identities. Short0xffff is broadcast and short0xfffe is an
invalid/sentinel value; neither is a physical node. An addressless ACK has no
endpoint association, even if its sequence matches another frame. This parser
does not infer parent, child, router, coordinator, a bidirectional link, distance,
or a Zigbee/Thread identity. Header addresses remain spoofable observations.

## Sources and licenses

The SDK used for the design is ESP-IDF v6.0.1, as reported by the C5 build
metadata. The v6.0.1 OpenThread submodule reference was independently
read from GitHub's contents API: `9b887f6bd1437170fc9bcb726f32185ccb3cb841`.

- ESP-IDF v6.0.1, Apache-2.0: [receive API/envelope](https://github.com/espressif/esp-idf/blob/v6.0.1/components/ieee802154/include/esp_ieee802154.h),
  [frame helpers](https://github.com/espressif/esp-idf/blob/v6.0.1/components/ieee802154/driver/esp_ieee802154_frame.c),
  [frame constants](https://github.com/espressif/esp-idf/blob/v6.0.1/components/ieee802154/private_include/esp_ieee802154_frame.h).
- ESP-IDF-pinned OpenThread, BSD-3-Clause: [PAN presence, clear-header and MIC processing](https://github.com/espressif/openthread/blob/9b887f6bd1437170fc9bcb726f32185ccb3cb841/src/core/mac/mac_frame.cpp),
  [Header IE layout](https://github.com/espressif/openthread/blob/9b887f6bd1437170fc9bcb726f32185ccb3cb841/src/core/mac/mac_header_ie.hpp),
  [address constants](https://github.com/espressif/openthread/blob/9b887f6bd1437170fc9bcb726f32185ccb3cb841/include/openthread/platform/radio.h),
  [license](https://github.com/espressif/openthread/blob/9b887f6bd1437170fc9bcb726f32185ccb3cb841/LICENSE).
- [RFC8180 Appendix A](https://www.rfc-editor.org/rfc/rfc8180.html#appendix-A),
  published May2017: primary examples of HT1, auxiliary counter suppression,
  ASN-in-nonce, and payload IEs. No RFC text or example byte sequence is copied
  into the implementation or fixtures.

These references informed protocol facts and independent tests. This module does
not transplant source functions, test vectors, or prose tables from another
implementation. If later changes copy/adapt external code, retain its
applicable notices rather than representing it as original.

## Tests and focused command

`tools/host_checks/mesh_mac_check.c` provides explicit 18-row 2015 PAN combinations, versions0/1/2,
legacy invalid-compression exclusions, sequence suppression, every shorter
required-header span, all supported security level/key-mode/counter/ASN flag
combinations, MIC and Header IE bounds, opaque encrypted payload, full addresses,
broadcast/sentinel fields, explicit PAN0, legacy ACK/command behavior, alias
rejection, immutable output, and 20,000 deterministic random bounded inputs.
All inputs are synthetic. The suite tests header parsing, not over-the-air
interoperability, topology accuracy, or sustained device load.

From the repository root, using the host-check Python environment with `ziglang`
installed:

```sh
python -B tools/check_mesh_mac.py
```

The harness uses the running interpreter's ziglang compiler, with C11 and
`-Wall -Wextra -Werror`, and writes its executable/cache, stdout log, and JSON
receipt under `artifacts/mesh-mac-checks/`. The receipt binds interpreter/compiler
versions, commands, source hashes before/after, return codes, and stdout hash.
A host parser PASS is not physical radio or live-feature acceptance.
