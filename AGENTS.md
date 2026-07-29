# Repository agent instructions

## Mandatory post-task review

After completing the implementation and local verification for every task, run
the `dev-fable5` skill once. One round means one skill invocation containing
its independent Claude Fable 5 and Claude Opus 5.0 reviews.

Treat every reviewer finding as a hypothesis, not as authority. Check it
against direct repository evidence and, where practical, reproduce it with a
focused test or diagnostic. Classify it as confirmed, refuted, or uncertain.
Do not change implementation code solely because a reviewer suggested it;
implement a review-driven change only after the underlying hypothesis has been
demonstrated by evidence. If the active request authorizes review but not
implementation, report the evaluated findings without making those changes.

Use the local skill at
`C:\Users\svjkr\.claude\skills\dev_loop_max\dev-fable5\SKILL.md` and follow its
two-review invariant without automatic retries or follow-up passes.
