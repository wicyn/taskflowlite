# TaskflowLite overview image

Target: [taskflowlite-overview.png](taskflowlite-overview.png), embedded in both repository READMEs.

Edited with the built-in imagegen tool from the previous overview image to add the
Coroutine task form and CoroToken. API semantics follow the current
[coroutine guide](../coroutines.md). This file records the editing prompt for future updates.

## Editing prompt

```text
Use case: infographic-diagram / precise edit.
Asset type: the README overview diagram for the TaskflowLite C++20 header-only task library.
Image 1 is the edit target. Update this existing technical diagram to include the library's Coroutine task form, preserving its recognizable visual style and every existing task form.

Keep the white/off-white background, navy text, blue normal edges, orange conditional/jump edges, violet context/module boxes, fine outlines, restrained shadows, and clean readable sans-serif typography. Preserve the TaskflowLite brand heading and "C++20 / Header-only". Produce a sharp high-resolution landscape image around 2560 x 1280. Reflow and widen the central composition as needed to give the added section ample room. No cropped labels, overlapping text, or connectors crossing boxes/text. Keep text large enough to read in a README.

Preserve the original graph's topology and labels:
entry [Placeholder] -> load [Basic] -> dispatch [MultiBranch].
Dispatch output 0 goes to pipeline [Module]; output 1 goes to dynamic [SubFlow].
Inside pipeline: read [Basic] -> check [Branch]; check output 0 -> done [Basic]; check output 1 -> retry [Jump]; retry's orange dashed arrow goes back to read.
Inside dynamic: extract [Basic] fans out to map [Basic] and filter [Basic], both merge into join [Placeholder].
Both pipeline and dynamic connect to aggregate [Runtime]. Inside aggregate keep TaskGroup, local scope, stats [Basic], summary [Basic], and "group.wait()" with a check mark. TaskGroup belongs only inside Runtime.
To the right preserve scatter [MultiJump], dashed orange output 0 to write_db [Basic], output 1 to write_cache [Basic], and both write tasks connect to finish [Basic].

THE IMPORTANT EDIT:
Insert a clearly visible new teal-outline panel BETWEEN aggregate and scatter, and route the blue main dependency chain aggregate -> async_step -> scatter. Title the new panel "async_step", subtitle "[Coroutine]", then "CoroTask<R>".
Inside the panel show three clean stacked callout rows, with precisely these texts:
"co_await coro.call(...)" / "nested coroutine"
"co_await coro.fork(...)" / "submit child work"
"co_await coro.join()" / "wait for children"
The code labels and short explanations must all be legible and correctly spelled. Rows should visually communicate sequential steps. fork submits child work; join waits for children. Do not show blocking get()/wait() inside the Coroutine.
Outside and below this panel place a small teal badge "I/O completion" with a thin teal dashed arrow up to the Coroutine panel, labeled exactly "CoroToken::resume()". This depicts external resumption, separate from ordinary blue task dependencies.

BOTTOM LEGEND:
Keep the thin dividing rule and the two headings.
Change the exact heading from "Task forms (9)" to "Task forms (10)".
Show exactly these ten evenly spaced task-form symbols and labels, each once:
Placeholder, Basic, Branch, MultiBranch, Jump, MultiJump, Runtime, Module, SubFlow, Coroutine.
Keep the original respective outlines/shapes; Coroutine is a teal rounded rectangle matching the new panel. Arrange them as a compact single row with room for labels.
Under "Execution tools", preserve AsyncTask, AsyncFuture, Semaphore with "acquire / release", TaskObserver; add "CoroToken" with a simple resume/play icon.
Maintain generous footer margins and a clear separation between task forms and execution tools. No extra task forms or tool categories.
Technical invariants: Coroutine is the tenth task form; TaskGroup is a Runtime scope, not a task form; CoroToken is an execution tool; all existing control-flow edges must stay intact.
```
