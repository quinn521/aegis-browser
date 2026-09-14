# Aegis model workflow

These model-selection rules apply to tasks in this repository.

| Task | Model and reasoning effort |
| --- | --- |
| Normal daily implementation | `gpt-6-astra`, `low` |
| Medium or larger task involving module boundaries, interfaces, or architectural uncertainty | Design with `gpt-6-astra`, `high`, then implement with `gpt-6-astra`, `low` for normal work or `medium` for complex work |
| Complex state or cross-module implementation | `gpt-6-astra`, `medium` |
| Repeated focused debugging does not converge | Escalate to `gpt-6-astra`, `medium`, and revisit the design |
| High-risk design or review | Consider `gpt-6-astra`, `xhigh` |
| Required independent review | Separate fresh-context `gpt-6-astra`, `high` |
| Review after fixes | Retain the same reviewer's context and bind re-review to the final HEAD |

- Routine low-risk changes need no separate Astra architecture design.
- Keep fixes with the original implementer; the independent reviewer verifies them.
  Give the initial reviewer the requirements, applicable design, base/head SHAs,
  diff, and original verification evidence without inheriting the implementer's
  discussion. Recheck fixes and affected paths against the final HEAD.
- These rules do not switch models automatically. Explicitly select both the model
  and reasoning effort using the available controls. If unavailable, disclose the
  actual execution model, any unknown effort, and the unmet routing stage.
- Terra and Luna are not designated daily default models by this file.
- Questions, read-only checks, and planning have no mandatory model; retain the
  current session model by default. The implementation design stage in the table
  uses its specified model.
- Model review does not replace tests or CI. The final HEAD must pass the required
  review and hosted CI; earlier-HEAD results cannot substitute for final validation.
  These rules do not grant merge or release authorization.
