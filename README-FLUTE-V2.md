# FLUTE version 2 (RFC 6726) on this branch

This branch adds FLUTE version 2 to the library as a per-session choice, defaulting to version 1.
It is **not a complete RFC 6726 implementation**, and nothing here should be read as a claim that
it is. What is and is not covered is listed below.

## Why version 2 is a separate branch, and not an option on the main one

The two versions are different protocols, not a compatible upgrade. RFC 6726 clause 11.1:

> Therefore, an implementation that relies on [RFC3926] and RFC 3451 will not be backwards
> compatible with FLUTE as specified in this document.

3GPP does not use version 2. TS 26.346 clause L.4.1 references RFC 3926, and neither TS 26.346 nor
TS 26.517 references RFC 6726. The 3GPP MBMS Download Profile therefore lives entirely on the
version 1 branch, which carries none of the behaviour below: every difference here is reached only
by a caller that has selected version 2 explicitly.

## Selecting a version

`Transmitter::set_flute_version()` and `Receiver::set_flute_version()`. Both default to 1. The
selection reaches the ALC/LCT parser, the FDT, and the receiver's object handling.

## What this branch implements

- **Version signalling.** Transmit sets the EXT_FDT version field to the selected version, and
  receive accepts only the version the session was configured for, rejecting the other in both
  directions. RFC 6726 clause 3.4.1 requires the field to be 2 for a version 2 session.
- **The RFC 6726 LCT generation.** RFC 5651 removed the Sender Current Time and Expected Residual
  Time header fields; RFC 6726 clause 11.1 states that under RFC 5651 the two bits that carried
  them "MUST be set to zero and MUST be ignored by receivers". Under version 2 the parser gives
  them no length and steps over nothing, so an extension placed after the TOI is reached. Under
  version 1 the RFC 3451 reading is unchanged. The transmit side has always sent both bits zero.
- **The RFC 6726 FDT Instance ID sequence.** Version 2 wraps to the smallest expired identifier,
  refuses to reuse one that is still live, and reports exhaustion; version 1 keeps RFC 3926's
  wrap to zero.
- **Expires read in the correct NTP era.** RFC 6726 clause 3.3 has a receiver choose the epoch
  "for which the expiration time is closest in time to the current time". Applied on parse for
  version 2 only; RFC 3926 states no such rule.
- **Ordering of two TOIs sharing a Content-Location.** RFC 6726 clause 3.4.2 makes the declaration
  from the greater FDT Instance ID the newer one. The receiver's replacement sweep follows that
  under version 2 instead of keeping whichever completed first.
- **The RFC 6726 FDT namespace**, `urn:ietf:params:xml:ns:fdt`, from clause 3.4.2.

Covered by `tests/test_flute_v2.cpp`.

## Obligations checked and already met

These needed no code. They are recorded so nobody re-opens them.

- **EXT_TIME and EXT_AUTH.** RFC 5651 clause 5.2.1: senders and receivers "MUST recognize EXT_AUTH
  and EXT_TIME, but are not required to be able to parse their content." The parser has explicit
  cases for both and skips them by their declared length.
- **ALC, RFC 3450 to RFC 5775.** RFC 5775 clause 8's change list is almost entirely editorial. Its
  two substantive items are the Source Packet Indication bit, which the LCT header struct carries,
  and the definition of EXT_FTI, which is implemented. Read from the change list, not a full
  re-read of the RFC.
- **The FDT schema body.** The attribute set RFC 6726 clause 3.4.2 defines matches what the library
  emits; the 3GPP extension attributes and elements it also carries are admitted by that schema's
  own `xs:any namespace="##other"` and `xs:anyAttribute`, and the 3GPP-only `schemaVersion` element
  is not emitted for this namespace. Established by reading the schema, not by running an emitted
  document through a validator.
- **IPsec/ESP.** RFC 6726 clause 7.5 makes it mandatory to implement, taking its service set from
  RFC 5775: data origin authentication, content integrity and anti-replay SHALL be supported, and
  confidentiality is RECOMMENDED. Encryption and HMAC-SHA256 authentication are configured, and
  the association now sets a replay window.

## What is NOT implemented

**Congestion control.** RFC 5775: "Congestion control MUST be applied to all packets within a
session". The library has none, and the LCT Congestion Control Information field is sent as zero.
This is a building block, not a gap that can be patched, and until it exists **no RFC 6726
conformance claim can be made for this branch whatever else is in place**. Tracked as step 3 of
the issue this branch advances.

## If you are picking this up

Congestion control is the only remaining item, and it is the largest. Everything else listed above
is either implemented and tested, or checked against the specification and found already met.
