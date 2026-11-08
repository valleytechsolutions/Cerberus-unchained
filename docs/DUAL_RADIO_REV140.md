# Rev140: one private upload profile

This milestone adds a single JSON profile for home Wi-Fi, WiGLE API name/token,
WDGWars token and Wardrift token to the existing Wardriving upload setup. It
changes no scan allocation or attack behavior. S3 owns parsing, NVS and SD;
C5 remains available for its existing receive work after this branch closes.

The [blank example](../config_examples/wardrive_upload.example.json) and
[setup guide](WARDRIVE_UPLOADS.md) describe the exact schema and SD path.
**IMPORT JSON FROM SD** appears in the existing setup screen and confirms
replacement of the whole saved profile. Parsing checks the complete bounded
JSON, exact fields, UTF-8, field lengths and enabled-service readiness before
writing. The worker verifies NVS readback before removing the temporary SD
copy. Failures retain the file; saved-but-not-removed is a distinct result.
No connection or upload starts as a side effect of setup.

The parser reuses the bounded tokenizer used for service replies; it does not
add a general-purpose JSON heap or a persistent task. Existing upload worker
scratch is cleared after parsing, and temporary decoded/settings copies are
cleared after use. The read-only `upload_profile` console command exposes only
readiness flags and the destination mask. The public example has no credentials;
filled local profiles and physical evidence remain outside Git.

Focused checks cover 52 JSON/encoding/bounds cases, the actual NVS adapter and
secret-free diagnostic, 173 existing upload-protocol cases, actual setup/import
touch handlers/rendering, and 12 manager/storage lifecycle cases. Import cases
include invalid input, save failure, readback failure, removal failure and
cancellation after a completed save. They exercise the real worker and leased
storage with synthetic credentials, without accessing a live service.

## Build and installation checkpoint

All **166 registered host suites** and **five native targets** pass on the same
1,299 unchanged inputs at source commit
`8ed554564c6d8deeacd25f45a010a5001357dbea`. The full run exposed three older
console fixtures that needed an isolation stub for the new command; those were
corrected before the passing run. Windows fixture-directory permission failures
were resolved by running with the required local access. The 21 CI shard checks,
20 build/package failure-path checks and SD-resource checks also pass.

Native interrupt, console-stack and C5 parser placement gates pass. The new
console handler has a 560-byte native frame. The reviewed known console call
chain leaves 1,856 bytes of the 4,096-byte task stack; this remains a partial
static bound, not a bound on every SDK call or interrupt path.

The matched development package is
`dist/cerberus-unchained-0.1.0-dev.rev140-20261105T034853Z`. All 22 listed package
files pass checksum verification. Manifest SHA-256:
`3e1141bddf3df131ee0c3b5fece18e4b5722f2110f08d15c7643df5f44edb538`.
S3 app size is 2,325,648 bytes, 5,312 bytes above rev139; C5 is 1,728,736 bytes
and the bridge is 255,296 bytes. Immutable pre-install evidence is retained in
`artifacts/rev140-validation-final/local-milestone.json`.

Rev140 is installed on the identified device. Fresh recovery backups cover
2,392,064 S3 bytes and 2,097,152 C5 bytes, including every erase-rounded write.
Six exact-loader identity checks pass. All nine bridge/C5/S3 image writes verify
their programmed digests. The installer performs no blanket erase or NVS rewrite.

Two post-install captures 77.78 seconds apart confirm mounted SD, C5 capability
75 online, released owners and no previous-reset fault. S3 internal free memory
stays at 135,344 bytes, its largest block at 94,208 and free PSRAM at 1,426,140;
ten tasks are observed. These values match the sampled rev139 baseline. C5
reports 53,460 free bytes. Console stack low-water reaches 1,168 bytes after
diagnostic use; these are sampled values, not loaded-operation guarantees.

SPI exchanges advance from 151 to 496 with unchanged C5 boot count and no
additional listed transport errors. Four initial READY timeouts and four legacy
fallbacks remain visible at both samples. The private installation receipt is
`artifacts/device-rev140/installation-receipt.json`, SHA-256
`4298dcc8ea158d066e8a6d02d850cd9616865ac1fe4b19f6440cdf0b9cb6f9bf`.

The installed read-only profile diagnostic initially reported all readiness
flags OFF. A private profile was prepared, validated with the production parser
and copied to the owner's SD card. After the owner's interaction, the console
reported home/WiGLE/WDGWars ready, with destinations mask `0x03`. Physical SD
inspection confirmed removal of the imported JSON and unchanged recorded CSV.
Profile import and SD cleanup are verified. A controlled restart on the exact
board then preserved all three readiness flags and mask `0x03`, with no previous
reset fault and C5 online. This verified persistence while the card was on the PC;
it did not initiate a connection or upload. Private credentials are absent from
the firmware source, example, diagnostics and PR.

The measured capture and log-navigation baselines remain
[rev138](DUAL_RADIO_REV138.md) and [rev139](DUAL_RADIO_REV139.md). The SD journals
also show a live WiGLE queued acknowledgement and an uncertain WDGWars result.
A read-only provider query found the matching WiGLE transaction, confirming
the home-network/TLS/upload path for that attempt. Processing/credit, WDGWars
delivery and upload cancellation remain separate acceptance tasks. The misleading
retry-stage label and follow-up diagnostics are addressed in
[rev141](DUAL_RADIO_REV141.md). The Wardrift adapter remains provisional. This is
not a claim of complete Marauder parity or encrypted credential storage.

The contribution incorporates upstream main through `db1dfdc`, preserving its
published rev136 installer, firmware and SD downloads. The README distinguishes
that published release from this development source; no rev140 binary release
is published by this checkpoint.
