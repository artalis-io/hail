# Continuous incremental transcription, 2026-11-19

**Whisper and Parakeet can support a continuously serviced microphone, but their
ordinary vLLM transcription endpoints do not provide that complete behavior.**
Both need a streaming policy above, or alongside, the recognizer. For our first
comparison, evaluate Parakeet TDT v3 through NeMo's buffered streaming path and
Whisper through SimulStreaming. Keep the existing text-generation service as
the conversation backend. This is an investigation/recommendation, not a claim
that either speech path has run on the Spark.

## Distinguish input streaming from output streaming

Three different things are often called streaming:

| Behavior | What it establishes |
| --- | --- |
| Upload an entire clip, then receive transcript tokens over SSE | Incremental output only; the recognizer already has the input |
| Send bounded overlapping windows while continuing microphone capture | Online service built around an offline recognizer; needs boundary handling and transcript stabilization |
| Feed audio chunks into a persistent model/session interface while decoding | Streaming input; still needs endpointing, queue bounds, interruption and finality rules |

Continuous capture does not require irrevocable text for every microphone
quantum. Tentative words may change as context arrives. Preserve the beginning
of speech, expose revisions explicitly, and finalize an utterance at a declared
endpoint. Hail's callback/worker must continue moving PCM independently of ASR
or network progress.

## Model and serving paths

| Path | Continuous microphone strategy | Constraint and judgment |
| --- | --- | --- |
| Whisper on ordinary vLLM transcription API | Repeated bounded audio windows plus an external stabilization/endpointing layer | Uploaded-file transcription and token streaming do not themselves accept an indefinitely growing microphone input |
| Whisper + SimulStreaming | Maintained simultaneous-decoding middleware, with AlignAtt/LocalAgreement policies and a microphone/server interface | Concrete streaming implementation to compare; separate PyTorch runtime integration and target-node qualification required |
| Parakeet TDT v3 + current vLLM plugin | Plugin exposes transcription of supplied audio; a streaming wrapper would need separate implementation | Plugin implements `SupportsTranscription`, not `SupportsRealtime`; no microphone append protocol established by this plugin |
| Parakeet TDT v3 + NeMo | Chunked encoder windows, left/right context and stateful transducer decoding | NVIDIA documents this path for this checkpoint; tune context/latency against accuracy and actual service scheduling |
| Qwen3-ASR + vLLM realtime adapter | Persistent WebSocket append/commit input and text deltas | Already present in installed source, but its hardcoded five-second segmentation is a poor starting point for low-latency partials |

Whisper's authors did not design the original model as a realtime recognizer.
The [Whisper-Streaming project](https://github.com/ufal/whisper_streaming)
adds agreement-based commitment and points new users to
[SimulStreaming](https://github.com/ufal/SimulStreaming). The latter provides
continuous microphone/server operation and simultaneous decoding policies.
Its performance claims are upstream claims, not measurements on our node.
Simply cutting consecutive non-overlapping clips risks truncated words and
inconsistent boundaries; overlapping windows need duplicate removal and a
stable/replaceable text distinction.

The [Parakeet TDT v3 model card](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3)
specifically points to NeMo streaming inference. Its example uses ten seconds
left context, two-second chunks and two seconds right context. The
[NVIDIA streaming implementation](https://github.com/NVIDIA-NeMo/Speech/blob/main/examples/asr/asr_chunked_inference/rnnt/speech_to_text_streaming_infer_rnnt.py)
explains the buffering tradeoff: chunk duration plus right context gives the
algorithmic delay before compute/transport. The example therefore starts at
about four seconds of algorithmic delay. Smaller validated windows are an
experiment, not an automatic accuracy-preserving speedup. Offline TDT v3 plus
buffering must not be described as a cache-aware streaming-trained checkpoint.

The [vLLM Parakeet plugin](https://github.com/Alex-Wengg/vllm-parakeet-plugin),
reviewed at `d673a14ef8216114fc7c4f9069192b02202a6220`, takes supplied audio in
its generation prompt and has no `SupportsRealtime` implementation. Its
dependency declaration requires `vllm[audio]>=0.32.0`. The Spark's deployed
version identifier is a custom development build, not evidence of satisfying
that requirement. Neither API compatibility nor ARM64/GPU execution was tested.
Installing into the existing attested text runtime would be an unjustified way
to discover compatibility; qualify a separate runtime if this route is pursued.

## What the installed Spark actually contains

Read-only SSH inspection examined the installed vLLM source under runtime
`vllm-0.1.dev21830+g8244d1c44.d20261108+cp312+linux_aarch64+cu13+wheelset-b64f75177620`.
Source hashes and structured observations are retained in
[streaming-source.json](evidence/inference-node-2026-11-19/streaming-source.json).
No speech weights were downloaded, no ASR model was loaded and no service or
microphone settings were changed. Existing `qwen38-fp4` health remained good.

* `model_executor/models/registry.py` registers Whisper transcription and
  `Qwen3ASRRealtimeGeneration`. A file named `parakeet.py` is a projected encoder
  helper, not a registered Parakeet TDT transcription service. Similarly,
  `whisper_causal.py` by itself does not establish realtime support for standard
  Whisper checkpoints.
* `qwen3_asr_realtime.py:buffer_realtime_audio` sets its segment duration locally
  to **5.0 seconds**. Ordinary ongoing input cannot produce its first complete
  audio segment before that much audio arrives. Ending input earlier flushes
  the remainder; it does not make ongoing transcription subsecond. The
  inspected function exposes no session parameter for reducing this duration.
* `RealtimeConnection` accepts PCM16 mono 16 kHz append events, a model update,
  a commit to start generation and a final commit to end input. It returns text
  deltas and a final transcript. This is a limited transcription protocol, not
  an established full OpenAI Realtime session contract.
* Its incoming `asyncio.Queue()` has no item limit, and append uses
  `put_nowait`. Per-message checks do not bound total queued audio during slow
  inference. Cleanup cancels the generation task, but no measured engine/GPU
  cancellation bound was established. The protocol has no speech-start/VAD,
  partial replacement range or response-cancel acknowledgement event.
* The existing authenticated gateway exposes neither these WebSockets nor audio
  endpoints. A separate bounded, authenticated session boundary remains needed.

These are source-confirmed constraints, not reproduced production failures or
performance measurements. They explain why enabling a route alone would not
meet the Hail client's latency, finality and bounded-storage contracts.

Qwen's own [streaming wrapper](https://github.com/QwenLM/Qwen3-ASR/blob/main/qwen_asr/inference/qwen3_asr.py)
is another implementation: it accumulates utterance audio and re-decodes with a
rolled-back text prefix, defaulting to two-second chunks. It is not the installed
vLLM realtime adapter. Its single-stream/no-timestamps interface and growing
utterance history require explicit limits; switching to that wrapper is not a
configuration-only fix to the installed adapter.

## Client/session contract required for either recognizer

The external adapter should publish utterance ID, monotonically increasing
revision, current hypothesis, stable prefix and a separate final indication.
Appending every hypothesis would duplicate words and make corrections
impossible. Silence, cancellation and input discontinuity must remain distinct
from successful final recognition.

Endpointing/VAD should run independently of recognition. Speech-start can
initiate local playback interruption immediately; waiting for stable ASR words
would add recognition delay to interruption. Use retained pre-roll so the first
phoneme is preserved. Echo rejection/AEC remains necessary for speaker operation;
the first conversation uses headphones.

Bound audio backlog by duration/bytes, utterance duration, hypothesis size and
concurrent sessions. Admit only supportable streams, pace accepted input, and
report overload or gaps explicitly. Never block Hail audio execution waiting
for inference. On reset/disconnect invalidate dependent state and reject late
results for the old utterance. Keep capture clock/position, text revision,
playback generation and remote response identity separate.

## Next executable comparison

1. Provision a separate declared speech runtime; start with Parakeet/NeMo's
   documented buffered path and compare Whisper/SimulStreaming. These are two
   test candidates, not two mandatory production dependencies.
2. Replay a small licensed/owner-approved speech fixture at wall-clock speed,
   with pauses, word boundaries, silence and a continuous passage. Include
   English/Hungarian if those are the product languages; both model families
   offer language coverage, but actual accuracy must be measured on the fixture.
3. Record first tentative word, stable-word lag, finalization lag after speech
   end, revisions, duplicated/missing words, recognition accuracy, peak queued
   audio and memory. Start with the documented context geometry, then vary valid
   chunk/lookahead settings under a predeclared matrix. No latency target has
   yet been accepted; report distributions instead of claiming a pass.
4. Repeat the chosen profile while the existing LLM streams; verify processing
   stays ahead of input and cancellation/reset clears old work without losing
   new speech. Test stalls and bounded rejection before opening a microphone.
5. Only then run the headset microphone path through Hail and the authenticated
   session service. That establishes microphone behavior separately from paced
   file replay and source inspection.

The investigation is complete; live incremental ASR qualification remains open.
