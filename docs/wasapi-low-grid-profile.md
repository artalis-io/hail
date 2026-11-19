# Predeclared exact low-amplitude WASAPI diagnostic profile

Profile: `transparent-s16-grid-low-v1`. Declared before its first native run.
This is a separate profile, not a passing replacement for the failed original
`transparent-s16-grid` campaign. No numerical tolerance is permitted.

The left sample in S16 units is `marker * 2048 + (frame_sequence & 2047)`;
right is its exact negative. Markers A/B/C are 1/2/3. Values are integers from
2048 through 8191, represented exactly as S16 or F32 divided by 32768. The
11-bit counter wraps every 2048 frames. Every nonzero sample must occupy this
exact grid, have correct channel negation, and advance the counter by one;
old-after-new markers are forbidden. Minimum verified frame counts, metadata,
strict sustained continuity, native flush, stale writes, stop/restart, fault
counters and allocation/retirement requirements remain in force.

The lower amplitude exercises a path below the transformation observed for
the high-level constant diagnostic. It does not qualify near-full-scale
transparency or supply a numerical oracle for Windows S16 conversion noise.
S16 remains an exact, zero-tolerance test and is expected to fail if that
conversion persists. No tolerance will be fitted to the observed +/-1 LSB.

Sequence diagnostics must first retain failures with unchanged sequencing.
Any later transition-boundary correction requires a demonstrated cause and
independent device-free negative tests. It may not excuse nominal sample skips
or unexplained native discontinuities. Trials are explicit and individually
retained; no retry-until-pass loop is permitted.

The initial low-grid F32 run isolated a single post-stop residual-B sequence
failure while all sample values were exact. The control stop resets/discards
pending native and Hail buffers, so continuity of the old tail across that
deliberate discard is not a valid assertion. The corrected oracle still checks
every old-tail value and channel, preserves monotonic marker order, reports
old-tail frame counts, and gives that tail no verified-frame credit. Continuity
becomes mandatory at the first C frame; any subsequent B or C counter skip
fails. Independent negative tests cover both those failures and corrupt tail
samples. Nominal and render-flush continuity requirements are unchanged.

Fixture conditioning is a separate preflight before each trial. Failed tests
may terminate with nonzero samples still queued downstream in the cable;
opening fresh Hail clients does not clear that external storage. The path helper
therefore drives F32 silence and requires at least 24,001 measured exact-zero
frames at Hail reference, native render loopback, native cable F32 and Hail
capture before closing. Its 600 ms setup interval and 900 ms measured interval
are outside the qualification trial. The dynamic oracle starts fresh afterward
with unchanged marker-regression rejection and verified-frame minima. An
unconditioned post-S16 failure and the first failed sustained attempt remain
retained as setup failures, not silently retried passing evidence.
