# ImRAD support header provenance

Audited on 25 October 2026 against the author's public repository, without account access.

- Author/project: [Tomas Pecholt, tpecholt/imrad](https://github.com/tpecholt/imrad).
- Pinned upstream commit: `cc85b514e201fac2fe1083e8aa1cc371a7c90ba1`.
- Corresponding header: [src/imrad.h at that commit](https://github.com/tpecholt/imrad/blob/cc85b514e201fac2fe1083e8aa1cc371a7c90ba1/src/imrad.h).
- Local `imrad.h` SHA-256: `67e07046aa3cb262dafa3fe80a8c0d22883fe6ec4409da68ef1ce0edce749d80`.
- Upstream header SHA-256: `259a037cb46ae05cde0895cb9eb9a95879832ca0d2f1c7fca43ddd9ee1bc27c2`.

The local and pinned upstream headers are identical after replacing CRLF line endings with LF. The differing byte hashes reflect line endings, not a claimed code change. No embedded version macro establishes a release number; the `release-0.9.1` header was checked and does not match this copy. Use the pinned commit to identify this header.

## Upstream license context and header exception

The upstream project's [LICENSE](https://github.com/tpecholt/imrad/blob/cc85b514e201fac2fe1083e8aa1cc371a7c90ba1/LICENSE) contains GNU GPL version 3. The adjacent local `LICENSE` preserves that file verbatim as project licensing context. Its SHA-256 is `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` (35,149 bytes).

The author's [README license section at the same commit](https://github.com/tpecholt/imrad/blob/cc85b514e201fac2fe1083e8aa1cc371a7c90ba1/README.md#license) distinguishes the GPL-licensed ImRAD tool from its generated output and explicitly excludes the accompanying `imrad.h` support header from GPL. It also describes use of generated code in open-source or commercial projects under the user's chosen license.

This provenance note preserves that upstream distinction. It does not assign an MIT license, impose GPL-only terms on the exempt header, or relicense `imrad.h`. The adjacent GPL text must be read with the author's linked header exception, rather than as a replacement for it. RepublicMP's own license is documented separately in the repository root.
