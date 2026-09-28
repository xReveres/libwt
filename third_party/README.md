# Third-party code

`ls-qpack` and `xxhash` are private codec dependencies copied from the former
libwtf dependency tree at commit `9d0a45532d7894ad47a11d6acf329c74decbab31`.
Their source headers retain the complete license notices.

- ls-qpack: LiteSpeed Technologies, MIT; https://github.com/litespeedtech/ls-qpack
- xxHash: Yann Collet, BSD-2-Clause; https://github.com/Cyan4973/xxHash

MsQuic is a separate CMake dependency, pinned to commit
`a01333cf7c2659cce0ff03ef3f21e1ff15bb5b83` (v2.6.1) by default.
It is MIT licensed; see its `LICENSE` in the fetched source or installed package.
Its OpenSSL submodule has separate licenses and notices in that checkout.
