# Known regressions

Open issues *outside this repository* that block or constrain rongo
against the current kaikai release. Each entry names the upstream
artefact, the observed symptom, the workaround in rongo (if any), and
whether it blocks the test suite.

This file follows ahu's and kohau's convention: bugs found outside
rongo's lane are documented here, not fixed inline. rongo does not
patch kaikai.

## kaikai 0.174.0

### A test that dies of an unhandled `Cancel` passes the run

**Symptom.** When a `test` block lets a `Cancel.raise` escape, the test
binary prints `kai: Cancel.raise: unhandled (fiber cancelled)` and stops:
the remaining blocks of that file never run, the file reports no
summary, and `kai test` still exits 0. A trap (division by zero, say)
exits 1 as it should; only the cancellation is lost.

```kaikai
fn boom() : Int / Cancel = Cancel.raise()

test "crash" {
  assert boom() == 1
}

test "after" {
  assert 1 == 2   # never runs
}

fn main() : Int = 0
```

```
$ kai test a_test.kai; echo $?
kai: Cancel.raise: unhandled (fiber cancelled)
0
```

`--json` behaves the same: no record for either block, exit 0.

**Impact on rongo.** The live tests run servers inside a nursery, and a
nursery re-raises a child's cancellation out of its scope, so a server
fiber that unwinds would end the file as a pass. `kai mutate` judges a
mutant by the same exit code, so a mutant that makes something unwind
would be reported as a survivor that nothing noticed — or worse, a
broken suite would look green.

**Workaround.** No test lets a `Cancel` reach the runner: a setup step
that cannot go on exits the process with status 1 (`process.exit`)
instead of raising, and the HTTP server's tests — whose handler raises
on purpose — run their nursery under a `Cancel` handler that turns an
unwind into a value the test asserts on.
The suite does not depend on the fix, but new tests must keep to this.

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

**Workaround, since removed.** Both predicates read `string_length`
of the subject and the separator *before* handing either to `find`,
and used the saved values afterwards. That restored correct behaviour
on 0.123.0 at the cost of load-bearing statement order. It was
reverted once the fix shipped; the sources read in their natural
order again.

**Status.** Resolved upstream in kaikai 0.124.0 by
[lnds/kaikai#2099](https://github.com/lnds/kaikai/pull/2099), *fix
(perceus): resolve a bare callee's borrow convention per calling
module*, filed from here as
[lnds/kaikai#2095](https://github.com/lnds/kaikai/issues/2095).
Verified on the released binaries with one clean build each: the
reproduction above aborts on 0.123.0 and prints `len=4` on both
0.124.0 and 0.124.1. With the workaround reverted, `make test` passes
25/25 and `examples/http_server` completes its keep-alive +
crash-isolation roundtrip on 0.124.1.

Nothing in this package works around it any more, so **0.124.0 is the
floor**: on 0.123.0 both the server and the client lanes deadlock at
runtime.
