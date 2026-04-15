# Contributing Guide

Welcome to the **Exodus Systems Inc. Engineering Workflow**.

This repository follows a structured development model designed to simulate professional engineering environments.

---

# Development Workflow

All development must follow this branching model:

```

feature/* → development → main

```

## Branch rules

| Branch        | Role                  |
| ------------- | --------------------- |
| `main`        | Stable release branch |
| `development` | Integration branch    |
| `feature/*`   | Work branches         |

---

# Starting Work

Always start from `development`.

```bash
git checkout development
git pull
git checkout -b feature/my-feature
```

---

# Commit Guidelines

Use clear and descriptive commit messages.

Examples:

```
feat: add distributed cache layer
fix: resolve race condition in worker pool
docs: update architecture diagram
test: add integration tests for API
```

---

# Pull Requests

All changes must go through a Pull Request.

PRs must include:

- explanation of the change
- reference to the issue
- passing CI checks

---

# Code Review

At least **one approval** is required before merging.

Reviewers should verify:

- correctness
- code readability
- architectural consistency
- test coverage

---

# CI Requirements

Pull Requests cannot be merged unless CI checks succeed.

CI includes:

- build
- automated tests

---

# Engineering Culture

Teams are encouraged to follow best practices:

- small commits
- frequent pushes
- meaningful PR descriptions
- collaborative reviews
