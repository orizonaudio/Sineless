# Security policy

## Supported versions

Security fixes go into the latest release only.

## Reporting a vulnerability

Report vulnerabilities privately through GitHub: open the repository's **Security** tab and
choose **Report a vulnerability**
([direct link](https://github.com/orizonaudio/Sineless/security/advisories/new)). Please do not
open a public issue.

Include what you found, how to reproduce it (a project or saved plugin state that triggers it
helps most), the Sineless version, the host and the operating system. You will get an answer
within a week; once a fix is released, the advisory is published with credit to you unless you
prefer otherwise.

## Scope

Sineless runs inside your DAW, so the most relevant issues are those an untrusted input can
trigger:

- plugin state loaded from a project file someone else made;
- the editor's embedded web page reaching anything other than its own bundled content.

Releases are not yet signed with an Apple Developer ID or a Windows code-signing certificate;
that is known and not a vulnerability report.
