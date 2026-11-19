# Inference node capability assessment, 2026-11-19

**The inspected Spark can supply streamed conversation text today. Its managed
deployment does not yet supply the ASR, TTS or duplex session interfaces needed
for a headset conversation.** This completes the initial capability assessment
in [roadmap milestone 2](roadmap.md#delivery-milestones-and-acceptance); speech
runtime selection and a tested session contract remain prerequisites to milestone 3.

## Evidence and provenance

Inspected the configured SSH host `spark-7468`, its installed managed-service
definitions, gateway source and running gateway identity, backend model/config,
health, OpenAPI paths and HTTP behavior. The probe observation began at
2026-11-19 11:09:36 UTC. Raw, filtered observations are in
[capabilities.json](evidence/inference-node-2026-11-19/capabilities.json).

The local appliance checkout at
`C:/Users/Mark/herdingsparks.com/herding-sparks` is at `45dfa23` with unrelated
changes. It is older than the deployed tools and was left untouched. Its README
is not evidence of the live feature set. The installed gateway source hash
`2c49a8786330c6627c68a5bcc092d31a4458010c43e6f6ac8705aa651ad088d0`
matches the running gateway's reported code identity; status reports no stale
artifacts. Installed source was inspected locally; private appliance source is
not copied into this repository. This is a capability snapshot, not a complete
runtime attestation or model-weight verification.

[`probe_inference_node.py`](../tools/probe_inference_node.py) repeats the bounded
inspection over an existing SSH configuration. It reads no credentials and
installs, starts, stops or changes no model/service. Its optional text smoke uses
one synthetic prompt with a 16-token ceiling on the loopback backend. No audio
device, microphone recording or audio upload is involved.

## Observed deployment

| Capability | Evidence | Status for the speech client |
| --- | --- | --- |
| Active inference | `qwen38-fp4`, vLLM, loopback port 8086; health and model identity pass | Available for conversation text |
| Model | `/v1/models` advertises `qwen38-fp4`, maximum context 262,144; config identifies `Qwen4ExpForConditionalGeneration` / `qwen4_exp` | Alias and configuration observed; no speech capability inferred from the model name |
| Runtime | `vllm-0.1.dev21830+g8244d1c44.d20261108+cp312+linux_aarch64+cu13+wheelset-b64f75177620` | Observed deployed identity; not the older runtime in the local README |
| Text streaming | Direct backend POST `/v1/chat/completions`: HTTP 200, `text/event-stream`, content `OK`, terminal `[DONE]` | Exercised, one request at a time |
| Gateway | `hs-gateway` healthy on loopback port 8780; configured ingress `api.spark-7468.artalis.io` | Existing authenticated text gateway; preserve its access boundary |
| Authentication | Unauthenticated gateway model listing and chat POST return 401 | Auth enforcement exercised; authenticated client flow not tested |
| ASR | No managed speech service identified; backend OpenAPI has no audio transcription route; gateway POST `/v1/audio/transcriptions` returns 404 | Missing from the inspected deployment |
| TTS | No managed speech service identified; backend OpenAPI has no speech route; gateway POST `/v1/audio/speech` returns 404 | Missing from the inspected deployment |
| Duplex/realtime | Gateway exact allowlist has no WebSocket/realtime or audio path; realtime POST returns 404 | Session service does not exist at this boundary |
| Admission/concurrency | Active alias has gateway admission capacity 8; runtime recipe sets `--max-num-seqs 8` | Configuration observed; simultaneous speech/text throughput and latency unmeasured |
| Cancellation | Gateway `proxy` races client disconnect against response forwarding, then aborts upstream transport and releases admission in `finally` | Source-inspected disconnect behavior; no tested turn cancellation acknowledgement or GPU retirement bound |

The installed gateway's exact allowlist is GET `/v1/models`, GET `/v1/hs/key`,
POST `/v1/chat/completions`, and GET `/hs-gateway/health` and
`/hs-gateway/identity`. It does not proxy arbitrary backend paths. The backend's
OpenAPI advertises Responses cancellation, but the gateway does not expose it;
an advertised backend route is not evidence of usable client cancellation.

The first smoke request delivered visible text in about 188 ms and completed in
about 256 ms, with three data frames. This is a single short loopback observation,
not an inference latency distribution, remote-client measurement or product
target. It bypasses the gateway for backend diagnosis only. A shipping client
must use the authenticated interface, not expose or depend on the backend port.

The installed gateway source also contains per-principal stream limits, a
bounded fair admission queue, queue timeout, token limits, disconnect handling
and incomplete-SSE rejection. Those mechanisms are useful existing substrate;
their presence does not prove mixed ASR/LLM/TTS scheduling or end-to-end speech
cancellation. No concurrency benchmark, credential creation, drain operation or
service reconfiguration was performed. Unmanaged applications and a second node
were outside this inspection, so this report makes no claim that no speech
software exists anywhere on the machines.

## Concrete integration direction

The subsequent [incremental transcription investigation](incremental-transcription-investigation.md)
distinguishes microphone input streaming from transcript output streaming and
records the installed realtime adapter's five-second segmentation and unbounded
incoming queue. It recommends comparing Parakeet/NeMo buffered streaming with
Whisper/SimulStreaming in a separate speech runtime before headset integration.

Use the existing authenticated text-chat gateway as the conversation backend.
Add a separately specified duplex session service above the inference services,
with an external native client adapter above Hail. The current gateway should
not become an unrestricted backend-path proxy to obtain speech endpoints.

The next deliverable is **a speech-service capability proof**:

1. Select and declare an ASR runtime/model and TTS runtime/model for this node.
   Establish supported languages/voices, accepted/output formats, incremental
   versus completed-utterance behavior, cancellation and resource budgets.
2. Exercise them with a small licensed or owner-approved synthetic speech
   fixture. Retain timings, exact format/framing, partial/final transcript
   semantics, first-audio behavior, cancellation and bounded memory evidence.
   No microphone is required for this step.
3. Verify authenticated text streaming through the actual client ingress using
   a scoped credential. Do not create or disclose a credential as part of
   capability inspection.
4. Run a small mixed-workload check: ASR stays serviced while text generation
   and TTS run, and cancelling one response releases its work. Eight configured
   text sequences do not establish this property.

Choose wire audio formats only after these facts are established. Preserve
native device format and quantum independently; client conversion/framing stays
outside Hail. Do not assume the model's sample rate equals the device rate or
that an audio HTTP route implies incremental ASR or streaming synthesis.

The session contract must then define session/turn/response IDs, ordered audio
sequences, declared format/framing, input speech events, transcript revisions and
finals, output text/audio, cancellation acknowledgement, completion/errors,
bounded backpressure and reconnect behavior. On interruption the client
immediately flushes local playback, preserves capture, clears response-specific
buffers and rejects late chunks by response ID before waiting for remote
cancellation. Hail generation and clock epochs retain their separate meanings.

Acceptance for milestone 3 remains one headset conversation with continuous
capture during playback and a verified interruption. Speaker echo cancellation,
physical latency qualification and multi-session operation follow separately.
No model download, deployment, public API/ABI change or speech/DSP/network
dependency was added to Hail in this assessment.
