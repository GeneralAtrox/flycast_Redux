# PowerVR semantic draw ownership v1

This slice separates renderer-independent TA primitive decoding from committed
host draws. A decoded strip is never evidence that a renderer call occurred.

## Native chain

```text
SH-4 instruction-owner token
    -> accepted 32-byte TA block
    -> material and ordered vertex block provenance
    -> decoded primitive generation
    -> committed raster generation
    -> render generation completion
```

Each TA context retains provenance beside its raw 32-byte blocks only while the
draw observation bus is active. The normal renderer fast path remains free of
provenance allocation. The common TA parser copies exact block references into
polygon strips, sprites and modifier volumes. A 64-byte parameter or vertex
retains both contributors even when the end-of-strip bit closes the primitive
before the second half is decoded.

`PrimitiveDecoded` is emitted before index generation and translucent sorting.
It contains the raw PCW/ISP/TSP/TCW material, list and render pass, original
vertex range, bounds, parameter blocks, vertex blocks and one of these owner
classes:

- `Exact`: every contributing block has the same complete SH-4 owner token;
- `Mixed`: contributors have different owners or an unavailable owner; or
- `Unowned`: no TA block owns the primitive.

The hardware-generated background polygon is always `Unowned`. It never
inherits the most recently decoded guest primitive.

A save state can restore a renderer-ready TA context whose original TA packets
predate observation. The selected address is retained, but that context and
its ordinary host draws are not assigned causal generations. Recording
continues until packets accepted during the current run produce a complete
primitive and committed draw. This prevents a convenient save-state boundary
from fabricating ownership.

`DrawConsumed` is emitted only after the backend issues a host draw. It has a
monotonic raster generation and references every primitive contributing to the
call. This matters when translucent sorting merges materially equivalent
polygons or modifier-volume resolution consumes several volumes. Backend
internal modifier resolution may remain explicitly unowned; ordinary draws may
not.

## Artifact

`flycast-pvr-draw-v1` (`.fcpvrd`) is a bounded binary artifact. Its header binds
the CPU backend, identity, Maple replay, raw TA artifact, PVR presentation
artifact and canonical renderer configuration. The independent binary decoder
checks framing, SHA-256, CRC, limits, event order, exact block-owner tokens,
context/pass joins, owner classification, primitive/raster generations and
successful render completion.

A complete artifact requires a successful render containing an actual committed
draw that references a non-background guest primitive. A background-only boot
frame cannot qualify the slice.

## Production admission

The native primitive boundary, DX11 committed-draw hooks, typed writer,
independent raw-TA semantic decoder, graph validator, validator CLI and atomic
package publisher are implemented. Set `research:PvrDrawRecord` together with
the required TA and presentation record paths. A clean stop finalizes TA and
presentation first, hashes those exact files, binds their hashes and the fixed
renderer configuration into the draw artifact, and then finalizes the draw
artifact. Dropped observations, renderer drift, aborts and unbound linked
artifacts leave the draw candidate incomplete.

The package publisher stages the exact identity, Maple replay, TA manifest, TA,
presentation and draw artifacts, validators, publisher and (when declared) the
authenticated initial save state. Independent validation occurs inside a
private sibling directory before one atomic rename exposes the accepted path.
Forced aborts after staging and after validation never expose that path.

The Lodoss Slot 1 qualification uses an authenticated 120-DMA replay. The
interpreter and research dynarec match all 96,776 SH-4 semantic events. The
same interpreter identity then produces 40 completed PowerVR renders and a
validated non-background draw reconstructed independently from the raw TA
artifact. Both the base TA package and final draw package revalidate after
publication.

Only DirectX 11 may claim this v1 semantic draw slice. Other renderer backends
remain outside this contract until they have equivalent committed-draw hooks.
