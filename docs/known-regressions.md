# Known regressions

Open issues *outside this repository* that block or constrain rongo
against the current kaikai release. Each entry names the upstream
artefact, the observed symptom, the workaround in rongo (if any), and
whether it blocks the test suite.

This file follows ahu's and kohau's convention: bugs found outside
rongo's lane are documented here, not fixed inline. rongo does not
patch kaikai.

## kaikai 0.123.0

### A `String` forwarded through a call is freed at the call site

**Symptom.** A `String` that a function hands to another function,
which in turn consumes it, is released against the *caller's* binding
as well. Reading that binding afterwards reads freed memory: the
program returns nonsense lengths, or aborts (SIGTRAP / SIGABRT).

Minimal reproduction — no FFI, no recursion, no dependency:

```kaikai
# `eq` consumes `needle` in a string comparison.
fn eq(hay: String, needle: String) : Bool = string_slice(hay, 0, 4) == needle

# `find` only forwards its own parameter to `eq`.
fn find(hay: String, needle: String) : Bool = eq(hay, needle)

fn main() : Int / Stdout {
  let sep = "abcd"
  print("eq=#{find("abcdyyy", sep)}")
  print("len=#{string_length(sep)}")   # sep is already freed here
  0
}
```

```
$ kai run .
eq=true
[exit 133]
```

One level of forwarding is what breaks it. Calling `eq` directly from
`main` with the same arguments is correct and does not crash, and the
recursive `find_loop` shape rongo actually uses is fine on its own —
it is the intermediate function that loses the reference.

**Impact on rongo — severe, and silent.** Both HTTP framing
predicates walk a header block with `find` and then read the lengths
of the strings they just passed to it:

- `http_server.request_complete` never reported a request with a body
  as complete, so the server read forever and the connection hung.
- `https.response_complete` did the same on the client side, so a
  `session_get` against any server never returned.

The unit tests caught the first as a plain assertion failure. The
second passed its own suite untouched — `tests/test_https_url.kai`
covers URL parsing, not response framing — and only showed up as a
deadlock in `examples/http_server`.

**Workaround applied.** Both predicates now read `string_length` of
the subject and the separator *before* handing either to `find`, and
use the saved values afterwards. This restores correct behaviour on
0.123.0 and is harmless on a fixed compiler, but it is load-bearing
statement order: do not reorder those `let`s, and prefer reading a
string's length before passing it on anywhere in this package until
the upstream fix lands.

**Blocks the suite.** No, not any more — `make test` passes 25/25 and
`examples/http_server` completes its keep-alive + crash-isolation
roundtrip with the workaround in place. Without it, 1 test fails and
both the server and client lanes deadlock at runtime.
