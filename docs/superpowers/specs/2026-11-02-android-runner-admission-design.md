# Android runner and source admission

The user authorized resolving Android runner and source admission while AAB10 continues. The baseline is 0994897e5b1689ab9fde0f14cd5c231ae9898723, tree 72497ac5f639835f677e770c4a772b5496778805, in the existing isolated Android delivery worktree.

Use the existing fork rehearsal model with an Android-only validation workflow. Keep upstream main/quality checks and all iOS/macOS behavior unchanged. A configured fork must pin its repository ID and exact source SHA, use the fixed `codex/android-release-acceptance` branch, and pass hosted contracts plus an unsigned Linux ARM64 native admission job at the latest exact run/attempt. Tags must also resolve to that source. Missing, skipped, duplicate, incomplete, stale or racing evidence rejects admission.

Provide one task-owned Linux ARM64 runner in a bounded container based on the already verified Android image. Install the official runner using its published SHA-256, without touching shared VM configuration or existing macOS runners. A host-installed, read-only job hook checks repository/ref/workflow/source pins before checkout and terminates only its verified owning worker on rejection. Native admission reads tools and uses temporary outputs; no signing credential is mounted. Packaging starts only after AAB10 releases its source/output lock and host reserves remain satisfied.

Alternatives considered: reusing macOS runners cannot run the Linux build; retaining the iOS archive prerequisite violates platform isolation. A dedicated Android fork rehearsal preserves the existing upstream boundary and makes the missing native prerequisite explicit. This is an implementation choice under the user's request to continue resolving these blockers.

Manual workflow registration still requires default-branch integration. This work does not change main/default branch or call a push rehearsal a manual dispatch. Service-account grants stay unchanged; Console Alpha internal remains the Play upload route. Contracts, native admission, real AAB, independent verification, Console/internal availability, Actions package and Tag Draft downloaded hashes remain distinct evidence.
