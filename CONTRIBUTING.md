# Contributing to Nihil

Thank you for your interest. Nihil is small on purpose: every feature has to
earn its place, and [SPEC.md](SPEC.md) (with its Appendix A, which records why
each decision was made) is the reference for the language.

## Before a pull request

- **Open an issue first** for anything beyond a small fix, so the design can be
  discussed before the code.
- **Stage 0 is frozen.** New features go only into stage 1 (`src1/`), and the
  source of stage 1 itself stays free of them, so stage 0 can still compile it
  (SPEC §17). That includes ARRAY of BOOL: stage 0 still makes it with 8-byte
  elements, so `src1/` keeps such flags in ARRAYs of INT (0 or 1), and stage 1
  built by stage 0 stays byte for byte the one built by itself.
- **The checks must pass** (GitHub Actions runs them on every push):

  ```sh
  make test test1 test1-O0 compare fixedpoint floatcheck
  ```

- **Add a test** in `tests/stage1/` for a new feature or a fixed bug: the program
  and its expected `.out` (or a `tests/stage1/errors/` case with its `.err`).
- **Update SPEC.md** when the language changes, including a line in the version
  history.

## Style

Comments explain why, in plain English. Library code included in other
programs names its locals with a `__` prefix (SPEC §18).

## License

By contributing, you agree that your contributions are licensed under the MIT
License of the project.
