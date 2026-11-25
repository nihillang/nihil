# Security

Please report vulnerabilities privately, not in a public issue: either with
**Report a vulnerability** in the repository's
[Security tab](https://github.com/nihillang/nihil/security/advisories/new), or by
email to **security@nihil-lang.dev**. Include the program or input that shows the problem, the version
(the commit) and what you expected to happen.

You will get an answer within a few days. Once a fix is ready, the report is
credited in the commit that fixes it, unless you prefer otherwise.

Two things are by design, not vulnerabilities:

- **The IDE server runs any code it receives.** It listens only on `127.0.0.1`;
  reach it from another machine through an SSH tunnel.
- **A Nihil program has the permissions of the user who runs it**: reading,
  writing and deleting files, network and processes.
