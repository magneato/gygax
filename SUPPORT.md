# Support

- Bugs and feature requests: open a GitHub issue using the templates.
- Questions: GitHub Discussions, or an issue labelled `question`.
- Security problems: see [SECURITY.md](SECURITY.md); do not file public issues.

Before filing a bug run `gygax doctor` and, for a running service, `scripts/collect-diagnostics.sh`. The bundle redacts tokens, IP addresses, host names and user names; please still skim it before attaching.

Useful details: `gygax version`, OS and compiler, how you started the service (flags and `GYGAX_*` variables with secrets removed), and the failing request with its `X-Request-Id`.
