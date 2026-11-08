# Rev126: Recon priorities and navigation

Source version is `0.1.0-dev.rev126`; onboard C5 capability revision remains
69. Implementation and independent source/visual reviews are complete. All
136 host suites and five native targets pass on the same 1,120 unchanged
inputs. The linked resource audit and matched local packaging pass. The paired
rev126/capability69 update is installed with verified backups, all image writes
and two successful boot captures. The owner confirmed the revised controls,
ALERT label and stable recognized-hint priority. The completed
[rev125 file-interchange package](DUAL_RADIO_REV125.md) remains a local,
uninstalled checkpoint whose changes are included in this follow-up.

The owner confirmed that the new dashboard appears and works, then requested
priority for recognized observations, a more intuitive layout, a visible radar
choice and the label ALERT instead of ANIMATED ALERT. After rev126 installation,
the owner confirmed Dashboard/Radar switching, Pause/Resume, Back and ALERT,
plus recognized hints remaining first and stable. Recording, every classifier
family and sustained load remain separate acceptance checks.

The dashboard keeps ordinary observations accessible while presenting existing
classified hints first, then confidence and first-consumed time with stable
slot ties. Refreshes alone no longer reorder rows across sources. Dashboard
and Radar have large, explicit tabs on the main Recon page. Recognized category
headings are larger; the Radar legend uses one line per observation with a
complete RSSI value. The graph is labeled ALL LIST CHANGES and retains its
all-observation totals. Radar positions are list slots on an RSSI scale, not
bearing or distance. Back, Pause/Resume, Log, Options and optional SD recording
remain available. Category preferences now say ALERT.

The existing S3 snapshot already retains Meta identifier/context hints, but
rev124's Recon classifier does not consume them: its Smart glasses category is
demo-only. A bounded adapter now reuses accepted current original
advertisement evidence and present it as a Meta identifier hint, without
claiming glasses or model identity. No new signatures, parser or radio activity
is part of this bridge. Source, lease, identity, suppression and stale-evidence
checks remain required and are exercised by actual producer and integration
fixtures. Any existing Watch match takes precedence. Default MED alert
filtering remains unchanged: LOW Meta hints are retained in history/recording
without automatically producing a notification. No new radio operation, task,
observation bank or view/model allocation is added.

Focused validation passes 539 actual-producer Meta projection checks, 4,250
actual Recon UI/journal-adapter checks, 814 lifecycle/capture checks and 89
local rejection/acknowledgment checks. Inventory UI passes 1,178 checks, and
the existing renderer bounds/touch suite passes. These are source/host results,
not physical observations. The final host matrix also passes all 136 registered
suites; S3, onboard C5, bridge, expansion WROOM and expansion XIAO all pass native
builds. Resource measurements, packaging, installation and the owner's targeted
physical acceptance are recorded below.
The full Marauder port and loaded radio/SD performance remain unfinished.

## Retained validation and resources

The final host receipt and its 136 hash-verified logs are retained under
`artifacts/rev126-host-evidence`. Native evidence for all five targets is in
`artifacts/rev126-native-evidence`. Nine actual-renderer previews with synthetic
data are bound to the final host receipt under
`artifacts/device-rev126/previews`; none is a physical screen capture.

`artifacts/device-rev126/native-memory-comparison.json` binds the installed
rev124 baseline receipt and both retained ELF sets. The unchanged model and
operation-allocation assertions pass.

| Resource | Installed rev124 | Rev126 |
|---|---:|---:|
| S3 application image | 1,973,984 B | 1,994,544 B |
| S3 fixed data/BSS | 169,356 B | 169,388 B |
| C5 fixed data/BSS | 83,184 B | 83,184 B |
| Optional Recon inventory/view store | 18,944 B | 18,944 B |
| Recon inventory model | 17,240 B | 17,240 B |
| Recon dashboard presentation | 760 B | 760 B |
| Dual Wi-Fi coordinator | 10,840 B | 10,840 B |
| Dual Wi-Fi live view | 5,664 B | 5,664 B |

Rev126 is 1,104 image bytes above the validated rev125 package and has the same
fixed S3 sections. The optional interchange allocations from rev125 remain:
10,168 bytes for the viewer, 75,784 for an AP JSON worker or 8,080 for an SSID
text worker. Existing BLE/AirTag/native Wi-Fi worker allocations and configured
task stacks remain unchanged. These figures exclude allocator overhead and do
not establish concurrent runtime peaks or fragmentation.

Selected native entry frames include 1,088 bytes for the UI loop, 320 for
`watch_draw` and 224 for `sentinel_watch_view_draw`, which includes the inlined
Recon dashboard body (16 above rev124). The audit's separate 160-byte
`draw_dashboard` symbol belongs to the general system dashboard, not Recon.
A separately bound retained-ELF inspection in
`artifacts/device-rev126/recon-frame-supplement.json` measures
`watch_consume_radio` at 192 bytes in both rev124 and rev126; its linked code
grows by 474 bytes. Some helpers are inlined and have no standalone measurement;
they are not treated as free. The audit is not a complete call chain or
worst-case runtime stack bound. Physical navigation, priority behavior
over real sweeps, repeated sessions, SD workflows and loaded performance remain
separate acceptance checks.

## Final package and device readiness

Built source commit: `d679ba0c3d4307330c5d29d3ce2e546e359d2635`.
Package: `dist/cerberus-unchained-0.1.0-dev.rev126-20261101T223739Z`.
Manifest SHA-256:
`84d3f81d2d0d0f126e3e96996e56f3d9c850d0352dd6eea4a44f8025efaebca5`.
`artifacts/device-rev126/local-milestone.json` binds the source, all nine images,
host/native receipts and resource audit. Its `installed: false` is intentional.

The candidate-bound six identity tests and 22 installation-helper tests pass.
Offline preflight verifies the exact package and all helper hashes. Initial
readiness checks found an active Recon session. A transient idle C5 sample was
insufficient: S3 remained initialized, and a later sample caught the next active
sweep. No reset or write occurred until the owner reported returning to Home
and a fresh capture passed all warm ownership checks.

The operator/recovery plan is `artifacts/device-rev126/INSTALL.md`. Documentation
after the built source commit records completed packaging and readiness only;
the strict installer requires the exact clean built commit before device work.
No push, pull request or release was made for this checkpoint. The overall
Marauder port remains unfinished.

## Completed local installation and owner acceptance

The owner returned to Home, and a fresh inactive capture authorized the guarded
non-flash reset. The original clean checkout was temporarily selected at the
exact built commit for installation, then returned to its development branch.
Fresh 2 MiB S3 and C5 backups match the installed rev124 images and passed full
prefix verification. All nine writes across the temporary bridge, onboard C5
and S3 application passed image-hash verification. Higher flash and the prior
crash record were retained.

The final receipt is `artifacts/device-rev126/installation-receipt.json`, SHA-256
`7feb116b5eaf8f1e29f4b4358c553032eb3fd61e8aff21955c182d77cf7dc718`.
The immutable earlier `local-milestone.json` still records its original
`installed: false`; the installation receipt records the later device event.

The two boot captures are 53.440668 seconds apart. They confirm rev126/C5
capability69, advancing uptime, C5 boot count 1, buffered display, mounted SD
and no previous reset fault. The initial READY timeouts and legacy fallbacks
remain at their startup baseline of four each; mutex, READY fall/high and driver
errors are zero and no listed SPI error counter increased between captures.

Both captures report 137,580 free internal S3 bytes, a 94,208-byte largest
internal block, 1,427,552 free PSRAM bytes and ten tasks. The observed console
low-water mark is 1,420 bytes after the settled capture. These sampled idle
figures do not establish peak memory, worst-case stack or loaded performance.

The owner then reported “All controls work; ALERT appears” and “Recognized hints
stay first and stable” in response to the explicit rev126 checks. This confirms
the targeted Dashboard/Radar, Pause/Resume, Back, label and ordering behavior;
it does not establish every classifier's accuracy, SD recording, continuous
loaded operation or complete Marauder feature parity. The report is retained
in `artifacts/device-rev126/owner-acceptance.json`.
