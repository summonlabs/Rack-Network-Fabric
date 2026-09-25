# Contributing to Rack Network Fabric

Rack Network Fabric is developed by Summon Software Labs and released under the
[Apache License, Version 2.0](LICENSE).

## License terms for contributions

By submitting a contribution to this project you agree that your contribution is
licensed under the Apache License, Version 2.0, without any additional terms or
conditions, as described in section 5 of that license ("Submission of
Contributions"). You keep the copyright to your contribution.

**There is no Contributor License Agreement (CLA) to sign.** You do not assign
copyright, and you do not need to file any paperwork. Opening a pull request is
the whole process.

If you are contributing on behalf of an employer, make sure you have permission
to do so. Do not submit code you did not write, or code that is licensed under
terms incompatible with Apache-2.0.

## Expectations for a change

1. **Build warning-clean.** The build uses `-Werror` / `/WX` with the warning sets
   defined in the root CMakeLists.txt. A change that adds a warning is not ready.
2. **Add tests.** Behavioural changes need a test in `tests/`. New invariants
   should be expressed as a property test with a deterministic seed rather than
   as a single hand-written example where that is practical.
3. **Do not weaken the proof surface.** If a change makes an invariant test
   vacuous, say so in the pull request; do not delete the invariant.
4. **Keep the boundaries honest.** Do not claim hardware, protocol, or
   multi-host behaviour that the repository cannot demonstrate. Mark simulated
   or modelled behaviour as such in code comments and documentation.
5. **Stay inside the rack scoped contract.** This runtime owns rack-scoped
   composition and authority. Physical discovery, switch programming, global
   bandwidth allocation, and cross-rack governance belong to adjacent runtimes,
   not to this one.
6. **No telemetry.** The runtime must not transmit data to any endpoint that the
   operator did not explicitly configure as a fabric endpoint.

## Style

* C++20, no compiler extensions.
* Four-space indent, 100 column soft limit, `snake_case` for functions and
  variables, `PascalCase` for types, trailing underscore for private members.
* Prefer returning `rnf::Result<T>` over throwing. Exceptions are reserved for
  `std::bad_alloc` and standard-library internals.
* Every public API must state its threading contract in a comment.

## Reporting a defect

Open an issue with the exact command, the observed behaviour, and the expected
behaviour. If the defect involves a failing property test, include the seed that
was printed with the failure.

## Security

Report suspected security defects privately to the maintainers rather than in a
public issue.
