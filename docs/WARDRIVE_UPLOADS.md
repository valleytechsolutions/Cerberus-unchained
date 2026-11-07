# Wardriving logs, home Wi-Fi and uploads

Stop the capture, then open **Scan → Wardriving → LOGS / UPLOAD**. Pick a file
to see its row counts and the upload status for each service. Choose **UPLOAD
THIS LOG** to send it, or **UPLOAD ALL LOCATED LOGS** from the file list to send
several. Both actions ask for confirmation before connecting to your home Wi-Fi.

**UPLOADED** means the service acknowledged every part of the file. You can
reopen the log to see its saved receipts. [Results and retries](#results-and-retries)

## Import one JSON profile from SD

1. Copy [wardrive_upload.example.json](../config_examples/wardrive_upload.example.json)
   to your PC and fill in the home network and service fields. The distributed
   example contains empty values only. Keep your filled copy private.
2. Set `enabled` to `true` for each destination you want to use. WiGLE needs
   both `api_name` and `api_token`; WDGWars needs its 64-character hexadecimal
   token; Wardrift needs its token. Leave unused services empty and disabled.
3. Save as UTF-8 without a byte-order mark, named **UPLOAD.JSON**. Copy it to
   **/CERBERUS/CONFIG/UPLOAD.JSON** on the SD card. Create `CONFIG` if necessary.
   Safely eject the card or USB drive and return SD ownership to Cerberus.
4. Open **LOGS / UPLOAD → HOME WI-FI / API SETUP → IMPORT JSON FROM SD**.
   Review the replacement notice and tap **CONFIRM IMPORT**. This replaces the
   entire saved upload profile, including fields left empty or disabled.
5. Wait for **PROFILE SAVED / SD COPY REMOVED**. Reopen setup to check the
   SET/EMPTY labels and enabled destinations. The saved settings persist across
   restarts. Importing or saving never starts a connection or upload.

The JSON contains these exact sections:

```json
{
  "schema": "cerberus-wardrive-upload-v1",
  "home_wifi": { "ssid": "", "password": "" },
  "wigle": { "enabled": false, "api_name": "", "api_token": "" },
  "wdgwars": { "enabled": false, "token": "" },
  "wardrift": { "enabled": false, "token": "" }
}
```

All sections and fields are required; comments, unknown or repeated keys and
incomplete enabled destinations are rejected. Maximum file size is 4,096 bytes.
Limits are measured in UTF-8 bytes: SSID 32, password 64, WiGLE API name 64,
WiGLE token 128, WDGWars token exactly 64 when present, and Wardrift token 128.
JSON supports UTF-8 SSIDs even though the on-device keyboard is ASCII. The home
AP must offer 2.4 GHz; the authentication limits below still apply.

Malformed JSON leaves saved settings unchanged. The worker saves valid settings,
reads them back and compares them before removing the SD copy. If saving or
verification fails, the SD file stays available for review/retry. **PROFILE SAVED
/ SD COPY REMAINS** means settings were verified but SD cleanup failed; remove
that file after checking setup. File removal is not a secure erase. The private
PC copy also remains under the user's control. Filled profile filenames are
ignored by Git; never include credentials in an issue, PR or firmware image.

The read-only `upload_profile` console command reports readiness flags and the
enabled-destination mask only. It does not print the network name, password,
tokens or credential fingerprints. The rev141 read-only `upload_status` command
adds operation/stage, service, HTTP status, response length/completeness and
transport error. It exposes no response contents or transaction IDs. These
current-attempt diagnostics are not persisted in the receipt journal.

## Set up home Wi-Fi and services

1. Open **Scan → Wardriving**, finish recording with **STOP**, then open
   **LOGS / UPLOAD → HOME WI-FI / API SETUP**.
2. Tap **HOME WI-FI SSID**, enter the exact network name and tap **DONE**.
   Enter **HOME WI-FI PASSWORD** the same way. Upload uses the S3's **2.4 GHz**
   station interface; a 5 GHz-only AP cannot be used. A blank password means an
   open network. Enterprise authentication and captive-portal sign-in are not
   implemented. The current keyboard enters ASCII.
3. For **WiGLE**, obtain the **API name and API token** from your account's
   [API page](https://wigle.net/account?account=api). Enter those two fields,
   rather than your normal account password.
4. For **WDGWars**, generate the 64-character hexadecimal API key in your
   profile and enter **WDGWARS TOKEN**. See the provider's
   [developer guide](https://wdgwars.pl/press).
5. For **Wardrift**, enter your device/API key in **WARDRIFT TOKEN**. This
   provisional adapter follows [Marauder's Wardrift contribution](https://github.com/justcallmekoko/ESP32Marauder/pull/1634),
   revision `0b6ac93f996beafb69070fe04b68656ce604c2d0`, separate from the pinned
   develop tree. Wardrift's homepage did not expose readable API documentation
   during this audit; live account verification remains required.
6. Toggle **WIGLE**, **WDGW** and **DRIFT** ON for the desired services, then
   **SAVE SETUP**. Destinations default OFF. Saving does not connect or upload.
   **CANCEL SETUP / LOGS** discards unsaved edits.

Password/key entry is masked, with an explicit reveal control. Setup reports
SET/EMPTY without showing saved values. A dedicated `ward_upload` NVS namespace
stores a versioned, corruption-checked blob. Keys/passwords are excluded from
CSV, receipts, general settings exports and application diagnostic messages.
This build does **not** enable NVS encryption or burn security eFuses; physical
access can expose credentials. **FORGET HOME AP AND ALL KEYS** confirms removal
of this profile, leaving captures and receipts intact. Rotate compromised keys
with their providers.

## Review, upload and delete

For a new recording, wait for **GPS DATA > FIX + UTC / WIGLE CSV**, tap START,
and check that **SD: ... ROWS** increases. Stop and wait for **CSV SAVED / OPEN
LOGS BELOW**, then open **LOGS / UPLOAD**. A successful radio scan alone does
not mean a CSV was recorded. Rev144 keeps **STOPPED / NO CSV SAVED** and the
reason visible if no eligible observations were written. An empty log list
explains this sequence; an SD catalog error is shown separately. The read-only
`ward_logs` console diagnostic distinguishes absent directories, CSV entries,
temporary captures and filenames excluded by the native-name filter.

Production recording saves one **CERBWardrive001.CSV**–**CERBWardrive999.CSV**
in `/CERBERUS/WARD` per session. The first qualifying Wi-Fi/BLE observation opens
it; Stop flushes and finalizes it. It uses the standard WiGLE 1.6 schema and
preserves repeated valid sightings. Scanning continues without a GPS fix, but
no row is saved until position and UTC are valid. Missing GPS/UTC, uncertain RF
age and unsupported format have explicit skipped counters. No new UNLOC file is
created, and a session without qualifying rows produces no empty CSV. The
existing 64 MiB limit ends the recording with a size-limit message and preserves
its one file instead of rotating another. Failed writes retain recoverable TMP
files and are not published as completed CSVs.

The paged list includes these names plus finalized legacy `WIGLE001.CSV`–
`WIGLE999.CSV` and `UNLOC001.CSV`–`UNLOC999.CSV`, up to 2,997 files. New-format
names appear first, then legacy WIGLE, then legacy UNLOC, with descending names
within each group. This is not a timestamp timeline. Selected-file details validate complete CSV
contents and show size and rows. Located files also show 2.4/5 GHz Wi-Fi and BLE
counts, earliest CSV UTC, missing-measurement warnings and destination receipts.
Legacy UNLOC entries are labeled **LEGACY NO GPS**; their details explain that
they cannot upload. Their radio subtotals are unavailable and no UTC/location is
invented. Old files are not renamed, rewritten or deleted by the update.

- Choose **UPLOAD THIS LOG** on a valid located file, or **UPLOAD ALL LOCATED
  LOGS** on the list. Review count, selected services and GPS/identifier
  disclosure, then **CONFIRM UPLOAD**.
- **DELETE THIS LOG** and **DELETE ALL FINALIZED LOGS** require separate
  confirmations bound to the reviewed catalog and SD lease. They exclude `.TMP`
  captures, unrelated folders/assets, credentials and receipts.
- Upload retains the original CSV. **CANCEL OPERATION** stops further work;
  **BACK / CANCEL AND CLOSE** waits for cleanup. Pending socket work can take
  its SDK timeout to exit. Canceling bulk deletion cannot restore files already
  deleted; the processed count records the partial operation.

Uploads establish UTC before contacting a service. A synchronization from this
boot can be reused for 15 minutes while the wall and monotonic clocks agree.
With GPS attached, open Wardriving and wait for **FIX + UTC** before opening
Logs / Upload; its recent NMEA time remains usable after leaving that screen.
This time handoff does not require recording a new log or keeping GPS running.

Without recent time, the uploader tries the NTP pool and Cloudflare's time
server over home Wi-Fi, up to 15 seconds each. **UTC TIMEOUT / LOG NOT SENT**
means that attempt never reached an upload service. An older **CHECK ACCOUNT**
receipt remains unchanged and still requires the explicit retry confirmation.
Connection, clock or certificate failures stop the upload. Certificate checks
remain enabled. Captures more than 60 seconds ahead of synchronized UTC are
rejected.

Unlocated logs stay local and are skipped by Upload All. Legacy pre-headers
missing Earth metadata remain inspectable but require PC export/review; the
device does not silently rewrite them. A malformed/truncated located file stops
the pass. Review it on a PC or select a valid file individually. Missing accuracy
or altitude remain warnings; HDOP is never substituted for measured accuracy.

## Results and retries

| Result | Meaning |
| --- | --- |
| NOT SENT | No matching receipt for these contents and credentials. |
| SENDING | Durable local intent, not a service acknowledgement. |
| IN PROGRESS / INCOMPLETE | At least one part has a receipt, but the entire log has not been confirmed for this service. The first label is used while the worker is active; the second is used after it stops. |
| UPLOADED | The complete service reply confirms receipt: WiGLE returned `success: true` and a transaction ID; WDGWars returned HTTP 202, `ok: true` and a positive job ID, or synchronous `ok`/import counts; Wardrift returned `accepted: true`. Upload receipt alone does not establish processing or account credit. WDGWars imported/rejected-row counts are shown separately when provided. |
| REJECTED | A complete recognized reply explicitly rejected the request. Correct its cause before manually retrying. |
| CHECK ACCOUNT | Ambiguous delivery, damaged receipt or unexpected response. No automatic resend. |

Both the progress screen and saved log details use **UPLOADED** for confirmed
service receipts. Asynchronous receipts also show **UPLOAD VERIFIED; IMPORT MAY
BE PENDING**. Existing receipts display the new label without another upload.
The journal retains its distinct `QUEUED` and `ACCEPTED` values; this is a display
change, not a receipt migration or new server-status check. The UI waits until
every part of the log has a confirmed receipt for that service. A partial upload,
incomplete reply, rejection or uncertain delivery cannot become **UPLOADED**.

Acknowledged parts are skipped on later uploads with the same contents and
credentials. Observation rows are never deduplicated. Receipts bind SHA-256 of
the **whole original file**, destination, credential fingerprint and versioned
part plan. Rotating a key creates a new receipt identity: check the account
before uploading old captures again. Receipts contain no raw keys, passwords,
observed MACs or coordinates.

After ambiguous delivery, check the provider account first. WiGLE transaction
IDs are retained in the journal; automatic transaction polling/reconciliation
is not implemented. Export `/CERBERUS/WARD/UPLOADS` and the original for PC
review. Do not remove receipts just to force retry without checking the account.
If a saved uncertain receipt blocks a retry, rev141 names the service, for
example **WDGWARS / CHECK ACCOUNT FIRST**. Rev140 could leave **VALIDATING SAVED
CSV / ESP_ERR_INVALID_STATE** on this path despite valid CSV and saved settings.
Rev143 presents **RETRY WDGWARS UPLOAD** directly on the affected log's details
when one enabled service has an uncertain receipt. Tap it, read the file/service
scope and **CONFIRM SERVICE RETRY**. Cancel returns to the log without sending.
If multiple services are uncertain, **UPLOAD THIS LOG** opens the service
choices; select the affected service's **REVIEW RETRY** button. The earlier
attempt may already have arrived, so this can resubmit its data. Only the chosen
service is attempted; acknowledged parts and other services are not resent.
Cancel does not send. This does not clear journals, override corrupt receipts,
or enable retries across Upload All. The original CSV and journal history stay
on SD. A disabled service must first be enabled in setup.

WDGWars uploads now use `/api/v2/upload-csv`, matching the current Marauder
development implementation. A valid 202 job receipt is retained internally as
`QUEUED` and displayed as **UPLOADED**;
processing is asynchronous and automatic job-status polling is not implemented.
See the [rev142 API references](DUAL_RADIO_REV142.md) and
[rev143 recording/retry scope](DUAL_RADIO_REV143.md).

Interrupted journal writes and invalid CRCs fail closed. Authentication/rate
errors stop the pass without automatic retry. Requests are sequential, at least
2.5 seconds apart. No synthetic test records are uploaded to production services.

## Implementation and contracts

S3 owns UI, SD, home Wi-Fi and HTTPS. C5 does not duplicate uploads. Receive
allocation is unchanged: S3 2.4 GHz plus C5 5 GHz for Wi-Fi, S3 Wi-Fi/BLE windows
with C5 on 5 GHz in combined mode, and both receivers in BLE-only mode.

The worker and 16 KiB internal stack exist only while this branch is open.
Catalog, transfer workspace and TLS allocations use PSRAM. Transfers stream
4 KiB buffers and split files at complete rows into parts containing at most
1 MiB of data plus the original two headers. This is below WDGWars's documented
40 MB cap, preserves every observation and avoids full-file RAM allocation.
Wardrift's current limits still need live verification.

| Service | Request | Acknowledgement |
| --- | --- | --- |
| WiGLE | `POST https://api.wigle.net/api/v2/file/upload`; Basic API-name/token; multipart `file` | HTTP 200, `success: true`, exactly one transaction ID; UPLOADED, server processing unverified |
| WDGWars | `POST https://wdgwars.pl/api/v2/upload-csv`; `X-API-Key`; multipart `file`; `Accept: application/json` | HTTP 202, `ok: true`, positive `job_id`: UPLOADED, server processing unverified; synchronous HTTP 200/import counts remain supported |
| Wardrift | `POST https://wardrift.net/v1/ingest/signals`; `X-API-Key`, `X-File-Name`; raw `text/csv` | HTTP 200, `accepted: true`; provisional until live verification |

HTTPS verifies CA chain/hostname and disables redirects. Complete responses are
bounded to 4 KiB and parsed structurally. HTML, truncated JSON, duplicate/escaped
decision keys, unexpected schemas and HTTP 200 service errors cannot mark success.

Primary references:

- [WiGLE CSV 1.6](https://api.wigle.net/csvFormat-1_6.html)
- [WiGLE upload response](https://github.com/wiglenet/wigle-wifi-wardriving/blob/main/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/model/api/UploadReseponse.java)
- [WiGLE endpoints](https://github.com/wiglenet/wigle-wifi-wardriving/blob/main/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/util/UrlConfig.java)
- [WDGWars client and contract](https://github.com/LOCOSP/pineapple_pager_wdgwars/tree/main/wdgwars/uploader)

Host fixtures exercise actual C codecs, leased storage/worker, SDK adapter and
touch handlers with synthetic inputs. They do not establish physical Wi-Fi/TLS
interoperability, service credit, power-loss durability or touchscreen acceptance.
