# Research v1 contract lock

## Status

The Maple evidence format and capture transaction described by
`MapleTraceV1.md` and `CaptureTransactionV1.md` are frozen v1 contracts. A
change that alters accepted bytes, package membership, required fields, field
meaning, or validation semantics must use a new version and validator path.
It must not rewrite v1 in place.

The unit test `ResearchMapleTrace.V1BinaryContractRemainsByteExact` generates
the deterministic four-event reference trace and requires this complete-file
SHA-256:

```text
08f5d03624936e03c601ea3697db182f09390489fe489102720adb743526e4b8
```

The capture transaction test independently locks the exact bytes of the five
v1 JSON schemas:

| Schema | SHA-256 |
| --- | --- |
| `flycast-research-capture-job-v1.schema.json` | `c045487db37d60df9ed409429d6ca70da5704ddc7c3277506ddc8d0b724e5314` |
| `flycast-research-capture-transcript-v1.schema.json` | `b11b5ab950d89308d560595b2512b8708ac3c84c244f44b1b7882c4551dff3ea` |
| `flycast-research-capture-validation-v1.schema.json` | `740f49022cd3d0f55c86cd997a8d8e11fc0032625ddf1daaff055a451211d805` |
| `flycast-research-identity-v1.schema.json` | `07cbd4a368806dff4fff28b1c1bc1f519539b1b2abf63afbe65fa6d3072b8b58` |
| `flycast-research-quarantine-v1.schema.json` | `254f4f648a943f3952d98fb4c64c7d89394824595d7ca6e16a816866d8f2c51f` |

Capture transaction v1 remains the exact five-entry package below. It accepts
one Maple artifact and no additional evidence artifact:

```text
job.json
identity.json
maple.fcmr
transcript.json
capture-validation.json
```

SH-4, PowerVR, GD-ROM, or multi-artifact publication therefore requires a v2
package (or another separately versioned typed package); it cannot be added to
capture transaction v1.
