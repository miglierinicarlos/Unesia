## Pull Request

### Summary

<!-- One-line description of what this PR does -->

### Related Issues

<!-- Link ALL related issues. Use "Closes" for issues this PR resolves. -->

Closes #<!-- issue number -->
Related to #<!-- issue number -->

---

### Type of Change

- [ ] `feat`: New feature (maps to a user story / sub-task)
- [ ] `fix`: Bug fix
- [ ] `refactor`: Code restructuring (no behavior change)
- [ ] `test`: Adding or modifying tests
- [ ] `docs`: Documentation only
- [ ] `ci`: CI/CD configuration
- [ ] `chore`: Build system, dependencies, tooling

---

### Changes Made

<!-- Describe the specific changes. Be concise but complete. -->

### How to Test

<!-- Step-by-step instructions for the reviewer to verify this PR -->

1. <!-- Step 1 -->
2. <!-- Step 2 -->
3. <!-- Step 3 -->

---

### Checklist (SW Agreement Compliance)

#### Repository (Sec 3)
- [ ] Commit(s) are GPG-signed
- [ ] Commit messages follow Conventional Commits format
- [ ] One logical change per commit (atomic)
- [ ] Branch follows naming convention: `feature/`, `fix/`, `hotfix/`
- [ ] No binaries, libraries, logs, or temp files included

#### Code Quality (Sec 4)
- [ ] `clang-format` compliant (C/C++) or `gofmt` (Go)
- [ ] `clang-tidy` zero warnings (C/C++) or `go vet` + `staticcheck` (Go)
- [ ] No magic numbers — all literals are named constants
- [ ] No global variables
- [ ] All inputs validated and sanitized
- [ ] RAII for all resources (C++ only)

#### Documentation (Sec 4.4)
- [ ] Doxygen headers on new/modified `.hpp` files
- [ ] GoDoc comments on new/modified exported Go types
- [ ] Inline comments are meaningful, not redundant
- [ ] README updated if public interface changed

#### Testing (Sec 5)
- [ ] New tests added for new functionality
- [ ] All existing tests pass
- [ ] No trivial/superficial tests
- [ ] CI pipeline green

#### Sanitizers (Sec 4.1)
- [ ] AddressSanitizer: zero findings
- [ ] ThreadSanitizer: zero findings (if concurrency is involved)
- [ ] Go race detector: zero findings (if Go code modified)

#### Architectural Decision
- [ ] If this PR makes an architectural decision → ADR documented in `/docs/adr/`
- [ ] N/A — no architectural decisions in this PR

---

### Screenshots / Logs / Evidence

<!-- If applicable: test output, SigNoz traces, Grafana panels, etc. -->
