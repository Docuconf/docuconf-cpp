# Security policy

## Reporting a vulnerability

Please report vulnerabilities privately, through GitHub's private vulnerability reporting: open the repository's
**Security** tab and choose **Report a vulnerability**
([direct link](https://github.com/docuconf/docuconf-cpp/security/advisories/new)). Do not open a public issue, pull
request or discussion for a suspected vulnerability.

Include what you can of:

- the affected version (release tag or commit SHA), compiler and OpenSSL version;
- what an attacker can do, and what they need first;
- steps or a minimal declaration, contract or program that reproduces it.

We work on the fix in a private security advisory, credit you in it unless you prefer otherwise, and publish the
advisory when a fixed release is out.

## Response targets

| | |
|---|---|
| Acknowledge the report | within 3 business days |
| First assessment (confirmed or not, severity) | as soon as we can reproduce it, and we keep you updated in the advisory |
| Fix | released as a patch to the supported version, then the advisory is published |

## Supported versions

docuconf-cpp is released as git tags `vX.Y.Z` with a source tarball (see [RELEASING.md](RELEASING.md)). Security
fixes go to the latest minor release, as a new patch release.

**During the beta, only the latest release is supported.** Upgrade to it to get a fix.

## Scope

In scope: the docuconf-cpp library in [`include`](include) and [`src`](src), for example a value marked `secret`
(including a key set's keys) that reaches an error message, a warning, the termination log or an exported
contract, a file input or overlay read from outside its declared path, or a value accepted at boot that the
contract rejects.

Out of scope: the example application under [`examples`](examples), vulnerabilities in dependencies (CLI11,
nlohmann/json, RE2, OpenSSL, yaml-cpp, toml++, json-schema-validator) that docuconf does not make reachable
(report those upstream), and issues in a platform or cluster that only arise from its own misconfiguration. The
docuconf CLI, the Go SDK, the Helm chart and the CUE meta-schema live in
[docuconf-go](https://github.com/docuconf/docuconf-go) and follow its policy; other language SDKs follow their
own.
