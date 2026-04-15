# Issue Hierarchy & Workflow — EOP

## Structure

```
Milestone (M1, M2, M3, M4)
└── Epic (E1, E2, ...)                    ← [EPIC] template
    ├── User Story (US-101, US-102, ...)   ← [US-XXX] template
    │   ├── Sub-Task (implementation)      ← [TASK] template
    │   ├── Sub-Task (tests)               ← [TASK] template
    │   └── Sub-Task (docs)                ← [TASK] template
    └── User Story (US-103, ...)
        ├── Sub-Task
        └── Sub-Task
```

## Mapping to Project

| Level | GitHub Artifact | Template | Example |
|-------|----------------|----------|---------|
| **Milestone** | GitHub Milestone | (native) | `M1 — Core Communication Layer` |
| **Epic** | Issue + `epic` label | `01-epic.md` | `[EPIC] E1 — C++ TCP Server` |
| **User Story** | Issue + `user-story` label (sub-issue of Epic) | `02-user-story.md` | `[US-101] Node connection and welcome handshake` |
| **Sub-Task** | Issue + `sub-task` label (sub-issue of Story) | `03-sub-task.md` | `[TASK] Implement socket acceptor with RAII` |
| **Bug** | Issue + `bug` label | `04-bug.md` | `[BUG] fd leak on concurrent disconnect` |
| **ADR** | Issue + `adr` label | `05-adr.md` | `[ADR] ADR-002 — Thread Pool Concurrency` |

## Linking

GitHub sub-issues are created by using the "Create sub-issue" feature or by adding
task list items (`- [ ] #123`) in the parent issue body. This creates a trackable
parent-child relationship visible in the project board.

```
Epic #10: [EPIC] E1 — C++ TCP Server
  body contains:
    - [ ] #11 US-101 — Node connection and welcome handshake
    - [ ] #12 US-102 — Concurrent multi-client support
    - [ ] #13 US-103 — Node registration
    - [ ] #14 US-104 — Node disconnection detection

Story #11: [US-101] Node connection and welcome handshake
  body contains:
    - [ ] #15 [TASK] Design socket acceptor and connection handler interfaces
    - [ ] #16 [TASK] Implement SocketAcceptor with bind/listen/accept
    - [ ] #17 [TASK] Implement welcome handshake (version, timestamp, session_id)
    - [ ] #18 [TASK] Add EOP_PORT and EOP_IDLE_TIMEOUT configuration
    - [ ] #19 [TASK] Write integration test: 100 iterations connect/handshake/disconnect
    - [ ] #20 [TASK] Add connection logging (timestamp, client IP, session ID, active count)
```

## Milestones Setup

Create these 5 milestones in GitHub:

| Milestone | Title | Due Date (relative) |
|-----------|-------|---------------------|
| M0 | Product Discovery | End of Week 2 |
| M1 | Core Communication Layer (v0.1) | End of Week 5 |
| M2 | Processing Capability (v0.2) | End of Week 9 |
| M3 | Product API & Integration (v0.3) | End of Week 12 |
| M4 | Edge & IoT Integration (v1.0) | End of Week 16 |

## Labels Setup

Create these labels in the repository:

### Type Labels
| Label | Color | Description |
|-------|-------|-------------|
| `epic` | `#6F42C1` | High-level feature epic |
| `user-story` | `#0075CA` | User story with acceptance criteria |
| `sub-task` | `#A2EEEF` | Implementable unit within a story |
| `bug` | `#D73A4A` | Something isn't working |
| `adr` | `#F9D0C4` | Architecture Decision Record |

### Priority Labels (SW Agreement Sec 6.3)
| Label | Color | Description |
|-------|-------|-------------|
| `priority: urgent` | `#B60205` | Must be resolved immediately |
| `priority: high` | `#D93F0B` | Direct impact on progression |
| `priority: medium` | `#FBCA04` | Important, doesn't block |
| `priority: low` | `#0E8A16` | Minor or non-urgent |

### Size Labels (SW Agreement Sec 6.4)
| Label | Color | Description |
|-------|-------|-------------|
| `size: XS` | `#C2E0C6` | < 1 day |
| `size: S` | `#A2D5AC` | 1–2 days |
| `size: M` | `#7BC67E` | 3–5 days |
| `size: L` | `#4CAF50` | 6–10 days |
| `size: XL` | `#2E7D32` | > 10 days |

### Status Labels
| Label | Color | Description |
|-------|-------|-------------|
| `needs-triage` | `#E4E669` | New issue, needs classification |
| `blocked` | `#B60205` | Blocked by dependency |
| `design` | `#BFD4F2` | In design phase |
| `needs-review` | `#FBCA04` | Ready for code review |

### Component Labels
| Label | Color | Description |
|-------|-------|-------------|
| `component: server` | `#1D76DB` | C++ TCP server |
| `component: client-lib` | `#1D76DB` | C client library |
| `component: hpc` | `#1D76DB` | HPC engine |
| `component: gateway` | `#1D76DB` | Go API gateway |
| `component: ingestor` | `#1D76DB` | Go edge ingestor |
| `component: firmware` | `#1D76DB` | ZephyrOS firmware |
| `component: k8s` | `#1D76DB` | Kubernetes/Helm |
| `component: monitoring` | `#1D76DB` | Observability stack |

## Workflow Status (SW Agreement Sec 6.1)

The **Workflow Status** custom field in the GitHub Project board defines the
real execution state. This is the authoritative status, not the native GitHub
"Open/Closed" state.

```
Triage → Backlog → In Progress → Pending Review → In Review →
On Hold → Pending Final Review → In Final Review → Done
```

## Branch Naming (SW Agreement Sec 3.2)

| Issue Type | Branch Pattern | Example |
|------------|---------------|---------|
| User Story / Sub-Task (feature) | `feature/<issue>_<desc>` | `feature/16_socket-acceptor` |
| Bug | `fix/<issue>_<desc>` | `fix/45_fd-leak-disconnect` |
| Critical fix | `hotfix/<issue>_<desc>` | `hotfix/78_sigterm-crash` |
| Release | `release/<semver>` | `release/0.1.0` |

## Commit Convention (SW Agreement Sec 3.1)

```
<type>(<scope>): <description>

[optional body]

[optional footer: Refs #<issue>]
```

Types: `feat`, `fix`, `refactor`, `test`, `docs`, `ci`, `chore`
Scopes: `server`, `client`, `hpc`, `gateway`, `ingestor`, `firmware`, `k8s`, `monitoring`

Examples:
```
feat(server): implement socket acceptor with RAII wrapper

Refs #16

fix(client): handle partial recv in readExact loop

The previous implementation assumed recv() returns the full
payload in a single call, which fails under load.

Refs #45
```
