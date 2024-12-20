MVP:

- [x] Parse request path into components
- [x] Parse form fields
- [x] Db
- [x] Proper encoding/decoding with db
- [x] Improve UX for writing logs
- [x] Encode/decode to/from db
- [x] Use sqlite properly for multiple processes
- [x] Clearly split db & html rendering steps
- [x] Cast a vote API (user = `hash(user-agent)`?)
- [x] Collect logs into OLAP db => sqlite/duckdb
- [x] Easy request duration stats (from OLAP db or dtrace/strace) => sqlite/duckdb/utop
- [x] Vendor & compile db code
- [x] Html builder
- [x] Use cookie for user id
- [x] Sanitize user input in html
- [ ] Consider storing the data in a file as a hashtable/treemap
- [ ] Cast a vote UI
- [ ] Content security policy
- [ ] See poll results
- [ ] Pretty UI
- [ ] Test with Chrome
- [ ] e2e tests
    - [ ] Test with non ascii strings
    - [ ] Test with max options and long strings
    - [ ] Monkey testing/fuzz testing
- [ ] Benchmark
- [ ] License
- [ ] Pledge/unveil/mseal

QoL:

- [ ] More robust arena (canaries etc)
- [ ] Close a poll (admin role?)
- [ ] Live poll timer
- [ ] Poll TTL & GC
- [ ] Shorter urls:
    - Use readable english words
- ~~[ ] TLS~~ => In-kernel TLS: https://freebsdfoundation.org/wp-content/uploads/2020/07/TLS-Offload-in-the-Kernel.pdf
- ~~[ ] Slow Loris protections~~ => Pf timeout
- ~~[ ] Maximum number of in-flight requests i.e. workers~~ => `setrlimit(RLIMIT_NPROC,...)` or `rctl -a jail:<jailname>:maxproc:deny:100` for 100
- [ ] Translations
- [ ] Button to copy url to clipboard
- [ ] Embed static files (if any)
- [ ] Retake a poll
- [ ] Poll options (e.g. any user can add options to the poll)
- [ ] QR code for link
- [ ] Crash reporting strategy/Full stacktrace (better assert)
