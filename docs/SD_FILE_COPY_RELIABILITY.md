# SD File Tools copy reliability

SD File Tools copy now treats either stream-close error as failure, prevents temporary-file publication and does not show COPY COMPLETE. The source must open before creating temporary output. Only ENOENT means an absent destination/temp path; other stat errors stop the operation.

Positive short reads continue until EOF. A zero read without EOF, read/write failure, flush, sync, close or rename error prevents success. Each opened stream closes once. Cleanup applies only to the temporary file created by this attempt; the source and existing destination/temp files are not deliberately replaced. The existing storage mutex and firmware-owner gate cover the operation.

Copy remains synchronous on its caller. CANCEL clears the staged clipboard before Paste and cannot interrupt a running transfer. Failed Paste retains the clipboard and shows an error. Temporary removal is best effort; leftover `.CERBTMP` files can block retry and are not silently overwritten.

Fault-injection fixtures exercise the production copy path and UI error propagation. No physical SD fault was induced for this change. Power-loss atomicity, card durability, throughput and a touch-response bound are not established. Move/rename, package writing and global capture ownership are outside this copy fix.

See [CONTRIBUTION_REV93.md](CONTRIBUTION_REV93.md) for the installed local checkpoint, independent contribution validation and remaining physical work.
